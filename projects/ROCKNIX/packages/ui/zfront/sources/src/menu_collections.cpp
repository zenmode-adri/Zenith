// `menu.collections` (ES `GuiCollectionSystemsOptions`, "GAME COLLECTION SETTINGS") and its sub screens, plus
// `menu.collections.game` (favourite / add to a collection for one game; the game options menu links it).
//
//   menu.collections                    main screen
//   menu.collections.systems            SYSTEMS DISPLAYED          HiddenSystems
//   menu.collections.auto               AUTOMATIC GAME COLLECTIONS CollectionSystemsAuto
//   menu.collections.custom             CUSTOM GAME COLLECTIONS    CollectionSystemsCustom + filter editing + delete
//   menu.collections.filter  (arg name) dynamic collection filter  <name>.xcc
//   menu.collections.filter.systems     (arg name)                 <name>.xcc
//   menu.collections.delete             delete a collection        custom-<name>.cfg / <name>.xcc
//   menu.collections.game  (arg "<system>|<game path>")            favorite / custom-<name>.cfg
//
// What ES does that is not here: GROUPED SYSTEMS and "don't show groups with only one system" (the console's es_systems.cfg has no
// <group>), arcade-by-manufacturer collections, the full GuiGamelistFilter (zfront edits text, systems and favourites only), the jump
// into the new collection after creating it, UPDATE GAMELISTS (Windows only in ES), and the manufacturer/hardware/year orderings of
// SORT SYSTEMS (the console's es_systems.cfg has no manufacturer, so ES offers only NO and ALPHABETICALLY there too).
#include <algorithm>
#include <memory>

#include "collections.hpp"
#include "menu.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf {
namespace {

using namespace collections;

// ---- settings helpers -------------------------------------------------------------------------------------------------------

bool es_save(Ctx& cx) {
  std::string err;
  if (cx.es.save(&err)) return true;
  cx.sv->toasts.push(tr("COULD NOT SAVE GAME DATA"), 4000);  // the same notice the gamelist writer uses
  return false;
}

void es_str(Ctx& cx, const char* key, const std::string& v) {
  cx.es.set_string(key, v);
  es_save(cx);
}

void es_flag(Ctx& cx, const char* key, bool v) {
  cx.es.set_bool(key, v);
  es_save(cx);
}

std::string es_text(const Ctx& cx, const char* key, const char* def) {
  const std::string* v = cx.es.get(key);
  return v ? *v : std::string(def);
}

std::string join(const std::vector<std::string>& v, char sep) {
  std::string o;
  for (const std::string& s : v) {
    if (!o.empty()) o += sep;
    o += s;
  }
  return o;
}

std::string label_of(const System& s) { return s.fullname.empty() ? s.name : s.fullname; }

std::string count_text(size_t on, size_t total) { return std::to_string(on) + "/" + std::to_string(total); }

void reload(bool on = true) { set_reload_needed(on); }

// ---- SYSTEMS DISPLAYED ------------------------------------------------------------------------------------------------------

std::unique_ptr<Screen> make_systems() {
  MenuSpec s;
  s.title = tr("SYSTEMS DISPLAYED");
  s.title_key = N_("SYSTEMS DISPLAYED");
  s.build = [](MenuBuilder& m, Ctx& cx) {
    for (const System& sys : cx.systems) {
      std::string name = sys.name;
      m.toggle(label_of(sys),
               [&cx, name] {
                 std::vector<std::string> h = hidden_systems(cx.es);
                 return std::find(h.begin(), h.end(), name) == h.end();
               },
               [&cx, name](bool on) {
                 std::vector<std::string> h = hidden_systems(cx.es);
                 set_member(h, name, !on);  // the order of the other names is kept
                 es_str(cx, "HiddenSystems", join(h, ';'));
                 reload();
               });
    }
  };
  return make_menu(std::move(s));
}

// ---- AUTOMATIC GAME COLLECTIONS ---------------------------------------------------------------------------------------------

std::unique_ptr<Screen> make_auto() {
  MenuSpec s;
  s.title = tr("AUTOMATIC GAME COLLECTIONS");
  s.title_key = N_("AUTOMATIC GAME COLLECTIONS");
  s.build = [](MenuBuilder& m, Ctx& cx) {
    bool genres = false;
    m.header(tr("AUTOMATIC COLLECTIONS"));
    for (const AutoDecl& d : auto_decls()) {
      if (d.genre && !genres) {
        genres = true;
        m.header(tr("PER GENRE"));
      }
      std::string name = d.name;
      m.toggle(label(d),
               [&cx, name] {
                 std::vector<std::string> on = enabled_auto(cx.es);
                 return std::find(on.begin(), on.end(), name) != on.end();
               },
               [&cx, name](bool v) {
                 std::vector<std::string> on = enabled_auto(cx.es);
                 set_member(on, name, v);
                 es_str(cx, "CollectionSystemsAuto", join_sorted(on));
                 reload();
               });
    }
  };
  return make_menu(std::move(s));
}

// ---- dynamic collection filter ----------------------------------------------------------------------------------------------

struct FilterState {
  std::string name;
  Filter f;
};

void write_back(Ctx& cx, const std::shared_ptr<FilterState>& st) {
  std::string err;
  if (!write_filter(st->name, st->f, err)) cx.sv->toasts.push(tr("COULD NOT SAVE GAME DATA"), 4000);
  reload();
}

std::shared_ptr<FilterState> load_filter(const std::string& name) {
  auto st = std::make_shared<FilterState>();
  st->name = name;
  read_filter(name, st->f);
  return st;
}

std::unique_ptr<Screen> make_filter(const std::string& name) {
  if (!file_exists(xcc_path(name))) return nullptr;
  auto st = load_filter(name);
  MenuSpec s;
  s.title = name;
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    m.text(tr("FILTER GAMES BY TEXT"), [st] { return st->f.text; }, [st, &cx](const std::string& v) {
      st->f.text = v;
      write_back(cx, st);
    });
    m.toggle(tr("FAVORITES ONLY"), [st] { return st->f.only_favorites; }, [st, &cx](bool v) {
      st->f.only_favorites = v;
      write_back(cx, st);
    });
    std::string v = st->f.systems.empty() ? std::string(tr("ALL")) : count_text(st->f.systems.size(), cx.systems.size());
    m.submenu(tr("SYSTEMS"), "menu.collections.filter.systems", st->name).value(v);
  };
  return make_menu(std::move(s));
}

std::unique_ptr<Screen> make_filter_systems(const std::string& name) {
  if (!file_exists(xcc_path(name))) return nullptr;
  auto st = load_filter(name);
  MenuSpec s;
  s.title = tr("SYSTEMS");
  s.title_key = N_("SYSTEMS");
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    for (const System& sys : cx.systems) {
      std::string sname = sys.name;
      m.toggle(label_of(sys),
               [st, sname] {
                 return std::find(st->f.systems.begin(), st->f.systems.end(), sname) != st->f.systems.end();
               },
               [st, sname, &cx](bool on) {
                 set_member(st->f.systems, sname, on);
                 write_back(cx, st);
               });
    }
  };
  return make_menu(std::move(s));
}

// ---- CUSTOM GAME COLLECTIONS ------------------------------------------------------------------------------------------------

bool is_enabled_custom(const Ctx& cx, const std::string& name) {
  std::vector<std::string> on = enabled_custom(cx.es);
  return std::find(on.begin(), on.end(), name) != on.end();
}

void set_enabled_custom(Ctx& cx, const std::string& name, bool v) {
  std::vector<std::string> on = enabled_custom(cx.es);
  set_member(on, name, v);
  es_str(cx, "CollectionSystemsCustom", join_sorted(on));
  reload();
}

std::unique_ptr<Screen> make_custom() {
  MenuSpec s;
  s.title = tr("CUSTOM GAME COLLECTIONS");
  s.title_key = N_("CUSTOM GAME COLLECTIONS");
  s.build = [](MenuBuilder& m, Ctx& cx) {
    std::vector<Custom> all = list_custom();
    bool head = false;
    for (int pass = 0; pass < 2; ++pass) {  // ES lists the editable ones first, then the dynamic ones
      bool dyn = pass == 1;
      head = false;
      for (const Custom& c : all) {
        if (c.dynamic != dyn) continue;
        if (!head) {
          head = true;
          m.header(dyn ? tr("DYNAMIC COLLECTIONS") : tr("EDITABLE COLLECTIONS"));
        }
        std::string name = c.name;
        m.toggle(name, [&cx, name] { return is_enabled_custom(cx, name); }, [&cx, name](bool v) { set_enabled_custom(cx, name, v); });
      }
    }
    bool any_dyn = std::any_of(all.begin(), all.end(), [](const Custom& c) { return c.dynamic; });
    if (any_dyn) {
      m.header(tr("EDIT FILTERS"));
      for (const Custom& c : all)
        if (c.dynamic) m.submenu(c.name, "menu.collections.filter", c.name);
    }
    if (!all.empty()) {
      m.header(tr("DELETE COLLECTION"));
      m.submenu(tr("DELETE COLLECTION"), "menu.collections.delete").danger();
    }
  };
  return make_menu(std::move(s));
}

std::unique_ptr<Screen> make_delete() {
  MenuSpec s;
  s.title = tr("DELETE COLLECTION");
  s.title_key = N_("DELETE COLLECTION");
  s.build = [](MenuBuilder& m, Ctx& cx) {
    for (const Custom& c : list_custom()) {
      std::string name = c.name;
      Refresh refresh = m.refresh;
      m.action(name,
               [&cx, name, refresh] {
                 std::string err;
                 if (remove_custom(name, cx.es, err)) {
                   es_save(cx);
                   reload();
                   cx.sv->toasts.push(fmt(tr("Removed '%s' from '%s'"), name, tr("COLLECTIONS")));
                 } else {
                   cx.sv->toasts.push(err.empty() ? std::string(tr("COULD NOT SAVE GAME DATA")) : err, 4000);
                 }
                 if (refresh) refresh();
               })
          .danger()
          .confirm(fmt(tr("Are you sure you want to delete '%s'?"), name));
    }
  };
  return make_menu(std::move(s));
}

// ---- name entry for new collections -----------------------------------------------------------------------------------------

std::vector<std::string> names_in_use(Ctx& cx) {
  std::vector<std::string> v = reserved_names();
  for (const System& s : cx.systems) {
    v.push_back(s.name);
    v.push_back(s.theme);
  }
  return v;
}

Transition ask_new_collection(Ctx& cx, bool dynamic, Refresh refresh) {
  ui::KeyboardOpts o;
  o.title = tr("New Collection Name");
  o.max_len = 40;
  return ui::keyboard(o, "", [dynamic, refresh](Ctx& c, const std::string& typed) {
    std::string name = valid_new_name(typed, names_in_use(c));
    std::string err;
    bool ok = dynamic ? create_dynamic(name, Filter(), err) : create_editable(name, err);
    if (!ok) {
      c.sv->toasts.push(err.empty() ? std::string(tr("COULD NOT SAVE GAME DATA")) : err, 4000);
      return;
    }
    set_enabled_custom(c, name, true);  // ES enables the new collection
    c.sv->toasts.push(fmt(tr("Added '%s' to '%s'"), name, tr("COLLECTIONS")));
    if (refresh) refresh();
  });
}

// ---- main screen ------------------------------------------------------------------------------------------------------------

std::unique_ptr<Screen> make_main() {
  MenuSpec s;
  s.title = tr("GAME COLLECTION SETTINGS");
  s.title_key = N_("GAME COLLECTION SETTINGS");
  s.build = [](MenuBuilder& m, Ctx& cx) {
    // counts
    std::vector<std::string> hidden = hidden_systems(cx.es);
    size_t shown = 0;
    for (const System& sys : cx.systems)
      if (std::find(hidden.begin(), hidden.end(), sys.name) == hidden.end()) ++shown;
    std::vector<std::string> au = enabled_auto(cx.es);
    size_t n_auto = 0;
    for (const AutoDecl& d : auto_decls())
      if (std::find(au.begin(), au.end(), d.name) != au.end()) ++n_auto;
    std::vector<Custom> customs = list_custom();
    std::vector<std::string> cu = enabled_custom(cx.es);
    size_t n_custom = 0;
    for (const Custom& c : customs)
      if (std::find(cu.begin(), cu.end(), c.name) != cu.end()) ++n_custom;

    m.header(tr("COLLECTIONS TO DISPLAY"));
    m.submenu(tr("SYSTEMS DISPLAYED"), "menu.collections.systems").value(count_text(shown, cx.systems.size()));
    m.submenu(tr("AUTOMATIC GAME COLLECTIONS"), "menu.collections.auto").value(std::to_string(n_auto));
    if (!customs.empty()) m.submenu(tr("CUSTOM GAME COLLECTIONS"), "menu.collections.custom").value(count_text(n_custom, customs.size()));

    m.header(tr("CREATE CUSTOM COLLECTION"));
    Refresh refresh = m.refresh;
    m.action(tr("CREATE NEW EDITABLE COLLECTION"), [refresh](Ctx& c) { return ask_new_collection(c, false, refresh); });
    m.action(tr("CREATE NEW DYNAMIC COLLECTION"), [refresh](Ctx& c) { return ask_new_collection(c, true, refresh); })
        .hint(tr("Set its filters afterwards in Custom game collections."));

    m.header(tr("OPTIONS"));
    m.choice(tr("SORT SYSTEMS"), {{"", tr("NO")}, {"alpha", tr("ALPHABETICALLY")}},
             [&cx] {
               std::string v = es_text(cx, "SortSystems", "manufacturer");
               return v == "alpha" ? v : std::string();  // ES shows the first item when the stored order is not offered
             },
             [&cx](const std::string& v) {
               es_str(cx, "SortSystems", v);
               reload();
             });

    std::vector<Option> start = {{"", tr("NONE")}, {"lastsystem", tr("RESTORE LAST SELECTED")}};
    for (const System& sys : cx.systems)
      if (std::find(hidden.begin(), hidden.end(), sys.name) == hidden.end()) start.push_back({sys.name, label_of(sys)});
    m.choice(tr("START ON SYSTEM"), std::move(start),
             [&cx] { return es_text(cx, "StartupSystem", "lastsystem"); },
             [&cx](const std::string& v) {
               cx.es.set_string("StartupSystem", v);
               cx.es.set_string("LastSystem", "");  // ES forgets the remembered system
               es_save(cx);
             })
        .no_cycle();
    m.toggle(tr("START ON GAMELIST"), [&cx] { return cx.es.get_bool("StartupOnGameList", false); }, [&cx](bool v) { es_flag(cx, "StartupOnGameList", v); });

    if (!customs.empty())
      m.choice(tr("GROUP CUSTOM COLLECTIONS"), {{"", tr("UNTHEMED")}, {"false", tr("NEVER")}, {"true", tr("ALWAYS")}},
               [&cx] { return es_text(cx, "UseCustomCollectionsSystemEx", ""); },
               [&cx](const std::string& v) {
                 es_str(cx, "UseCustomCollectionsSystemEx", v);
                 reload();
               });
    m.toggle(tr("SHOW SYSTEM NAME IN COLLECTIONS"), [&cx] { return cx.es.get_bool("CollectionShowSystemInfo", true); }, [&cx](bool v) {
      es_flag(cx, "CollectionShowSystemInfo", v);
      reload();
    });
    if (!hidden.empty())
      m.toggle(tr("SHOW GAMES OF HIDDEN SYSTEMS IN COLLECTIONS"), [&cx] { return cx.es.get_bool("HiddenSystemsShowGames", false); }, [&cx](bool v) {
        es_flag(cx, "HiddenSystemsShowGames", v);
        reload();
      });
    m.toggle(tr("SHOW EMPTY SYSTEMS"), [&cx] { return cx.es.get_bool("LoadEmptySystems", false); }, [&cx](bool v) {
      es_flag(cx, "LoadEmptySystems", v);
      reload();
    });
  };
  s.on_close = [](Ctx& cx) {
    if (!reload_needed()) return;
    // The carousel is built at start. Whoever owns the system loader installs a handler; until then the change waits for a restart.
    cx.sv->toasts.push(tr("Restart the front-end to apply the changes."), 3500);
  };
  return make_menu(std::move(s));
}

// ---- one game: favourite and collections ------------------------------------------------------------------------------------

std::unique_ptr<Screen> make_game(Ctx& cx, const std::string& arg) {
  size_t si = 0, gi = 0;
  // same argument as the savestates screen: "<system name>|<game path>"
  size_t bar = arg.find('|');
  if (bar == std::string::npos) return nullptr;
  bool found = false;
  for (size_t i = 0; i < cx.systems.size() && !found; ++i) {
    if (cx.systems[i].name != arg.substr(0, bar)) continue;
    cx.open_system(i);
    for (size_t j = 0; j < cx.systems[i].games.size(); ++j)
      if (cx.systems[i].games[j].path == arg.substr(bar + 1)) {
        si = i, gi = j, found = true;
        break;
      }
  }
  if (!found) return nullptr;
  std::string path = cx.systems[si].games[gi].path;
  MenuSpec s;
  s.title = tr("COLLECTIONS");
  s.title_key = N_("COLLECTIONS");
  s.build = [si, gi, path](MenuBuilder& m, Ctx& cx2) {
    if (si >= cx2.systems.size() || gi >= cx2.systems[si].games.size()) return;
    const Game& g = cx2.systems[si].games[gi];
    bool fav = g.meta.favorite;
    m.action(fmt(fav ? tr("REMOVE FROM %s") : tr("ADD TO %s"), tr("FAVORITES")), [si, gi, &cx2] {
      toggle_favorite(cx2, si, gi);
      return Transition::pop();
    });
    for (const Custom& c : list_custom()) {
      if (c.dynamic || !is_enabled_custom(cx2, c.name)) continue;
      std::string name = c.name;
      bool in = contains(name, path);
      m.action(fmt(in ? tr("REMOVE FROM %s") : tr("ADD TO %s"), utf8_upper(name)), [name, path, in, &cx2] {
        std::string err;
        bool ok = in ? remove_game(name, path, err) : add_game(name, path, err);
        if (!ok) {
          cx2.sv->toasts.push(err.empty() ? std::string(tr("COULD NOT SAVE GAME DATA")) : err, 4000);
        } else {
          std::string title = path_stem(path);
          cx2.sv->toasts.push(fmt(in ? tr("Removed '%s' from '%s'") : tr("Added '%s' to '%s'"), title, utf8_upper(name)), 2500);
          reload();
        }
        return Transition::pop();
      });
    }
  };
  return make_menu(std::move(s));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.collections", [](Ctx&, const std::string&) { return make_main(); });
ZF_REGISTER_SCREEN("menu.collections.systems", [](Ctx&, const std::string&) { return make_systems(); });
ZF_REGISTER_SCREEN("menu.collections.auto", [](Ctx&, const std::string&) { return make_auto(); });
ZF_REGISTER_SCREEN("menu.collections.custom", [](Ctx&, const std::string&) { return make_custom(); });
ZF_REGISTER_SCREEN("menu.collections.delete", [](Ctx&, const std::string&) { return make_delete(); });
ZF_REGISTER_SCREEN("menu.collections.filter", [](Ctx&, const std::string& arg) { return make_filter(arg); });
ZF_REGISTER_SCREEN("menu.collections.filter.systems", [](Ctx&, const std::string& arg) { return make_filter_systems(arg); });
ZF_REGISTER_SCREEN("menu.collections.game", [](Ctx& cx, const std::string& arg) { return make_game(cx, arg); });

}  // namespace zf
