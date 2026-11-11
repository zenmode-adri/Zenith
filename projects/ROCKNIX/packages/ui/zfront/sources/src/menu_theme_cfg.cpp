#include "menu_theme_cfg.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>

#include "bootargs.hpp"
#include "menu_game.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "util.hpp"

namespace zf::themecfg {

namespace {

// ES GuiMenu.cpp: the subsets whose setting is not `subset.<name>`
struct Special {
  const char* subset;
  const char* key;
};
const Special kSpecial[] = {{"colorset", "ThemeColorSet"},       {"iconset", "ThemeIconSet"},           {"menu", "ThemeMenu"},
                            {"systemview", "ThemeSystemView"}, {"gamelistview", "ThemeGamelistView"}, {"region", "ThemeRegionName"}};

bool safe_name(const std::string& n) { return !n.empty() && n[0] != '.' && n.find('/') == std::string::npos && n.find('\\') == std::string::npos; }

std::string file_of(const std::string& theme_set) { return settings_dir() + "/" + theme_set + ".cfg"; }

std::string value_of(const EsSettings& es, const std::string& key) {
  const std::string* v = es.get(key);
  return v ? *v : std::string();
}

}  // namespace

std::string setting_key(const std::string& subset) {
  for (const Special& s : kSpecial)
    if (subset == s.subset) return s.key;
  return "subset." + subset;
}

std::vector<Row> rows_for(const std::vector<estheme::SubsetInfo>& infos) {
  std::vector<Row> rows;
  for (const estheme::SubsetInfo& i : infos) {
    if (i.options.empty()) continue;
    if (i.display == "-" && i.options.size() <= 1) continue;  // ES draws an empty line there
    Row r;
    r.subset = i.name;
    r.key = setting_key(i.name);
    r.label = i.display.empty() ? std::string(tr(("THEME " + utf8_upper(i.name)).c_str())) : i.display;  // ES: _("THEME COLORSET")...
    for (const estheme::SubsetOption& o : i.options) r.options.push_back({o.name, o.display});
    r.def = i.options.front().name;
    r.applies = !i.applies_to.empty() && !i.display.empty();
    rows.push_back(std::move(r));
  }
  // ES pushes the subsets that have an `appliesTo` to the end (stable sort on "has appliesTo")
  std::vector<bool> has;
  for (const estheme::SubsetInfo& i : infos)
    if (!i.options.empty() && !(i.display == "-" && i.options.size() <= 1)) has.push_back(!i.applies_to.empty());
  std::vector<size_t> order(rows.size());
  for (size_t k = 0; k < order.size(); ++k) order[k] = k;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return has[a] < has[b]; });
  std::vector<Row> sorted;
  for (size_t k : order) sorted.push_back(std::move(rows[k]));
  return sorted;
}

bool honored(const std::string& style, const std::string& subset) {
  if (style.empty()) return true;
  if (style == "iconic") return subset == "color-scheme" || subset == "play-video-previews-of-games" || subset == "show-detailed-game-info";
  return false;  // xmb, artbook, techdweeb: nothing of the ES theme's choices reaches their drawing
}

std::vector<Row> honored_rows(std::vector<Row> rows, const std::string& style) {
  rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const Row& r) { return !honored(style, r.subset); }), rows.end());
  return rows;
}

std::string current_value(const EsSettings& es, const Row& r) {
  const std::string v = value_of(es, r.key);
  for (const Option& o : r.options)
    if (o.value == v) return v;
  return r.def;
}

std::string stored_value(const Row& r, const std::string& chosen) { return chosen == r.def ? std::string() : chosen; }

// ---- themesettings/<ThemeSet>.cfg ---------------------------------------------------------------------------------------------------

std::string settings_dir() { return mg::env().rooted(mg::env().user_dir + "/themesettings"); }

void save_for(const Ctx& cx, const std::string& theme_set) {
  if (!safe_name(theme_set) || writes_blocked()) return;
  std::string data;
  auto line = [&](const std::string& name, const std::string& value) {
    if (!value.empty()) data += name + "=" + value + "\r";
  };
  for (const Special& s : kSpecial) line(s.subset, value_of(cx.es, s.key));
  const std::vector<std::string> names = cx.es.names();
  for (const std::string& n : names)
    if (starts_with(n, "subset.")) line(n, value_of(cx.es, n));  // also `subset.<system folder>.<subset>`, the per-system choices
  const std::string style = value_of(cx.es, "GamelistViewStyle");
  if (style != "automatic") line("GamelistViewStyle", style);
  line("DefaultGridSize", value_of(cx.es, "DefaultGridSize"));
  for (const std::string& n : names)
    if (ends_with(n, ".defaultView") || ends_with(n, ".gridSize")) line(n, value_of(cx.es, n));

  const std::string path = file_of(theme_set);
  if (data.empty()) {
    ::unlink(path.c_str());
    return;
  }
  ::mkdir(settings_dir().c_str(), 0755);
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return;
  const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) ::unlink(tmp.c_str());
}

void load_for(Ctx& cx, const std::string& theme_set) {
  if (!safe_name(theme_set)) return;
  std::string text;
  if (!read_file(file_of(theme_set), text)) return;
  size_t i = 0;
  while (i <= text.size()) {
    size_t e = text.find_first_of("\r\n", i);  // ES splits on '\r'; a file edited by hand may use '\n'
    if (e == std::string::npos) e = text.size();
    const std::string line = text.substr(i, e - i);
    i = e + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#' || line[0] == ';') continue;
    const std::string name = line.substr(0, eq), value = line.substr(eq + 1);
    if (name.empty() || value.empty()) continue;
    bool done = false;
    for (const Special& s : kSpecial)
      if (name == s.subset) {
        cx.es.set_string(s.key, value);
        done = true;
      }
    if (done) continue;
    if (name == "GamelistViewStyle" || name == "DefaultGridSize" || name.find(".defaultView") != std::string::npos || name.find(".gridSize") != std::string::npos ||
        starts_with(name, "subset."))
      cx.es.set_string(name, value);
  }
}

void clear_all(Ctx& cx) {
  auto clear = [&](const std::string& key) {
    if (!value_of(cx.es, key).empty()) cx.es.set_string(key, "");
  };
  for (const Special& s : kSpecial) clear(s.key);
  clear("GamelistViewStyle");
  clear("DefaultGridSize");
  for (const std::string& n : cx.es.names())
    if (starts_with(n, "subset.") || ends_with(n, ".defaultView") || ends_with(n, ".gridSize")) clear(n);
}

void switch_theme(Ctx& cx, const std::string& new_set) {
  const std::string old = value_of(cx.es, "ThemeSet");
  if (!old.empty()) save_for(cx, old);
  cx.es.set_string("ThemeSet", new_set);
  clear_all(cx);
  load_for(cx, new_set);
  oscontract::fire_event("theme-changed", {new_set, old});  // ES: Scripting::fireEvent("theme-changed", new, old)
}

// ---- the subsets of the active theme ------------------------------------------------------------------------------------------------

namespace {

struct Slot {
  bool ready = false;
  std::vector<estheme::SubsetInfo> infos;
};
std::map<std::string, std::shared_ptr<Slot>>& cache() {
  static std::map<std::string, std::shared_ptr<Slot>> c;
  return c;
}

std::vector<estheme::SubsetInfo> read_subsets(const std::string& dir, unsigned w, unsigned h, const std::map<std::string, std::string>& snap) {
  estheme::Options opt;
  opt.ratio = ratio_name(w, h);
  opt.width = w;
  opt.height = h;
  opt.setting = [&snap](const std::string& k) -> const std::string* {
    auto it = snap.find(k);
    return it == snap.end() ? nullptr : &it->second;
  };
  return estheme::list_subsets(dir, opt);
}

// The subsets of the theme ThemeSet names, or null while they are being read / when the theme has none to read (zfront's own Zenith).
// `wait`: read them now, on this thread, when they are not known yet.
const std::vector<estheme::SubsetInfo>* subsets(Ctx& cx, const Refresh& refresh, bool wait) {
  const std::string* set = cx.es.get("ThemeSet");
  const std::string dir = boot::resolve_es_theme(set ? *set : std::string(), mg::env().root);
  if (dir.empty()) return nullptr;
  const unsigned w = unsigned(cx.w), h = unsigned(cx.h);
  const std::string key = dir + "|" + std::to_string(w) + "x" + std::to_string(h);
  std::shared_ptr<Slot>& slot = cache()[key];
  if (!slot) {
    slot = std::make_shared<Slot>();
    auto snap = std::make_shared<std::map<std::string, std::string>>();  // the choices at this moment: the worker never reads `cx.es`
    for (const std::string& n : cx.es.names())
      if (starts_with(n, "Theme") || starts_with(n, "subset.")) (*snap)[n] = value_of(cx.es, n);
    if (wait || !cx.async_covers) {
      slot->infos = read_subsets(dir, w, h, *snap);
      slot->ready = true;
    } else {
      std::shared_ptr<Slot> s = slot;
      cx.sv->jobs.post([s, dir, w, h, snap] { s->infos = read_subsets(dir, w, h, *snap); },
                       [s, refresh] {
                         s->ready = true;
                         if (refresh) refresh();
                       });
    }
  }
  return slot->ready ? &slot->infos : nullptr;
}

// The rows of the active theme: ES's, less the ones zfront would not honor if the theme is one it redraws.
std::vector<Row> rows_of(Ctx& cx, const std::vector<estheme::SubsetInfo>& infos) {
  const std::string* set = cx.es.get("ThemeSet");
  const std::string dir = boot::resolve_es_theme(set ? *set : std::string(), mg::env().root);
  return honored_rows(rows_for(infos), estheme::remade_style(dir));
}

struct Draft {
  std::vector<Row> rows;
  std::map<std::string, std::string> chosen;  // setting key -> option picked in this visit
};

// ES's save function of the screen: the changed choices are stored (default = ""), and the frontend reloads once
void commit(Ctx& cx, Draft& d) {
  bool changed = false;
  for (const Row& r : d.rows) {
    auto it = d.chosen.find(r.key);
    if (it == d.chosen.end()) continue;
    const std::string v = stored_value(r, it->second);
    if (v != value_of(cx.es, r.key)) {
      cx.es.set_string(r.key, v);
      changed = true;
    }
  }
  d.chosen.clear();
  if (!changed) return;
  std::string err;
  if (!cx.es.save(&err)) {
    cx.sv->toasts.push(tr("Could not save the settings"));
    return;
  }
  cx.sv->toasts.push(tr("Restarting to apply the theme"));
  oscontract::quit(oscontract::Mode::Quit);  // systemd (Restart=always) starts the frontend again with the new choices
}

// ES TOOLS > RESET CUSTOMIZATIONS: the theme's choices (and the remembered file of the set) go away
void reset_customizations(Ctx& cx) {
  clear_all(cx);
  const std::string* set = cx.es.get("ThemeSet");
  if (set && safe_name(*set) && !writes_blocked()) ::unlink(file_of(*set).c_str());
  std::string err;
  if (!cx.es.save(&err)) {
    cx.sv->toasts.push(tr("Could not save the settings"));
    return;
  }
  cx.sv->toasts.push(tr("Restarting to apply the theme"));
  oscontract::quit(oscontract::Mode::Quit);
}

std::unique_ptr<Screen> make_screen(Ctx& cx, const std::string&) {
  const std::vector<estheme::SubsetInfo>* infos = subsets(cx, nullptr, true);
  if (!infos) return nullptr;
  auto d = std::make_shared<Draft>();
  d->rows = rows_of(cx, *infos);
  if (d->rows.empty()) return nullptr;
  MenuSpec s;
  s.title = tr("THEME CONFIGURATION");
  s.title_key = N_("THEME CONFIGURATION");
  s.build = [d](MenuBuilder& m, Ctx& c) {
    Ctx* cp = &c;  // the builder is gone when a getter runs
    int group = -1;  // 0 THEME OPTIONS, 1 GAMELIST THEME OPTIONS
    for (const Row& r : d->rows) {
      const int g = r.applies ? 1 : 0;
      if (g != group) {
        m.header(g == 1 ? tr("GAMELIST THEME OPTIONS") : tr("THEME OPTIONS"));
        group = g;
      }
      const std::string key = r.key;
      const Row row = r;
      m.choice(
           r.label, r.options,
           [d, row, cp] {
             auto it = d->chosen.find(row.key);
             return it != d->chosen.end() ? it->second : current_value(cp->es, row);
           },
           [d, key](const std::string& v) { d->chosen[key] = v; })
          .no_cycle();
    }
    m.header(tr("TOOLS"));
    m.action(tr("RESET CUSTOMIZATIONS"), [d](Ctx& c) {
      d->chosen.clear();
      reset_customizations(c);
      return Transition::pop();
    });
  };
  s.on_close = [d](Ctx& c) { commit(c, *d); };
  return make_menu(std::move(s));
}

}  // namespace

bool available(Ctx& cx, const Refresh& refresh) {
  const std::vector<estheme::SubsetInfo>* infos = subsets(cx, refresh, false);
  return infos && !rows_of(cx, *infos).empty();
}

ZF_REGISTER_SCREEN("menu.ui.theme", make_screen);

}  // namespace zf::themecfg
