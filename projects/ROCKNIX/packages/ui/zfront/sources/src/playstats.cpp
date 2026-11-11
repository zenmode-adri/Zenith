#include "playstats.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>

#include "gamelist.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace zf::playstats {

std::string log_path() {
  if (const char* e = std::getenv("ZF_PLAYLOG")) return e;
  return "/storage/.config/zfront/playlog.tsv";
}

namespace {

std::string clean(std::string s) {
  for (char& c : s)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}

std::string iso_dash(time_t t) {
  struct tm tmv;
  ::localtime_r(&t, &tmv);
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tmv);
  return b;
}

bool parse_iso(const std::string& s, time_t& out) {
  struct tm tmv;
  std::memset(&tmv, 0, sizeof tmv);
  const char* end = ::strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tmv);
  if (!end || *end) return false;
  tmv.tm_isdst = -1;
  out = ::mktime(&tmv);
  return out != time_t(-1);
}

// the newest half of an oversized log, cut at a line start
void trim(const std::string& path) {
  std::error_code ec;
  const auto sz = std::filesystem::file_size(path, ec);
  if (ec || sz <= kMaxLogBytes) return;
  std::string old;
  if (!read_file(path, old)) return;
  const size_t cut = old.find('\n', old.size() / 2);
  if (cut == std::string::npos) return;
  write_atomic(path, old.substr(cut + 1));
}

}  // namespace

std::string format_line(time_t start, uint64_t secs, const std::string& system, const std::string& rom, int drop_pct10) {
  std::string l = iso_dash(start) + "\t" + std::to_string(secs) + "\t" + clean(system) + "\t" + clean(rom);
  if (drop_pct10 >= 0) l += "\t" + std::to_string(drop_pct10);
  return l + "\n";
}

bool record(const std::string& path, time_t start, uint64_t secs, const std::string& system, const std::string& rom, int drop_pct10) {
  if (secs < kMinSecs) return false;
  trim(path);
  const std::string line = format_line(start, secs, system, rom, drop_pct10);
  int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {  // first time: nothing else in zfront makes this folder
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return false;
  }
  const ssize_t n = ::write(fd, line.data(), line.size());
  ::close(fd);
  return n == ssize_t(line.size());
}

bool record_ended(const std::string& system, const std::string& rom, uint64_t secs, time_t end, int drop_pct10) {
  if (end == 0) end = ::time(nullptr);
  return record(log_path(), end - time_t(secs), secs, system, rom, drop_pct10);
}

std::vector<LogEntry> parse_log(const std::string& text) {
  std::vector<LogEntry> out;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t e = text.find('\n', pos);
    if (e == std::string::npos) e = text.size();
    const std::string line = text.substr(pos, e - pos);
    pos = e + 1;
    std::vector<std::string> f;
    size_t a = 0;
    for (int i = 0; i < 3; ++i) {
      const size_t t = line.find('\t', a);
      if (t == std::string::npos) break;
      f.push_back(line.substr(a, t - a));
      a = t + 1;
    }
    if (f.size() != 3) continue;
    f.push_back(line.substr(a));
    LogEntry le;
    if (!parse_iso(f[0], le.start)) continue;
    char* endp = nullptr;
    const unsigned long long v = std::strtoull(f[1].c_str(), &endp, 10);
    if (f[1].empty() || *endp || f[1][0] < '0' || f[1][0] > '9') continue;  // strtoull takes a sign: "-5" is not a time
    le.secs = v;
    le.system = f[2];
    // the rom path ends at the next TAB; what follows is the optional battery column (anything after that is ignored)
    const size_t t4 = f[3].find('\t');
    le.rom = f[3].substr(0, t4);
    if (t4 != std::string::npos) {
      const size_t t5 = f[3].find('\t', t4 + 1);
      const std::string col = f[3].substr(t4 + 1, t5 == std::string::npos ? std::string::npos : t5 - t4 - 1);
      char* ep = nullptr;
      const long d = std::strtol(col.c_str(), &ep, 10);
      if (!col.empty() && !*ep && d >= 0 && d <= 1000) le.drop_pct10 = int(d);
    }
    out.push_back(std::move(le));
  }
  return out;
}

std::vector<GameTime> collect(const std::vector<System>& systems) {
  std::vector<GameTime> out;
  auto take = [&](const System& s, const std::vector<Game>& games) {
    const std::string sysname = s.fullname.empty() ? s.name : s.fullname;
    for (const Game& g : games) {
      if (g.meta.gametime == 0 && g.meta.playcount == 0) continue;
      GameTime t;
      t.name = g.name.empty() ? path_stem(g.path) : g.name;
      t.system = sysname;
      t.secs = g.meta.gametime;
      t.plays = g.meta.playcount;
      out.push_back(std::move(t));
    }
  };
  for (const System& s : systems) {
    if (s.loaded || !s.games.empty()) {
      take(s, s.games);
    } else if (!s.gamelist.empty() && file_exists(s.gamelist)) {  // nothing can have been played in a system without a gamelist
      std::vector<Game> tmp = load_games(s.rom_dir, s.exts, s.gamelist);
      take(s, tmp);
    }
  }
  return out;
}

bool Stats::empty() const {
  if (total_secs || total_plays) return false;
  for (uint64_t w : week_secs)
    if (w) return false;
  return true;
}

Stats compute(const std::vector<GameTime>& games, const std::vector<LogEntry>& log, time_t now) {
  Stats st;
  std::map<std::string, uint64_t> by_sys;
  std::vector<GameTime> top;
  for (const GameTime& g : games) {
    st.total_secs += g.secs;
    st.total_plays += g.plays;
    if (!g.secs) continue;
    by_sys[g.system] += g.secs;
    top.push_back(g);
  }
  std::stable_sort(top.begin(), top.end(), [](const GameTime& a, const GameTime& b) {
    if (a.secs != b.secs) return a.secs > b.secs;
    return a.name < b.name;
  });
  if (top.size() > 10) top.resize(10);
  st.top = std::move(top);
  for (const auto& kv : by_sys) st.systems.push_back(kv);
  std::stable_sort(st.systems.begin(), st.systems.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  constexpr time_t kWeek = 7 * 24 * 3600;
  for (const LogEntry& e : log) {
    if (e.secs < kMinSecs) continue;
    const time_t age = std::max<time_t>(0, now - e.start);  // a clock that went back counts as now
    const time_t w = age / kWeek;
    if (w < kWeeks) st.week_secs[w] += e.secs;
  }
  return st;
}

std::string format_duration(uint64_t secs) {
  if (secs > 0 && secs < 60) return "<1 min";
  const uint64_t m = (secs + 30) / 60;
  char b[48];
  if (m >= 60) std::snprintf(b, sizeof b, "%llu h %02llu min", (unsigned long long)(m / 60), (unsigned long long)(m % 60));
  else std::snprintf(b, sizeof b, "%llu min", (unsigned long long)m);
  return b;
}

}  // namespace zf::playstats
