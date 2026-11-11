// MODULE `listopts`: see listopts.hpp. ES: FileSorts.cpp (sort types, comparators), FileData::getChildrenListToDisplay (favourites first,
// then the sort), GuiMenu::openUISettings (GAMELIST OPTIONS), GuiGamelistOptions (SORT GAMES BY, filters).
#include "listopts.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <map>

#include "i18n.hpp"
#include "menu_lang.hpp"
#include "registry.hpp"
#include "vpreview.hpp"
#include "util.hpp"

namespace zf::listopts {

namespace {

std::atomic<unsigned> g_generation{0};

// A bool setting with the per-system override ES reads (`<system>.<key>` = "1" / "0", anything else = the global one).
bool bool_key(const EsSettings& es, const std::string& system, const std::string& key, bool def, bool per_system) {
  bool v = es.get_bool(key, def);
  if (per_system) {
    const std::string* s = es.get(system + "." + key);
    if (s && *s == "1") v = true;
    else if (s && *s == "0") v = false;
  }
  return v;
}

std::string lower(const std::string& s) { return utf8_lower(s); }

void save_es(Ctx& cx) {
  std::string err;
  if (!cx.es.save(&err)) cx.sv->toasts.push(tr("Could not save the settings"));
}

int players_of(const Game& g) { return std::atoi(std::string(g.meta.players).c_str()); }  // ES getInt: "1-2" is 1, empty is 0

}  // namespace

const std::vector<SortType>& sort_types() {
  static const std::vector<SortType> v = {
      {0, N_("FILENAME, ASCENDING")},        {1, N_("FILENAME, DESCENDING")},       {2, N_("RATING, ASCENDING")},
      {3, N_("RATING, DESCENDING")},         {4, N_("TIMES PLAYED, ASCENDING")},    {5, N_("TIMES PLAYED, DESCENDING")},
      {6, N_("LAST PLAYED, ASCENDING")},     {7, N_("LAST PLAYED, DESCENDING")},    {8, N_("NUMBER PLAYERS, ASCENDING")},
      {9, N_("NUMBER PLAYERS, DESCENDING")}, {10, N_("RELEASE DATE, ASCENDING")},   {11, N_("RELEASE DATE, DESCENDING")},
      {12, N_("GENRE, ASCENDING")},          {13, N_("GENRE, DESCENDING")},         {22, N_("GAME TIME, ASCENDING")},
      {23, N_("GAME TIME, DESCENDING")},
  };
  return v;
}

Opts read(const EsSettings& es, const std::string& system) {
  Opts o;
  o.favorites_first = bool_key(es, system, "FavoritesFirst", false, true);
  o.show_filenames = bool_key(es, system, "ShowFilenames", false, true);
  o.show_hidden = bool_key(es, system, "ShowHiddenFiles", false, false);
  o.ignore_articles = bool_key(es, system, "IgnoreLeadingArticles", true, false);
  const long id = es.get_int(system + ".sort", 0);
  o.sort = 0;
  for (const SortType& t : sort_types())
    if (t.id == id) o.sort = int(id);  // an id zfront cannot compute (developer, system...) is the name order
  return o;
}

Filter& filter_of(const std::string& system) {
  static std::map<std::string, Filter> m;
  return m[system];
}

unsigned generation() { return g_generation.load(); }
void changed() { ++g_generation; }

std::string display_name(const Game& g, const Opts& o) { return o.show_filenames ? path_stem(g.path) : g.name; }

std::vector<size_t> arrange(const std::vector<Game>& games, const Opts& o, const Filter& f, const std::string& query_lower, bool favorites_collection) {
  const std::string genre = lower(f.genre);
  std::vector<size_t> rows;
  rows.reserve(games.size());
  for (size_t i = 0; i < games.size(); ++i) {
    const Game& g = games[i];
    if (g.meta.hidden && !o.show_hidden) continue;  // hidden after the games were read (metadata editor): gone at once, as in ES
    if (!query_lower.empty() && lower(display_name(g, o)).find(query_lower) == std::string::npos) continue;
    if (f.favorites_only && !g.meta.favorite) continue;
    if (!genre.empty() && lower(std::string(g.meta.genre)) != genre) continue;
    rows.push_back(i);
  }
  // name order: the load order, unless the names shown are the file names (the sortname is not kept per game, so it does not count then)
  if (o.show_filenames) {
    std::vector<std::string> keys(games.size());
    for (size_t i : rows) keys[i] = sort_key_opt(path_stem(games[i].path), o.ignore_articles);
    std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) { return keys[a] < keys[b]; });
  }
  const bool desc = o.sort % 2 == 1 && o.sort < 22 ? true : o.sort == 23;
  const int kind = o.sort >= 22 ? 22 : o.sort & ~1;  // 0 name, 2 rating, 4 played, 6 last, 8 players, 10 release, 12 genre, 22 game time
  // keys computed once per row, not once per comparison (a string allocation each, n log n times, on the draw thread)
  std::vector<int> players_key;
  std::vector<std::string> genre_key;
  if (kind == 8) {
    players_key.resize(games.size());
    for (size_t i : rows) players_key[i] = players_of(games[i]);
  } else if (kind == 12) {
    genre_key.resize(games.size());
    for (size_t i : rows) genre_key[i] = lower(std::string(games[i].meta.genre));
  }
  auto cmp = [&](size_t a, size_t b) {  // a before b, ascending
    const Game &x = games[a], &y = games[b];
    switch (kind) {
      case 2: return x.meta.rating_pct < y.meta.rating_pct;
      case 4: return x.meta.playcount < y.meta.playcount;
      case 6: return x.meta.lastplayed < y.meta.lastplayed;
      case 8: return players_key[a] < players_key[b];
      case 10: return x.meta.release_year < y.meta.release_year;
      case 12: return genre_key[a] < genre_key[b];
      case 22: return x.meta.gametime < y.meta.gametime;
      default: return false;
    }
  };
  if (kind == 0) {
    if (desc) std::reverse(rows.begin(), rows.end());
  } else if (desc) {
    std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) { return cmp(b, a); });
  } else {
    std::stable_sort(rows.begin(), rows.end(), cmp);
  }
  if (o.favorites_first && !favorites_collection) std::stable_partition(rows.begin(), rows.end(), [&](size_t i) { return games[i].meta.favorite; });
  return rows;
}

void prepare(Ctx& cx, size_t si) {
  if (si >= cx.systems.size()) return;
  System& s = cx.systems[si];
  const Opts o = read(cx.es, s.name);
  LoadOptions lo;
  lo.show_hidden = o.show_hidden;
  lo.ignore_articles = o.ignore_articles;
  set_load_options(lo);
  if (s.loaded && s.load_opts != load_tag()) {
    s.games = load_games(s.rom_dir, s.exts, s.gamelist, &s.gamelist_damaged);
    s.count = s.games.size();
    s.load_opts = load_tag();
    const long k = cx.item_of(si);
    if (k >= 0) cx.items[size_t(k)].games = uint32_t(s.count);
  }
}

namespace {

void set_flag(Ctx& cx, const char* key, bool on) {
  cx.es.set_bool(key, on);
  save_es(cx);
  changed();
}

}  // namespace

void build_group(MenuBuilder& m, Ctx& cx, bool header) {
  if (header) m.header(tr("GAMELIST OPTIONS"));
  Ctx* c = &cx;
  m.toggle(tr("SHOW FAVORITES ON TOP"), [c] { return c->es.get_bool("FavoritesFirst", false); }, [c](bool on) { set_flag(*c, "FavoritesFirst", on); });
  m.toggle(tr("SHOW HIDDEN FILES"), [c] { return c->es.get_bool("ShowHiddenFiles", false); }, [c](bool on) { set_flag(*c, "ShowHiddenFiles", on); });
  m.toggle(tr("SHOW FILENAMES INSTEAD"), [c] { return c->es.get_bool("ShowFilenames", false); }, [c](bool on) { set_flag(*c, "ShowFilenames", on); });
  m.toggle(tr("IGNORE LEADING ARTICLES WHEN SORTING"), [c] { return c->es.get_bool("IgnoreLeadingArticles", true); },
           [c](bool on) { set_flag(*c, "IgnoreLeadingArticles", on); })
      .hint(tr("Ignore 'The' and 'A' if at the start."));
  vpreview::build_preview_row(m, cx);  // SHOW VIDEO PREVIEWS (ES: GAMELIST OPTIONS)
  vpreview::build_audio_row(m, cx);    // ENABLE VIDEO PREVIEW AUDIO (ES: SOUND SETTINGS; it sat under the battery warning)
}

namespace {

// `menu.list_options` (arg = system name): ES's gamelist options. SORT GAMES BY is stored in `<system>.sort`; the filters last for the session.
MenuSpec list_options_spec(const std::string& system) {
  MenuSpec s;
  s.title = tr("GAMELIST OPTIONS");
  s.title_key = N_("GAMELIST OPTIONS");
  s.build = [system](MenuBuilder& m, Ctx& cx) {
    Ctx* c = &cx;
    const std::string sys = system;
    if (sys.empty()) {  // from the library menu: no system, only the switches
      build_group(m, cx, false);
      return;
    }
    std::vector<Option> sorts;
    for (const SortType& t : sort_types()) sorts.push_back({std::to_string(t.id), tr(t.label)});
    m.choice(
         tr("SORT GAMES BY"), sorts, [c, sys] { return std::to_string(read(c->es, sys).sort); },
         [c, sys](const std::string& v) {
           c->es.set_int(sys + ".sort", std::atol(v.c_str()));
           save_es(*c);
           changed();
         })
        .no_cycle();
    m.header(tr("FILTER BY"));
    m.toggle(tr("FAVORITES ONLY"), [sys] { return filter_of(sys).favorites_only; }, [sys](bool on) {
      filter_of(sys).favorites_only = on;
      changed();
    });
    // the genres of the system, one entry per spelling-without-case
    std::vector<Option> genres = {{"", tr("ALL")}};
    std::vector<std::string> seen;
    std::vector<std::pair<std::string, std::string>> spelt;  // lower-case key -> the first spelling met ("Action", not "action")
    for (const std::vector<System>* group : {&cx.systems, &cx.cols}) {  // a collection is a system of its own here
      for (const System& st : *group) {
        if (st.name != sys) continue;
        for (const Game& g : st.games) {
          const std::string gen(g.meta.genre);
          if (gen.empty()) continue;
          const std::string k = utf8_lower(gen);
          if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
          seen.push_back(k);
          spelt.emplace_back(k, gen);
        }
      }
    }
    {
      // the filter in force stays in the list even when no game has that genre any more (its last game was edited): the row then
      // says what filters the list instead of ALL over an empty list
      const std::string now = utf8_lower(filter_of(sys).genre);
      if (!now.empty() && std::find(seen.begin(), seen.end(), now) == seen.end()) seen.push_back(now);
    }
    std::sort(seen.begin(), seen.end());
    for (const std::string& k : seen) {
      std::string shown = k;
      for (const auto& kv : spelt)
        if (kv.first == k) {
          shown = kv.second;
          break;
        }
      genres.push_back({k, shown});
    }
    std::vector<Option> opts = genres;
    m.choice(
         tr("GENRE"), opts,
         [sys, opts] {
           const std::string cur = utf8_lower(filter_of(sys).genre);
           for (const Option& o : opts)
             if (o.value == cur) return cur;
           return std::string();
         },
         [sys](const std::string& v) {
           filter_of(sys).genre = v;
           changed();
         })
        .no_cycle();
    m.header(tr("LIST"));
    build_group(m, cx, false);
  };
  return s;
}

}  // namespace

ZF_REGISTER_SCREEN("menu.list_options", [](Ctx&, const std::string& arg) { return std::make_unique<LangMenu>(list_options_spec(arg)); });

}  // namespace zf::listopts
