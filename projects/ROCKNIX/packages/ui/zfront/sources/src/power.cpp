#include "power.hpp"

#include <dirent.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>

#include "app.hpp"
#include "menu_system.hpp"
#include "recent.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace zf::power {

namespace {

std::string root() {
  const char* e = std::getenv("ZF_BATTERY_ROOT");
  return e ? std::string(e) : std::string();
}

std::string read_one(const std::string& path) {
  std::string s;
  if (!read_file(path, s)) return {};
  return std::string(trim(s));
}
long read_long(const std::string& path, long def = 0) {
  const std::string s = read_one(path);
  return s.empty() ? def : std::strtol(s.c_str(), nullptr, 10);
}

std::string battery_dir() {
  const std::string base = root() + "/sys/class/power_supply";
  if (is_dir(base + "/battery")) return base + "/battery";
  std::string found;
  if (DIR* d = ::opendir(base.c_str())) {
    while (const dirent* e = ::readdir(d)) {
      std::string n = e->d_name;
      if (n[0] == '.') continue;
      for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
      if (n.find("bat") != std::string::npos && (found.empty() || std::string(e->d_name) < found)) found = e->d_name;
    }
    ::closedir(d);
  }
  return found.empty() ? std::string() : base + "/" + found;
}

int brightness_now() {
  const std::string base = root() + "/sys/class/backlight";
  DIR* d = ::opendir(base.c_str());
  if (!d) return -1;
  int pct = -1;
  while (const dirent* e = ::readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const long mx = read_long(base + "/" + e->d_name + "/max_brightness");
    if (mx > 0) {
      pct = int(read_long(base + "/" + e->d_name + "/brightness") * 100 / mx);
      break;
    }
  }
  ::closedir(d);
  return pct;
}

long clock_secs(clockid_t id) {
  timespec t{};
  ::clock_gettime(id, &t);
  return long(t.tv_sec);
}

struct Entry {
  char kind = 'S';
  long boot = 0, mono = 0, uah = 0;
  int pct = 0, bright = -1;
  std::string name;
};

// One line: kind, boot clock, monotonic clock, charge, percent, brightness, name. Whatever does not parse is skipped.
bool parse_line(std::string_view l, Entry& e) {
  std::vector<std::string_view> f;
  size_t i = 0;
  while (i <= l.size() && f.size() < 7) {
    size_t j = f.size() == 6 ? std::string_view::npos : l.find('\t', i);
    if (j == std::string_view::npos) j = l.size();
    f.push_back(l.substr(i, j - i));
    i = j + 1;
  }
  if (f.size() < 6 || f[0].size() != 1) return false;
  e.kind = f[0][0];
  e.boot = std::strtol(std::string(f[1]).c_str(), nullptr, 10);
  e.mono = std::strtol(std::string(f[2]).c_str(), nullptr, 10);
  e.uah = std::strtol(std::string(f[3]).c_str(), nullptr, 10);
  e.pct = int(std::strtol(std::string(f[4]).c_str(), nullptr, 10));
  e.bright = int(std::strtol(std::string(f[5]).c_str(), nullptr, 10));
  if (f.size() == 7) e.name = std::string(f[6]);
  return e.pct >= 0 && e.pct <= 100;
}

constexpr size_t kMaxLog = 96u << 10;  // bytes; two weeks of normal use
constexpr long kSampleSecs = 600;
constexpr float kNominalVolts = 3.8f;

long g_last_sample = -100000;
bool g_in_game = false;
Now g_start_now, g_end_now;
bool g_have_session = false;

}  // namespace

Now read_now() {
  Now n;
  const std::string d = battery_dir();
  if (d.empty() || !file_exists(d + "/capacity")) return n;
  n.have = true;
  n.percent = std::clamp(int(read_long(d + "/capacity")), 0, 100);
  const std::string st = read_one(d + "/status");
  n.charging = st == "Charging" || st == "Full";
  const long uv = read_long(d + "/voltage_avg", read_long(d + "/voltage_now"));
  const long ua = read_long(d + "/current_avg", read_long(d + "/current_now"));
  n.volts = float(uv) / 1e6f;
  n.amps = ua < 0 ? float(-ua) / 1e6f : 0.0f;
  n.watts = n.volts * n.amps;
  n.charge_uah = read_long(d + "/charge_now");
  n.full_uah = read_long(d + "/charge_full");
  n.brightness_pct = brightness_now();
  return n;
}

float hours_left(const Now& n) {
  if (!n.have || n.charging || n.amps < 0.02f || n.charge_uah <= 0) return kNoHours;
  return std::clamp(float(n.charge_uah) / 1e6f / n.amps, 0.0f, 48.0f);
}

Summary summarize(const std::string& log, float screen_mw_50) {
  Summary out;
  std::vector<Entry> all;
  size_t i = 0;
  while (i < log.size()) {
    size_t j = log.find('\n', i);
    if (j == std::string::npos) j = log.size();
    Entry e;
    if (parse_line(std::string_view(log).substr(i, j - i), e)) all.push_back(std::move(e));
    i = j + 1;
  }
  if (all.size() < 2) return out;
  // since the last time it was full
  size_t first = 0;
  for (size_t k = all.size(); k-- > 0;)
    if (all[k].pct >= 99) {
      first = k;
      break;
    }
  std::map<std::string, std::pair<float, long>> games;  // lost, seconds
  float menus = 0, asleep = 0, screen = 0;
  long menus_s = 0, asleep_s = 0, screen_s = 0;
  float span_h = 0;
  std::vector<std::pair<float, int>> marks;
  marks.push_back({0.0f, all[first].pct});
  const float full_mah = 3000.0f;  // only for lines without a charge reading: 1 % of a usual battery
  for (size_t k = first; k + 1 < all.size(); ++k) {
    const Entry& a = all[k];
    const Entry& b = all[k + 1];
    const long dt = b.boot - a.boot;
    if (dt <= 0) continue;  // a reboot between the two: nobody knows what happened
    span_h += float(dt) / 3600.0f;
    marks.push_back({span_h, b.pct});
    float lost = 0;  // mAh
    if (a.uah > 0 && b.uah > 0) lost = float(a.uah - b.uah) / 1000.0f;
    else lost = float(a.pct - b.pct) * full_mah / 100.0f;
    if (lost <= 0) continue;  // on the charger
    const long slept = std::max(0L, (b.boot - b.mono) - (a.boot - a.mono));
    if (slept * 10 >= dt * 6) {
      asleep += lost, asleep_s += slept;
      continue;
    }
    const long awake = dt - slept;
    float scr = 0;
    if (a.bright > 0 && screen_mw_50 > 0) scr = std::min(screen_mw_50 * float(a.bright) / 50.0f / 1000.0f * float(awake) / 3600.0f / kNominalVolts * 1000.0f, lost * 0.6f);
    screen += scr, screen_s += awake;
    out.awake_secs += awake;
    if (a.kind == 'G' && !a.name.empty()) {
      auto& g = games[a.name];
      g.first += lost - scr;
      g.second += awake;
    } else {
      menus += lost - scr, menus_s += awake;
    }
  }
  const float total = menus + asleep + screen + [&] {
    float s = 0;
    for (const auto& g : games) s += g.second.first;
    return s;
  }();
  out.span_hours = span_h;
  out.used_pct = std::max(0, all[first].pct - all.back().pct);
  for (const auto& m : marks) out.curve.push_back({span_h - m.first, m.second});
  if (total <= 0.5f) return out;
  out.valid = true;
  auto add = [&](Row::Kind kind, const std::string& name, float mah, long secs) {
    if (mah <= 0) return;
    Row r;
    r.kind = kind, r.name = name, r.pct = mah * 100.0f / total, r.secs = secs;
    out.rows.push_back(std::move(r));
  };
  for (const auto& g : games) add(Row::Game, g.first, g.second.first, g.second.second);
  add(Row::Screen, {}, screen, screen_s);
  add(Row::Menus, {}, menus, menus_s);
  add(Row::Asleep, {}, asleep, asleep_s);
  std::stable_sort(out.rows.begin(), out.rows.end(), [](const Row& x, const Row& y) { return x.pct > y.pct; });
  return out;
}

std::string log_path() {
  if (const char* e = std::getenv("ZF_BATTERY_LOG")) return e;
  return "/storage/.config/zfront/battery.log";
}

void log_event(char kind, const std::string& name) {
  const Now n = read_now();
  if (!n.have) return;
  const std::string path = log_path();
  // trims itself: the older half goes (rewritten through a temp file: a power cut mid-trim must not leave a truncated log)
  std::string old;
  FILE* probe_f = std::fopen(path.c_str(), "rb");
  if (probe_f) {
    std::fseek(probe_f, 0, SEEK_END);
    const long sz = std::ftell(probe_f);
    std::fclose(probe_f);
    if (sz > long(kMaxLog) && read_file(path, old)) {
      size_t cut = old.find('\n', old.size() / 2);
      if (cut != std::string::npos) {
        write_atomic(path, old.substr(cut + 1));
      }
    }
  }
  FILE* f = std::fopen(path.c_str(), "ab");
  if (!f) {  // first time: nothing else in zfront makes this folder
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    f = std::fopen(path.c_str(), "ab");
    if (!f) return;
  }
  std::string nm = name;
  for (char& c : nm)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  std::fprintf(f, "%c\t%ld\t%ld\t%ld\t%d\t%d\t%s\n", kind, clock_secs(CLOCK_BOOTTIME), clock_secs(CLOCK_MONOTONIC), n.charge_uah, n.percent, n.brightness_pct, nm.c_str());
  std::fclose(f);
}

Summary summary_now() {
  std::string log;
  if (!read_file(log_path(), log)) return Summary();
  return summarize(log);
}

Session make_session(const Now& a, const Now& b, uint64_t secs) {
  Session s;
  if (!a.have || !b.have || a.charging || b.charging || secs < uint64_t(kMinSessionSecs)) return s;
  int drop10 = 0;
  if (a.charge_uah > 0 && b.charge_uah > 0 && a.full_uah > 0) drop10 = int(((a.charge_uah - b.charge_uah) * 1000L + a.full_uah / 2) / a.full_uah);
  else drop10 = (a.percent - b.percent) * 10;
  if (drop10 <= 0) return s;  // charged meanwhile, or too little to tell
  s.valid = true;
  s.drop_pct10 = std::min(drop10, 1000);
  return s;
}

Session take_session(uint64_t secs) {
  if (!g_have_session) return Session();
  g_have_session = false;
  return make_session(g_start_now, g_end_now, secs);
}

void game_started(const std::string& name) {
  g_start_now = read_now();
  g_have_session = false;
  log_event('G', short_title(name));
  g_in_game = true;
  g_last_sample = clock_secs(CLOCK_BOOTTIME);
}

void game_ended() {
  g_end_now = read_now();
  g_have_session = true;
  log_event('E');
  g_in_game = false;
  g_last_sample = clock_secs(CLOCK_BOOTTIME);
}

// ---- power saver ------------------------------------------------------------------------------------------------------------------------------

bool saver_on(Ctx& cx) { return msys::get(cx, "power.saver") == "1"; }
int saver_auto(Ctx& cx) { return std::atoi(msys::get(cx, "power.saver_auto", "0").c_str()); }
void set_saver_auto(Ctx& cx, int pct) { msys::put(cx, "power.saver_auto", std::to_string(pct)); }

void set_saver(Ctx& cx, bool on, bool automatic) {
  if (on == saver_on(cx)) return;
  const auto freq = [&](const std::string& what) { cx.sv->cmd.spawn("/usr/bin/sh -lc " + shell_quote(". /etc/profile.d/099-freqfunctions; " + what) + " >/dev/null 2>&1"); };
  if (on) {
    // what the user has now, to give it back
    msys::put(cx, "power.saved.cpugov", msys::get(cx, "system.cpugovernor", "default"));
    msys::put(cx, "power.saved.perfmode", msys::get(cx, "global.perfmode", "default"));
    msys::put(cx, "power.saved.brightness", msys::get(cx, "system.brightness", ""));
    msys::put(cx, "power.saved.screensaver", std::to_string(cx.es.get_int("ScreenSaverTime", 300000)));
    msys::put(cx, "system.cpugovernor", "ondemand");
    msys::put(cx, "global.perfmode", "0");
    freq("ondemand");
    const Now n = read_now();
    if (n.brightness_pct > 40) cx.sv->cmd.spawn("brightness set 40");
    const long ss = cx.es.get_int("ScreenSaverTime", 300000);
    // sooner, never later than what the user chose; "never" stays. Remembered, so that turning the saver off gives the time back only
    // when nobody changed it meanwhile
    msys::put(cx, "power.saved.ss_lowered", ss > 120000 ? "1" : "0");
    if (ss > 120000) cx.es.set_int("ScreenSaverTime", 120000);
    msys::put(cx, "power.saver", "1");
    msys::put(cx, "power.saver_byauto", automatic ? "1" : "0");
  } else {
    const std::string gov = msys::get(cx, "power.saved.cpugov", "default"), perf = msys::get(cx, "power.saved.perfmode", "default"), bright = msys::get(cx, "power.saved.brightness", "");
    msys::put(cx, "system.cpugovernor", gov);
    msys::put(cx, "global.perfmode", perf);
    if (gov != "default") freq(gov);
    else freq("ondemand");
    if (!bright.empty()) cx.sv->cmd.spawn("brightness set " + bright);
    // the key is missing when the saver came on with an older version: then the value is given back as before
    const std::string lowered = msys::get(cx, "power.saved.ss_lowered", "");
    if (lowered != "0" && (lowered.empty() || cx.es.get_int("ScreenSaverTime", 300000) == 120000))
      cx.es.set_int("ScreenSaverTime", std::atol(msys::get(cx, "power.saved.screensaver", "300000").c_str()));
    msys::put(cx, "power.saver", "0");
    msys::put(cx, "power.saver_byauto", "0");
  }
  if (!cx.es.save()) cx.sv->toasts.push(tr("Could not save the settings"));
}

void tick(Ctx& cx) {
  if (cx.sv->cmd.dry_run) return;
  const long t = clock_secs(CLOCK_BOOTTIME);
  if (t - g_last_sample < kSampleSecs || g_in_game) return;
  g_last_sample = t;
  log_event('S');
  const Now n = read_now();
  if (!n.have) return;
  const int at = saver_auto(cx);
  if (at > 0 && !n.charging && n.percent <= at && !saver_on(cx)) {
    set_saver(cx, true, true);
    cx.sv->toasts.push(tr("Power saver on"));
  } else if (n.charging && saver_on(cx) && msys::get(cx, "power.saver_byauto") == "1") {  // it came on by itself, and it goes the same way
    set_saver(cx, false);
  }
}

}  // namespace zf::power
