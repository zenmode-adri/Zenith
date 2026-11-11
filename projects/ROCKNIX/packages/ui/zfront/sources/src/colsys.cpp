#include "colsys.hpp"

#include <algorithm>
#include <unordered_map>

#include "collections.hpp"
#include "gamemeta.hpp"
#include "library_doctor.hpp"
#include "i18n.hpp"
#include "util.hpp"

namespace zf::colsys {

namespace {

bool g_requested = false;

bool has(const std::vector<std::string>& v, const char* s) { return std::find(v.begin(), v.end(), s) != v.end(); }

std::string game_name(const Game& g) { return g.name.empty() ? path_stem(g.path) : g.name; }

bool admit(const Game& g, const smartcol::Options& o) {
  const bool kid_only = o.mode == uimode::Mode::Kid;
  const bool show_hidden = o.show_hidden && o.mode == uimode::Mode::Full;
  return !((g.meta.hidden && !show_hidden) || (kid_only && !g.meta.kid));
}

// the rule of the automatic collections that can be checked from one game's metadata (resync); the others always hold
bool qualifies(const std::string& name, const Game& g) {
  if (name == "favorites") return g.meta.favorite;
  if (name == "neverplayed") return g.meta.playcount == 0 && g.meta.lastplayed == 0;
  if (name == "recent") return g.meta.playcount > 0;
  return true;
}

struct Cand {
  std::string key;  // what the order is made on
  uint64_t num = 0;
  size_t si = 0, gi = 0;
};

Game copy_of(const std::vector<System>& snap, size_t si, size_t gi) {
  Game g = snap[si].games[gi];
  g.src_system = gamelist_intern(snap[si].name);
  g.src_idx = uint32_t(gi);
  return g;
}

System make_system(const std::string& name, const std::string& full, const std::string& theme) {
  System s;
  s.name = name;
  s.fullname = full;
  s.theme = theme;
  s.loaded = true;
  s.collection = true;
  s.load_opts = load_tag();
  return s;
}

void by_name(std::vector<Cand>& v, const std::vector<System>& snap) {
  std::stable_sort(v.begin(), v.end(), [&](const Cand& a, const Cand& b) {
    if (a.key != b.key) return a.key < b.key;
    return snap[a.si].games[a.gi].path < snap[b.si].games[b.gi].path;
  });
}

void finish(System& c, const std::vector<System>& snap, const std::vector<Cand>& v) {
  c.games.reserve(v.size());
  for (const Cand& k : v) c.games.push_back(copy_of(snap, k.si, k.gi));
  c.count = c.games.size();
}

}  // namespace

bool any_enabled(const EsSettings& es) {
  const std::vector<std::string> a = collections::enabled_auto(es);
  for (const char* n : {"all", "recent", "favorites", "neverplayed"})
    if (has(a, n)) return true;
  if (!smartcol::enabled(es).empty()) return true;
  return !collections::enabled_custom(es).empty();
}

bool needs_log(const EsSettings& es) {
  const std::vector<std::string> s = smartcol::enabled(es);
  return has(s, "weekly");
}

void fill(std::vector<System>& snap) {
  for (System& s : snap) {
    if (s.loaded || !s.games.empty() || s.rom_dir.empty()) continue;
    s.games = load_games(s.rom_dir, s.exts, s.gamelist, &s.gamelist_damaged);
    s.count = s.games.size();
    s.loaded = true;
  }
}

bool keeps_order(const std::string& name) { return name == "recent" || name == "recentlyadded" || name == "unfinished" || name == "weekly"; }

void keep_game_systems(std::vector<System>& snap) {
  snap.erase(std::remove_if(snap.begin(), snap.end(), [](const System& s) { return !doctor::is_game_system(s); }), snap.end());
}

std::vector<System> build(const std::vector<System>& snap, const EsSettings& es, const std::vector<playstats::LogEntry>& log, const smartcol::Options& o) {
  // ES (CollectionSystemManager::updateSystemsList): the custom collections first, then the automatic ones in the order of its map (by key:
  // all, favorites, neverplayed, recent), then zfront's smart ones. Each group is built apart and joined at the end.
  std::vector<System> custom_out, auto_out, smart_out;
  std::vector<System>* dst = &auto_out;
  const std::vector<std::string> on = collections::enabled_auto(es);
  auto add = [&](System&& c) {
    if (!c.games.empty()) dst->push_back(std::move(c));
  };

  // ---- ES automatic collections -------------------------------------------------------------------------------------
  auto ordinary = [&](const char* name, bool (*keep)(const Game&), bool newest_first) {
    for (const collections::AutoDecl& d : collections::auto_decls()) {
      if (d.name != name) continue;
      System c = make_system(d.name, std::string(tr(utf8_upper(collections::label(d)).c_str())), d.theme_folder);  // written like a system's name
      std::vector<Cand> v;
      for (size_t si = 0; si < snap.size(); ++si)
        for (size_t gi = 0; gi < snap[si].games.size(); ++gi) {
          const Game& g = snap[si].games[gi];
          if (!admit(g, o) || !keep(g)) continue;
          Cand k;
          k.si = si, k.gi = gi, k.key = utf8_upper(game_name(g)), k.num = g.meta.lastplayed;
          v.push_back(std::move(k));
        }
      if (newest_first) {
        std::stable_sort(v.begin(), v.end(), [&](const Cand& a, const Cand& b) {
          if (a.num != b.num) return a.num > b.num;
          if (a.key != b.key) return a.key < b.key;
          return snap[a.si].games[a.gi].path < snap[b.si].games[b.gi].path;
        });
        if (v.size() > kLastPlayedLimit) v.resize(kLastPlayedLimit);
      } else {
        by_name(v, snap);
      }
      finish(c, snap, v);
      add(std::move(c));
    }
  };
  if (has(on, "all")) ordinary("all", [](const Game&) { return true; }, false);
  if (has(on, "favorites")) ordinary("favorites", [](const Game& g) { return g.meta.favorite; }, false);

  // ---- NEVER PLAYED (an ES key, built by smartcol) and the smart ones ------------------------------------------------------
  const std::vector<smartcol::Collection> smart = smartcol::build(snap, es, log, o);
  auto from_smart = [&](const smartcol::Collection& sc) {
    System c = make_system(sc.name, std::string(tr(utf8_upper(sc.label).c_str())), sc.theme_folder);
    for (const smartcol::Member& m : sc.games) {
      if (m.sys >= snap.size() || m.game == smartcol::kNoGame || m.game >= snap[m.sys].games.size()) continue;
      c.games.push_back(copy_of(snap, m.sys, m.game));
    }
    c.count = c.games.size();
    add(std::move(c));
  };
  for (const smartcol::Collection& sc : smart)
    if (sc.name == "neverplayed") from_smart(sc);
  if (has(on, "recent")) ordinary("recent", [](const Game& g) { return g.meta.playcount > 0; }, true);
  dst = &smart_out;
  for (const smartcol::Collection& sc : smart)
    if (sc.name != "neverplayed") from_smart(sc);
  dst = &custom_out;

  // ---- custom collections ---------------------------------------------------------------------------------------------
  const std::vector<std::string> custom_on = collections::enabled_custom(es);
  if (!custom_on.empty()) {
    std::unordered_map<std::string, std::pair<size_t, size_t>> by_path;
    for (size_t si = 0; si < snap.size(); ++si)
      for (size_t gi = 0; gi < snap[si].games.size(); ++gi)
        if (admit(snap[si].games[gi], o)) by_path.emplace(snap[si].games[gi].path, std::make_pair(si, gi));
    for (const collections::Custom& cu : collections::list_custom()) {
      if (std::find(custom_on.begin(), custom_on.end(), cu.name) == custom_on.end()) continue;
      System c = make_system(cu.name, cu.name, "custom-collections");
      std::vector<Cand> v;
      auto push = [&](size_t si, size_t gi) {
        Cand k;
        k.si = si, k.gi = gi, k.key = utf8_upper(game_name(snap[si].games[gi]));
        v.push_back(std::move(k));
      };
      if (cu.dynamic) {
        collections::Filter f;
        if (!collections::read_filter(cu.name, f)) continue;
        const std::string text = utf8_upper(f.text);
        for (size_t si = 0; si < snap.size(); ++si) {
          if (!f.systems.empty() && std::find(f.systems.begin(), f.systems.end(), snap[si].name) == f.systems.end()) continue;
          for (size_t gi = 0; gi < snap[si].games.size(); ++gi) {
            const Game& g = snap[si].games[gi];
            if (!admit(g, o) || (f.only_favorites && !g.meta.favorite)) continue;
            if (!text.empty() && utf8_upper(game_name(g)).find(text) == std::string::npos) continue;
            push(si, gi);
          }
        }
      } else {
        for (const std::string& p : collections::members(cu.name)) {
          auto it = by_path.find(p);
          if (it != by_path.end()) push(it->second.first, it->second.second);
        }
      }
      by_name(v, snap);
      finish(c, snap, v);
      add(std::move(c));
    }
  }
  std::vector<System> out = std::move(custom_out);
  for (System& c : auto_out) out.push_back(std::move(c));
  for (System& c : smart_out) out.push_back(std::move(c));
  return out;
}

const System* source_system(const Ctx& cx, const Game& g) {
  if (g.src_system.empty()) return nullptr;
  for (const System& s : cx.systems)
    if (s.name == g.src_system) return &s;
  return nullptr;
}

bool resolve(Ctx& cx, size_t sys, size_t game, Real& out, bool open) {
  if (sys < cx.systems.size()) {
    if (game >= cx.systems[sys].games.size()) return false;
    out = {sys, game};
    return true;
  }
  if (!cx.is_collection(sys)) return false;
  const System& c = cx.sys_at(sys);
  if (game >= c.games.size()) return false;
  const std::string path = c.games[game].path;
  const std::string name(c.games[game].src_system);
  const size_t hint = c.games[game].src_idx;
  for (size_t i = 0; i < cx.systems.size(); ++i) {
    if (cx.systems[i].name != name) continue;
    if (!open && !cx.systems[i].loaded && cx.systems[i].games.empty()) return false;
    cx.open_system(i);
    const std::vector<Game>& gs = cx.systems[i].games;
    if (hint < gs.size() && gs[hint].path == path) {
      out = {i, hint};
      return true;
    }
    for (size_t j = 0; j < gs.size(); ++j)
      if (gs[j].path == path) {
        out = {i, j};
        return true;
      }
    return false;
  }
  return false;
}

void resync(Ctx& cx, size_t sys) {
  if (sys < cx.systems.size() || !cx.is_collection(sys)) return;
  System& c = cx.sys_at(sys);
  smartcol::Options o;
  o.mode = uimode::current();
  std::vector<Game> keep;
  keep.reserve(c.games.size());
  for (Game& g : c.games) {
    const System* src = source_system(cx, g);
    if (!src || (!src->loaded && src->games.empty())) {  // not read: nothing of it can have changed
      keep.push_back(std::move(g));
      continue;
    }
    const Game* now = nullptr;
    if (g.src_idx < src->games.size() && src->games[g.src_idx].path == g.path) {
      now = &src->games[g.src_idx];
    } else {
      for (size_t j = 0; j < src->games.size() && !now; ++j)
        if (src->games[j].path == g.path) now = &src->games[j];
    }
    if (!now || !admit(*now, o) || !qualifies(c.name, *now)) continue;  // deleted, hidden, or no longer a member
    Game n = *now;
    n.src_system = g.src_system;
    n.src_idx = uint32_t(now - src->games.data());
    keep.push_back(std::move(n));
  }
  c.games = std::move(keep);
  c.count = c.games.size();
  const long k = cx.item_of(sys);
  if (k >= 0) cx.items[size_t(k)].games = uint32_t(c.count);
}

void request() { g_requested = true; }
bool requested() { return g_requested; }
void clear_request() { g_requested = false; }

}  // namespace zf::colsys
