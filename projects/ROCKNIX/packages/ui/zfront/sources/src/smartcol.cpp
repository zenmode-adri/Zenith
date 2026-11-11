#include "smartcol.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <map>

#include "collections.hpp"
#include "i18n.hpp"
#include "util.hpp"

namespace zf::smartcol {

namespace {

time_t stat_time(const std::string& p) {
  struct stat st;
  if (::stat(p.c_str(), &st) != 0) return 0;
  return std::max<time_t>(st.st_mtime, st.st_ctime);
}

struct Cand {
  size_t sys, game;
  const Game* g;
};

}  // namespace

const std::vector<Decl>& decls() {
  static const std::vector<Decl> d = {
      {"recentlyadded", N_("recently added"), "auto-recentlyadded"},
      {"unfinished", N_("unfinished"), "auto-unfinished"},
      {"weekly", N_("most played this week"), "auto-mostplayedweek"},
  };
  return d;
}

std::string label(const Decl& d) { return utf8_cap_first(tr(d.long_name.c_str())); }

std::vector<std::string> enabled(const EsSettings& es) {
  std::vector<std::string> out;
  const std::string* v = es.get(kSettingsKey);
  if (!v) return out;
  for (const std::string& t : collections::split_list(*v, ',')) {
    std::string s(trim(t));
    if (!s.empty()) out.push_back(s);
  }
  return out;
}

time_t lastplayed_time(uint64_t lp) {
  if (lp == 0) return 0;
  struct tm t;
  std::memset(&t, 0, sizeof t);
  const uint64_t d = lp / 1000000ULL, h = lp % 1000000ULL;
  t.tm_year = int(d / 10000) - 1900;
  t.tm_mon = int(d / 100 % 100) - 1;
  t.tm_mday = int(d % 100);
  t.tm_hour = int(h / 10000);
  t.tm_min = int(h / 100 % 100);
  t.tm_sec = int(h % 100);
  t.tm_isdst = -1;
  if (t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31) return 0;
  const time_t r = ::mktime(&t);
  return r == time_t(-1) ? 0 : r;
}

std::vector<Member> members(const std::string& name, const std::vector<System>& systems, const std::vector<playstats::LogEntry>& log, const Options& o) {
  std::vector<Member> out;
  if (name != "recentlyadded" && name != "unfinished" && name != "weekly" && name != "neverplayed") return out;

  // candidates: loaded games as they are, unloaded systems read for this call; hidden / kid rules applied here (same as load_games)
  const bool kid_only = o.mode == uimode::Mode::Kid;
  const bool show_hidden = o.show_hidden && o.mode == uimode::Mode::Full;
  std::vector<std::vector<Game>> owned;
  owned.reserve(systems.size());
  std::vector<Cand> cands;
  for (size_t si = 0; si < systems.size(); ++si) {
    const System& s = systems[si];
    const std::vector<Game>* games = nullptr;
    bool is_loaded = true;
    if (s.loaded || !s.games.empty()) {
      games = &s.games;
    } else if (!s.rom_dir.empty()) {
      owned.push_back(load_games(s.rom_dir, s.exts, s.gamelist));
      games = &owned.back();
      is_loaded = false;
    }
    if (!games) continue;
    for (size_t gi = 0; gi < games->size(); ++gi) {
      const Game& g = (*games)[gi];
      if ((g.meta.hidden && !show_hidden) || (kid_only && !g.meta.kid)) continue;
      cands.push_back({si, is_loaded ? gi : kNoGame, &g});
    }
  }
  auto make = [&](const Cand& c, time_t key) {
    Member m;
    m.sys = c.sys;
    m.game = c.game;
    m.path = c.g->path;
    m.name = c.g->name.empty() ? path_stem(c.g->path) : c.g->name;
    m.key = key;
    return m;
  };

  constexpr time_t kDay = 24 * 3600;
  if (name == "recentlyadded") {
    auto ft = o.file_time ? o.file_time : std::function<time_t(const std::string&)>(stat_time);
    for (const Cand& c : cands) {
      const time_t t = ft(c.g->path);
      if (t > 0 && o.now - t <= kAddedDays * kDay) out.push_back(make(c, t));  // a time in the future (clock set back) counts as new
    }
  } else if (name == "neverplayed") {
    for (const Cand& c : cands)
      if (c.g->meta.playcount == 0 && c.g->meta.lastplayed == 0) out.push_back(make(c, 0));
  } else if (name == "unfinished") {
    for (const Cand& c : cands) {
      const GameMeta& m = c.g->meta;
      if (m.playcount < 1 || m.playcount > uint32_t(kUnfinishedMaxPlays) || m.gametime >= kUnfinishedMaxSecs) continue;
      const time_t lp = lastplayed_time(m.lastplayed);
      if (lp == 0 || o.now - lp <= kUnfinishedIdleDays * kDay) continue;
      out.push_back(make(c, lp));
    }
  } else {  // weekly: same window as playstats::compute week_secs[0]
    std::map<std::string, uint64_t> secs;
    for (const playstats::LogEntry& e : log) {
      if (e.secs < playstats::kMinSecs) continue;
      const time_t age = std::max<time_t>(0, o.now - e.start);
      if (age / (kWeekDays * kDay) == 0) secs[e.rom] += e.secs;
    }
    for (const Cand& c : cands) {
      auto it = secs.find(c.g->path);
      if (it != secs.end()) out.push_back(make(c, time_t(it->second)));
    }
  }

  std::stable_sort(out.begin(), out.end(), [&](const Member& a, const Member& b) {
    if (name != "neverplayed" && a.key != b.key) return a.key > b.key;  // newest / most first
    const std::string ua = utf8_upper(a.name), ub = utf8_upper(b.name);
    if (ua != ub) return ua < ub;
    return a.path < b.path;
  });
  return out;
}

std::vector<Collection> build(const std::vector<System>& systems, const EsSettings& es, const std::vector<playstats::LogEntry>& log, const Options& o,
                              bool keep_empty) {
  std::vector<Collection> out;
  auto add = [&](const std::string& name, const std::string& folder, const std::string& lab) {
    Collection c;
    c.name = name;
    c.theme_folder = folder;
    c.label = lab;
    c.games = members(name, systems, log, o);
    if (!c.games.empty() || keep_empty) out.push_back(std::move(c));
  };
  const std::vector<std::string> es_on = collections::enabled_auto(es);
  if (std::find(es_on.begin(), es_on.end(), "neverplayed") != es_on.end()) add("neverplayed", "auto-neverplayed", utf8_cap_first(tr("never played")));
  const std::vector<std::string> on = enabled(es);
  for (const Decl& d : decls())
    if (std::find(on.begin(), on.end(), d.name) != on.end()) add(d.name, d.theme_folder, label(d));
  return out;
}

}  // namespace zf::smartcol
