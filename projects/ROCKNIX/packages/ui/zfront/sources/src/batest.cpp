#include "batest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <utility>

#include <sys/stat.h>

#include "i18n.hpp"
#include "menu.hpp"
#include "power.hpp"
#include "util.hpp"

namespace zf::batest {

bool usable(const playstats::LogEntry& e) { return e.secs >= kMinSecs && e.drop_pct10 > 0; }

Rate rate_of(const std::vector<playstats::LogEntry>& log, size_t n, const std::string& system, const std::string& rom, bool by_system, bool by_game) {
  std::vector<const playstats::LogEntry*> pick;
  for (const playstats::LogEntry& e : log) {
    if (!usable(e)) continue;
    if (by_system && e.system != system) continue;
    if (by_game && e.rom != rom) continue;
    pick.push_back(&e);
  }
  // newest first; equal starts keep the log order, the later line being the newer one
  std::stable_sort(pick.begin(), pick.end(), [](const playstats::LogEntry* a, const playstats::LogEntry* b) { return a->start > b->start; });
  if (pick.size() > n) pick.resize(n);
  Rate r;
  if (pick.empty()) return r;
  double used = 0, secs = 0, w = 1;
  for (const playstats::LogEntry* e : pick) {
    used += w * double(e->drop_pct10) / 10.0;
    secs += w * double(e->secs);
    w *= kDecay;
  }
  if (secs <= 0 || used <= 0) return r;
  r.pct_per_hour = used / secs * 3600.0;
  r.sessions = pick.size();
  r.source = by_game ? Rate::Game : by_system ? Rate::System : Rate::Global;
  return r;
}

Rate estimate(const std::vector<playstats::LogEntry>& log, const std::string& system, const std::string& rom) {
  Rate r = rate_of(log, kGameN, system, rom, true, true);
  if (r.valid()) return r;
  r = rate_of(log, kSystemN, system, rom, true, false);
  if (r.valid()) return r;
  return rate_of(log, kGlobalN, system, rom, false, false);
}

double hours_left(const Rate& r, int percent) {
  if (!r.valid() || r.pct_per_hour <= 0) return -1.0;
  return double(std::max(0, percent)) / r.pct_per_hour;
}

std::string format_left(double hours) {
  if (hours > 48.0) return "> 48 h";
  const uint64_t secs = uint64_t(std::max(0.0, hours) * 3600.0 + 0.5);
  if (secs < 60) return "<1 min";
  if (secs < 300) return "~" + playstats::format_duration(secs);
  const uint64_t r5 = ((secs + 150) / 300) * 300;  // nearest five minutes
  return "~" + playstats::format_duration(r5);
}

std::vector<Drainer> top_drainers(const std::vector<playstats::LogEntry>& log, size_t n) {
  std::map<std::pair<std::string, std::string>, bool> seen;
  std::vector<Drainer> out;
  for (const playstats::LogEntry& e : log) {
    if (!usable(e)) continue;
    if (!seen.emplace(std::make_pair(e.system, e.rom), true).second) continue;
    const Rate r = rate_of(log, kGameN, e.system, e.rom, true, true);
    if (!r.valid()) continue;
    out.push_back({e.system, e.rom, r.pct_per_hour, r.sessions});
  }
  std::stable_sort(out.begin(), out.end(), [](const Drainer& a, const Drainer& b) {
    if (a.pct_per_hour != b.pct_per_hour) return a.pct_per_hour > b.pct_per_hour;
    return a.rom < b.rom;
  });
  if (out.size() > n) out.resize(n);
  return out;
}

namespace {
// The play log only grows, and GAME OPTIONS is built on every refresh: it is read and parsed again only when the file changed
// (path, size or time). Main thread only.
const std::vector<playstats::LogEntry>& cached_log() {
  static std::vector<playstats::LogEntry> log;
  static std::string path;
  static long long size = -1, sec = 0, nsec = 0;
  const std::string p = playstats::log_path();
  struct stat st;
  if (::stat(p.c_str(), &st) != 0) {
    log.clear();
    path.clear();
    size = -1;
    return log;
  }
  if (p == path && (long long)st.st_size == size && (long long)st.st_mtim.tv_sec == sec && (long long)st.st_mtim.tv_nsec == nsec) return log;
  std::string text;
  read_file(p, text);
  log = playstats::parse_log(text);
  path = p;
  size = (long long)st.st_size;
  sec = (long long)st.st_mtim.tv_sec;
  nsec = (long long)st.st_mtim.tv_nsec;
  return log;
}
}  // namespace

void game_entry(MenuBuilder& m, Ctx&, const std::string& system, const std::string& rom_path) {
  const power::Now now = power::read_now();
  if (!now.have) return;  // a console without a battery has nothing to say
  const Rate r = estimate(cached_log(), system, rom_path);
  if (!r.valid()) {
    m.info(tr("BATTERY"), tr("NOT ENOUGH DATA YET")).hint(tr("Play this game for five minutes or more on the battery and the estimate appears here."));
    return;
  }
  const std::string value = format_left(hours_left(r, now.percent));
  switch (r.source) {
    case Rate::Game: m.info(tr("BATTERY"), value).hint(tr("With the current charge, at the rate of the last sessions of this game.")); break;
    case Rate::System: m.info(tr("BATTERY"), value).hint(tr("With the current charge. No sessions of this game yet: the average of its system is used.")); break;
    default: m.info(tr("BATTERY"), value).hint(tr("With the current charge. No sessions of this system yet: the average of all games is used.")); break;
  }
}

}  // namespace zf::batest
