// PERFORMANCE SUGGESTIONS (autoperf.hpp): the sampler, the classifier, the per-game state and the menu rows.
#include "autoperf.hpp"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "i18n.hpp"
#include "menu_game.hpp"
#include "settings.hpp"
#include "uimode.hpp"
#include "util.hpp"

namespace zf::autoperf {

namespace {

// `read_file` sizes its buffer with ftell, and /proc/<pid>/stat and debugfs files report a size of 0: they would read as empty. This reads to EOF.
bool slurp_small(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[1024];
  size_t n;
  while (out.size() < 65536 && (n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}

bool read_trimmed(const std::string& path, std::string& out) {
  std::string t;
  if (!slurp_small(path, t)) return false;
  out = std::string(trim(t));
  return true;
}

// Hz in a file -> MHz, -1 when unreadable.
double read_mhz(const std::string& path) {
  std::string t;
  long v = 0;
  if (!read_trimmed(path, t) || !parse_int(t, v) || v <= 0) return -1;
  return double(v) / 1e6;
}

// First integer 0..100 of a file ("45", "45@600000000Hz", "utilization: 45"), -1 when none.
double parse_busy(const std::string& text) {
  size_t i = 0;
  while (i < text.size() && !(text[i] >= '0' && text[i] <= '9')) ++i;
  if (i >= text.size()) return -1;
  long v = 0;
  size_t digits = 0;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 6) {
    v = v * 10 + (text[i++] - '0');
    ++digits;
  }
  return v <= 100 ? double(v) : -1;
}

// kbase's dvfs_utilization prints cumulative counters, "busy_time N idle_time M" (herramientas/gpuprof.sh reads fields 2 and 4).
bool parse_counters(const std::string& text, long long& busy, long long& idle) {
  std::vector<std::string> tok;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\n' || text[i] == '\t')) ++i;
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\n' && text[j] != '\t') ++j;
    if (j > i) tok.push_back(text.substr(i, j - i));
    i = j;
  }
  long b = 0, d = 0;
  if (tok.size() < 4 || !parse_int(tok[1], b) || !parse_int(tok[3], d) || (tok[0].find(':') == std::string::npos && tok[0].find("busy") == std::string::npos)) return false;
  busy = b;
  idle = d;
  return true;
}

bool all_digits(const char* s) {
  if (!*s) return false;
  for (; *s; ++s)
    if (*s < '0' || *s > '9') return false;
  return true;
}

// /proc/<pid>/stat: ppid and utime+stime (read after the last ')' because the command name may hold anything).
bool parse_stat(const std::string& text, int& ppid, unsigned long long& ticks) {
  const size_t rp = text.rfind(')');
  if (rp == std::string::npos || rp + 2 >= text.size()) return false;
  std::vector<std::string> f;
  size_t i = rp + 2;
  while (i < text.size() && f.size() < 13) {
    size_t j = text.find(' ', i);
    if (j == std::string::npos) j = text.size();
    if (j > i) f.push_back(text.substr(i, j - i));
    i = j + 1;
  }
  if (f.size() < 13) return false;
  long pp = 0, ut = 0, st = 0;
  if (!parse_int(f[1], pp) || !parse_int(f[11], ut) || !parse_int(f[12], st)) return false;
  ppid = int(pp);
  ticks = (unsigned long long)(ut) + (unsigned long long)(st);
  return true;
}

std::vector<int> list_numeric(const std::string& dir) {
  std::vector<int> out;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return out;
  while (const dirent* e = ::readdir(d))
    if (all_digits(e->d_name)) out.push_back(std::atoi(e->d_name));
  ::closedir(d);
  return out;
}

// "200000000 300000000 ..." (or min_freq / max_freq) -> lowest and highest step in MHz.
bool read_range(const std::string& dir, double& lo, double& hi) {
  std::string t;
  lo = hi = 0;
  if (read_trimmed(dir + "/available_frequencies", t)) {
    size_t i = 0;
    while (i < t.size()) {
      while (i < t.size() && (t[i] == ' ' || t[i] == '\n')) ++i;
      size_t j = i;
      while (j < t.size() && t[j] != ' ' && t[j] != '\n') ++j;
      long v = 0;
      if (j > i && parse_int(std::string_view(t).substr(i, j - i), v) && v > 0) {
        const double m = double(v) / 1e6;
        if (lo == 0 || m < lo) lo = m;
        if (m > hi) hi = m;
      }
      i = j;
    }
    if (hi > lo) return true;
  }
  const double a = read_mhz(dir + "/min_freq"), b = read_mhz(dir + "/max_freq");
  if (a > 0 && b > a) {
    lo = a;
    hi = b;
    return true;
  }
  lo = hi = 0;
  return false;
}

}  // namespace

// ---- Sampler ------------------------------------------------------------------------------------------------------------------

Sampler::Sampler(Paths p) : p_(std::move(p)) {
  const long t = p_.clk_tck > 0 ? p_.clk_tck : ::sysconf(_SC_CLK_TCK);
  tck_ = t > 0 ? t : 100;
}

void Sampler::fill_ranges(Series& s) const {
  read_range(p_.gpu_dir, s.gpu_min, s.gpu_max);
  read_range(p_.dmc_dir, s.dmc_min, s.dmc_max);
}

Sample Sampler::take(int root_pid, double now_s) {
  Sample s;
  s.t_s = now_s;
  s.gpu_mhz = read_mhz(p_.gpu_dir + "/cur_freq");
  s.dmc_mhz = read_mhz(p_.dmc_dir + "/cur_freq");
  for (const std::string& f : p_.busy_files) {
    std::string t;
    if (!read_trimmed(f, t)) continue;
    long long cb = 0, ci = 0;
    if (parse_counters(t, cb, ci)) {  // kbase: percent over the time since the previous sample
      if (have_counters_ && cb >= prev_busy_ && ci >= prev_idle_ && (cb - prev_busy_) + (ci - prev_idle_) > 0)
        s.gpu_busy = 100.0 * double(cb - prev_busy_) / double((cb - prev_busy_) + (ci - prev_idle_));
      prev_busy_ = cb, prev_idle_ = ci, have_counters_ = true;
      break;
    }
    const double b = parse_busy(t);  // a plain percentage ("45", "45@600000000Hz")
    if (b >= 0) {
      s.gpu_busy = b;
      break;
    }
  }
  // the process tree of the game (runemu.sh forks the emulator), then every thread of it
  std::map<int, std::vector<int>> kids;
  for (int pid : list_numeric(p_.proc_root)) {
    std::string t;
    int pp = 0;
    unsigned long long ticks = 0;
    if (slurp_small(p_.proc_root + "/" + std::to_string(pid) + "/stat", t) && parse_stat(t, pp, ticks)) kids[pp].push_back(pid);
  }
  std::vector<int> tree = {root_pid};
  for (size_t i = 0; i < tree.size(); ++i) {
    auto it = kids.find(tree[i]);
    if (it == kids.end()) continue;
    for (int k : it->second)
      if (std::find(tree.begin(), tree.end(), k) == tree.end()) tree.push_back(k);
  }
  std::vector<std::pair<int, unsigned long long>> cur;
  for (int pid : tree) {
    const std::string base = p_.proc_root + "/" + std::to_string(pid);
    const std::vector<int> tids = list_numeric(base + "/task");
    std::string t;
    int pp = 0;
    unsigned long long ticks = 0;
    if (tids.empty()) {  // no task directory: the process as a whole
      if (slurp_small(base + "/stat", t) && parse_stat(t, pp, ticks)) cur.emplace_back(pid, ticks);
      continue;
    }
    for (int tid : tids)
      if (slurp_small(base + "/task/" + std::to_string(tid) + "/stat", t) && parse_stat(t, pp, ticks)) cur.emplace_back(tid, ticks);
  }
  std::sort(cur.begin(), cur.end());
  const double dt = now_s - prev_t_;
  if (prev_t_ >= 0 && dt > 0 && !cur.empty()) {
    double hot = 0, total = 0;
    for (const auto& c : cur) {
      auto it = std::lower_bound(prev_.begin(), prev_.end(), std::make_pair(c.first, 0ull));
      if (it == prev_.end() || it->first != c.first || c.second < it->second) continue;
      const double pct = double(c.second - it->second) / double(tck_) / dt * 100.0;
      hot = std::max(hot, pct);
      total += pct;
    }
    s.cpu_hot = hot;
    s.cpu_total = total;
  }
  prev_ = std::move(cur);
  prev_t_ = now_s;
  return s;
}

// ---- classifier -----------------------------------------------------------------------------------------------------------------

const char* verdict_name(Verdict v) {
  switch (v) {
    case Verdict::TooShort: return "too-short";
    case Verdict::PerfmodeOn: return "perfmode-on";
    case Verdict::NoData: return "no-data";
    case Verdict::Idle: return "idle";
    case Verdict::CpuBound: return "cpu-bound";
    case Verdict::GovernorLimited: return "governor-limited";
    case Verdict::Unclear: return "unclear";
  }
  return "?";
}

Verdict classify(const Series& s, bool perfmode_on_now, const Params& p) {
  if (perfmode_on_now) return Verdict::PerfmodeOn;
  if (s.secs < p.min_secs) return Verdict::TooShort;
  std::vector<const Sample*> u;
  for (const Sample& x : s.samples)
    if (x.t_s >= p.warmup_s && x.gpu_mhz >= 0) u.push_back(&x);
  if (u.size() < p.min_samples || s.gpu_max <= s.gpu_min) return Verdict::NoData;
  const bool dmc_known = s.dmc_max > s.dmc_min;
  size_t with_busy = 0;
  for (const Sample* x : u) with_busy += x->gpu_busy >= 0;
  const bool busy_known = double(with_busy) >= 0.8 * double(u.size());
  size_t lim = 0, cpub = 0, idle = 0;
  for (const Sample* x : u) {
    const bool gpu_low = (x->gpu_mhz - s.gpu_min) / (s.gpu_max - s.gpu_min) <= p.low_frac;
    if (busy_known && x->gpu_busy >= 0) {
      lim += gpu_low && x->gpu_busy >= p.busy_hi;
      cpub += x->gpu_busy < p.busy_lo && x->cpu_hot >= p.cpu_sat;
      idle += x->gpu_busy < 30 && (x->cpu_hot < 0 || x->cpu_hot < p.idle_cpu);
    } else {
      const bool dmc_low = !dmc_known || x->dmc_mhz < 0 || (x->dmc_mhz - s.dmc_min) / (s.dmc_max - s.dmc_min) <= p.low_frac;
      lim += gpu_low && dmc_low && x->cpu_hot >= p.cpu_sat;
      idle += x->cpu_hot >= 0 && x->cpu_hot < p.idle_cpu;
    }
  }
  const double n = double(u.size());
  if (double(lim) / n >= (busy_known ? p.share_busy : p.share_nobusy)) return Verdict::GovernorLimited;
  if (busy_known && double(cpub) / n >= p.share_busy) return Verdict::CpuBound;
  if (double(idle) / n >= p.share_other) return Verdict::Idle;
  return Verdict::Unclear;
}

// ---- Monitor --------------------------------------------------------------------------------------------------------------------

struct Monitor::Impl {
  Paths paths;
  double interval = 2.0;
  std::thread th;
  std::mutex mu;
  std::condition_variable cv;
  bool stopping = false, started = false;
  Series series;
  Clock::time_point t0;
};

Monitor::Monitor(Paths p, double interval_s) : i_(new Impl) {
  i_->paths = std::move(p);
  i_->interval = interval_s > 0.005 ? interval_s : 0.005;
}

Monitor::~Monitor() {
  stop();
  delete i_;
}

void Monitor::start(int pid) {
  if (i_->started) return;
  i_->started = true;
  i_->stopping = false;
  i_->series = Series();
  i_->t0 = Clock::now();
  Impl* im = i_;
  im->th = std::thread([im, pid] {
    Sampler sm(im->paths);
    Series ranges;
    sm.fill_ranges(ranges);
    auto elapsed = [im] { return std::chrono::duration<double>(Clock::now() - im->t0).count(); };
    Sample first = sm.take(pid, 0);  // seeds the CPU ticks; its own CPU figures are unknown
    std::unique_lock<std::mutex> lk(im->mu);
    im->series.gpu_min = ranges.gpu_min;
    im->series.gpu_max = ranges.gpu_max;
    im->series.dmc_min = ranges.dmc_min;
    im->series.dmc_max = ranges.dmc_max;
    im->series.samples.push_back(first);
    while (!im->cv.wait_for(lk, std::chrono::duration<double>(im->interval), [im] { return im->stopping; })) {
      lk.unlock();
      Sample x = sm.take(pid, elapsed());
      lk.lock();
      im->series.samples.push_back(x);
    }
  });
}

Series Monitor::stop() {
  if (!i_->started) return Series();
  {
    std::lock_guard<std::mutex> lk(i_->mu);
    i_->stopping = true;
  }
  i_->cv.notify_all();
  if (i_->th.joinable()) i_->th.join();
  i_->started = false;
  Series out = std::move(i_->series);
  out.secs = std::chrono::duration<double>(Clock::now() - i_->t0).count();
  i_->series = Series();
  return out;
}

// ---- the hook of launch::run ----------------------------------------------------------------------------------------------------

namespace {
struct Hook {
  std::mutex mu;
  bool armed = false;
  Paths paths;
  double interval = 2.0;
  std::unique_ptr<Monitor> mon;
  bool have = false;
  Series last;
};
Hook& hook() {
  static Hook h;
  return h;
}
}  // namespace

void arm(const Paths& p, double interval_s) {
  Hook& h = hook();
  std::lock_guard<std::mutex> lk(h.mu);
  h.armed = true;
  h.paths = p;
  h.interval = interval_s;
  h.have = false;
  h.last = Series();
}

void disarm() {
  Hook& h = hook();
  std::lock_guard<std::mutex> lk(h.mu);
  h.armed = false;
  h.have = false;
}

bool armed() {
  Hook& h = hook();
  std::lock_guard<std::mutex> lk(h.mu);
  return h.armed;
}

void on_spawn(int pid) {
  Hook& h = hook();
  std::lock_guard<std::mutex> lk(h.mu);
  if (!h.armed || pid <= 0) return;
  h.mon = std::make_unique<Monitor>(h.paths, h.interval);
  h.mon->start(pid);
}

void on_exit() {
  Hook& h = hook();
  std::unique_ptr<Monitor> m;
  {
    std::lock_guard<std::mutex> lk(h.mu);
    m = std::move(h.mon);
  }
  if (!m) return;
  Series s = m->stop();  // joined outside the lock
  std::lock_guard<std::mutex> lk(h.mu);
  h.last = std::move(s);
  h.have = true;
  h.armed = false;
}

bool take_series(Series& out) {
  Hook& h = hook();
  std::lock_guard<std::mutex> lk(h.mu);
  if (!h.have) return false;
  out = std::move(h.last);
  h.have = false;
  return true;
}

// ---- state kept between sessions ------------------------------------------------------------------------------------------------

std::string store_path() {
  if (const char* e = std::getenv("ZF_AUTOPERF")) return e;
  return "/storage/.config/zfront/autoperf.tsv";
}

namespace {

struct Row {
  State st = State::None;
  std::string sys, rom;
};
constexpr size_t kMaxRows = 2000;

const char* state_word(State s) {
  return s == State::Suggest ? "suggest" : s == State::Dismissed ? "dismissed" : s == State::Accepted ? "accepted" : "";
}

std::string clean(std::string s) {
  for (char& c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}

std::vector<Row> load_rows(const std::string& file) {
  std::vector<Row> out;
  std::string text;
  if (!read_file(file, text)) return out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t e = text.find('\n', pos);
    if (e == std::string::npos) e = text.size();
    const std::string line = text.substr(pos, e - pos);
    pos = e + 1;
    const size_t a = line.find('\t');
    const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos) continue;
    Row r;
    const std::string w = line.substr(0, a);
    r.st = w == "suggest" ? State::Suggest : w == "dismissed" ? State::Dismissed : w == "accepted" ? State::Accepted : State::None;
    if (r.st == State::None) continue;
    r.sys = line.substr(a + 1, b - a - 1);
    r.rom = line.substr(b + 1);
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace

State get_state(const std::string& file, const std::string& system, const std::string& rom) {
  const std::string s = clean(system), r = clean(rom);
  for (const Row& x : load_rows(file))
    if (x.sys == s && x.rom == r) return x.st;
  return State::None;
}

bool set_state(const std::string& file, const std::string& system, const std::string& rom, State st) {
  const std::string s = clean(system), r = clean(rom);
  std::vector<Row> rows = load_rows(file);
  rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& x) { return x.sys == s && x.rom == r; }), rows.end());
  if (st != State::None) rows.push_back({st, s, r});
  if (rows.size() > kMaxRows) rows.erase(rows.begin(), rows.begin() + long(rows.size() - kMaxRows));
  std::string text;
  for (const Row& x : rows) text += std::string(state_word(x.st)) + "\t" + x.sys + "\t" + x.rom + "\n";
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
  return write_atomic(file, text);
}

bool record_result(const std::string& file, const std::string& system, const std::string& rom, Verdict v) {
  const State cur = get_state(file, system, rom);
  if (v == Verdict::PerfmodeOn) {  // the user turned it on by hand: an open suggestion is done
    if (cur == State::Suggest) set_state(file, system, rom, State::Accepted);
    return false;
  }
  if (v != Verdict::GovernorLimited || cur != State::None) return false;  // dismissed stays dismissed; a standing suggestion is not repeated
  return set_state(file, system, rom, State::Suggest);
}

// ---- with the settings ----------------------------------------------------------------------------------------------------------

bool enabled(const Ctx& cx) { return mg::conf_get(cx, kKey) != "0"; }

bool perfmode_on(const Ctx& cx, const std::string& system, const std::string& rom_file) {
  const std::string keys[3] = {mg::game_cfg(system, rom_file) + ".perfmode", system + ".perfmode", "global.perfmode"};
  for (const std::string& key : keys) {
    const std::string v = mg::conf_get(cx, key);
    if (v == "1") return true;
    if (v == "0") return false;
  }
  return false;
}

// The game's own PERFORMANCE MODE = OFF is a decision of the user: no sampling, no suggestion.
bool game_off(const Ctx& cx, const std::string& system, const std::string& rom_file) { return mg::conf_get(cx, mg::game_cfg(system, rom_file) + ".perfmode") == "0"; }

bool supported(const Ctx& cx, const std::string& system, const std::string& rom_file) {
  const System* sys = mg::find_system(cx, system);
  if (!sys) return false;
  const mg::Pick rp = mg::resolved_pick(*sys, cx.conf.map(), rom_file);
  return mg::features().resolve(*sys, rp.emu, rp.core).has_custom("perfmode");
}

void begin_session(const Ctx& cx, const std::string& system, const std::string& rom_path) {
  disarm();
  if (system.empty() || rom_path.empty() || !enabled(cx)) return;
  if (uimode::current() != uimode::Mode::Full) return;  // Kiosk / Kid have no game options to act on a suggestion
  const std::string file = path_filename(rom_path);
  if (game_off(cx, system, file)) return;
  if (get_state(store_path(), system, rom_path) != State::None) return;  // already suggested, dismissed or accepted: nothing to learn
  if (perfmode_on(cx, system, file) || !supported(cx, system, file)) return;
  arm(Paths());
}

std::string conclude(Ctx& cx, const std::string& system, const std::string& rom_path, const Series& s) {
  const Verdict v = classify(s, perfmode_on(cx, system, path_filename(rom_path)));
  std::fprintf(stderr, "zfront: performance suggestions: %s (%.0f s, %zu samples)\n", verdict_name(v), s.secs, s.samples.size());
  if (!record_result(store_path(), system, rom_path, v)) return std::string();
  return std::string(tr("PERFORMANCE MODE MAY HELP THIS GAME"));
}

std::string end_session(Ctx& cx, const std::string& system, const std::string& rom_path, unsigned long secs, int exit_code) {
  Series s;
  const bool have = take_series(s);  // before disarm(), which drops the series
  disarm();  // a spawn that failed leaves the hook armed for nobody
  if (!have) return std::string();
  if (exit_code != 0 || system.empty() || rom_path.empty()) return std::string();
  if (s.secs <= 0) s.secs = double(secs);
  return conclude(cx, system, rom_path, s);
}

bool pending(const Ctx& cx, const std::string& system, const std::string& rom_path) {
  if (!enabled(cx)) return false;
  const std::string file = path_filename(rom_path);
  if (game_off(cx, system, file)) return false;
  return get_state(store_path(), system, rom_path) == State::Suggest && !perfmode_on(cx, system, file) && supported(cx, system, file);
}

void game_entries(MenuBuilder& m, Ctx& cx, const std::string& system, const std::string& rom_path) {
  if (!pending(cx, system, rom_path)) return;
  Ctx* c = &cx;
  Refresh rf = m.refresh;
  const std::string cfg = mg::game_cfg(system, path_filename(rom_path));
  m.header(tr("THIS GAME MAY RUN BETTER WITH PERFORMANCE MODE"));
  m.action(tr("TURN ON PERFORMANCE MODE FOR THIS GAME"),
           [c, rf, system, rom_path, cfg] {
             if (!mg::conf_put(*c, cfg + ".perfmode", "1")) return;
             set_state(store_path(), system, rom_path, State::Accepted);
             c->sv->toasts.push(tr("PERFORMANCE MODE ON FOR THIS GAME"), 2500);
             if (rf) rf();
           })
      .hint(tr("This game may run better with performance mode: the GPU and memory stay at full speed while playing."));
  m.action(tr("STOP SUGGESTING PERFORMANCE MODE"),
           [c, rf, system, rom_path] {
             (void)c;
             set_state(store_path(), system, rom_path, State::Dismissed);
             if (rf) rf();
           })
      .hint(tr("Do not suggest performance mode for this game again."));
}

}  // namespace zf::autoperf
