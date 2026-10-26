// menu-game: the options of one system or one game (`menu.system_options`, `menu.game_options`) and the screens they open
// (`menu.game.sysopt`, `menu.game.latency`, `menu.game.features`). ES: `GuiMenu::popSpecificConfigurationGui`,
// `openSystemOptionsConfiguration`, `openLatencyReductionConfiguration`, `addFeatures`/`addFeatureItem`.
//
// Keys: `<system>.<key>` for a system and `<system>["<rom file>"].<key>` for a game, exactly as `setsettings.sh` /
// `runemu.sh` read them (`get_setting`: game, then system, then global).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

#include "i18n.hpp"
#include "menu_game_int.hpp"
#include "registry.hpp"
#include "util.hpp"

namespace zf::mg {
namespace detail {

// ---- bound entries ---------------------------------------------------------------------------------------------------

std::vector<Option> tri_options() { return {{"auto", tr("AUTO")}, {"1", tr("ON")}, {"0", tr("OFF")}}; }

Entry& conf_choice(MenuBuilder& m, Ctx& cx, std::string label, std::string key, std::vector<Option> options, std::string def) {
  std::vector<Option> opts = options;
  auto has = [opts](const std::string& v) {
    return std::any_of(opts.begin(), opts.end(), [&](const Option& o) { return o.value == v; });
  };
  Ctx* c = &cx;
  return m.choice(std::move(label), std::move(options),
                  [c, key, def, has] {
                    std::string v = conf_get(*c, key);
                    if (v.empty() || ((v == "auto" || v == "default") && !has(v))) return def;
                    return v;
                  },
                  [c, key](const std::string& v) { conf_put(*c, key, v); });
}

Entry& conf_tri(MenuBuilder& m, Ctx& cx, std::string label, std::string key) { return conf_choice(m, cx, std::move(label), std::move(key), tri_options(), "auto"); }

Entry& conf_switch(MenuBuilder& m, Ctx& cx, std::string label, std::string key, std::string on_value, std::string off_value) {
  Ctx* c = &cx;
  return m.toggle(std::move(label), [c, key, on_value] { return conf_get(*c, key) == on_value; },
                  [c, key, on_value, off_value](bool on) { conf_put(*c, key, on ? on_value : off_value); });
}

Entry& conf_text(MenuBuilder& m, Ctx& cx, std::string label, std::string key, bool secret) {
  Ctx* c = &cx;
  Entry& e = m.text(std::move(label), [c, key] { return conf_get(*c, key); }, [c, key](const std::string& v) { conf_put(*c, key, v); });
  if (secret) e.secret();
  return e;
}

// ---- lists -----------------------------------------------------------------------------------------------------------

std::vector<Option> shader_options(bool* loaded) {
  std::vector<Option> o = {{"auto", tr("AUTO")}, {"none", tr("NONE")}};
  for (const std::string& s : shader_list(loaded)) o.push_back({s, s});
  return o;
}

std::vector<Option> filter_options(bool* loaded) {
  std::vector<Option> o = {{"auto", tr("AUTO")}, {"none", tr("NONE")}};
  for (const std::string& s : filter_list(loaded)) o.push_back({s, s});
  return o;
}

std::vector<Option> decoration_options() {
  std::vector<Option> o = {{"", tr("AUTO")}, {"none", tr("NONE")}};
  for (const std::string& s : decoration_sets()) o.push_back({s, s});
  return o;
}

void ensure_lists(Ctx& cx, const Refresh& refresh, bool shaders, bool filters) {
  bool sl = false, fl = false;
  shader_list(&sl);
  filter_list(&fl);
  bool need_s = shaders && !sl, need_f = filters && !fl;
  if (!need_s && !need_f) return;
  if (cx.sv->cmd.dry_run) {  // tests and the preview harness: deterministic, no thread
    if (need_s) load_shader_list();
    if (need_f) load_filter_list();
    if (refresh) refresh();
    return;
  }
  cx.sv->jobs.post(
      [need_s, need_f] {
        if (need_s) load_shader_list();
        if (need_f) load_filter_list();
      },
      [refresh] {
        if (refresh) refresh();
      });
}

namespace {

bool shaders_available() {
  bool loaded = false;
  const auto& l = shader_list(&loaded);
  return loaded ? !l.empty() : is_dir(env().rooted(env().shaders_dir));
}

bool filters_available() {
  bool loaded = false;
  const auto& l = filter_list(&loaded);
  return loaded ? !l.empty() : is_dir(env().rooted(env().filters_dir));
}

}  // namespace

std::string feature_label(const std::string& name) {
  const char* t = i18n::ctr("game_options", name.c_str());
  return utf8_upper(t ? t : name.c_str());  // like the rest of the menu: ES shows the labels in capitals
}

// ---- es_features.cfg items -------------------------------------------------------------------------------------------

namespace {

std::string inherited_value(const Ctx& cx, const std::string& storage, const std::string& stored) {
  if (starts_with(storage, "global.")) return {};
  std::string system_setting = storage;
  bool query_system = false;
  size_t gs = storage.find("[\"");
  if (gs != std::string::npos) {
    size_t ge = storage.find("\"]");
    if (ge != std::string::npos) {
      system_setting = storage.substr(0, gs) + storage.substr(ge + 2);
      query_system = true;
    }
  }
  std::string inherited;
  size_t dot = system_setting.find('.');
  if (dot != std::string::npos) {
    std::string gv = conf_get(cx, "global." + system_setting.substr(dot + 1));
    if (!gv.empty() && gv != "auto" && gv != stored) inherited = gv;
  }
  if (query_system) {
    std::string sv = conf_get(cx, system_setting);
    if (!sv.empty() && sv != "auto" && sv != stored) inherited = sv;
  }
  return inherited;
}

int to_int(const std::string& s, int def) {
  float f;
  return parse_float(s, f) ? int(std::lround(f)) : def;
}

std::vector<std::string> split_space(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ') ++j;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

std::string hint_for(const Feature& f, const std::string& inherited_display) {
  std::string d = f.description.empty() ? std::string() : std::string(i18n::ctr("game_options", f.description.c_str()));
  if (!inherited_display.empty()) {
    std::string cur = std::string(tr("Current setting")) + ": " + inherited_display;
    d = d.empty() ? cur : d + "  " + cur;
  }
  return d;
}

std::string display_of(const std::vector<Option>& opts, const std::string& value) {
  for (const Option& o : opts)
    if (o.value == value) return o.label;
  return {};
}

bool add_item(Sec& s, Ctx& cx, const Feature& f, const std::string& cfg) {
  if (f.preset == "hidden") return false;
  std::string storage = cfg + "." + f.value;
  if (cfg == "global" && starts_with(f.value, "global.")) storage = f.value;
  std::string label = feature_label(f.name);
  const std::string& p = f.preset;

  // text-like presets (the file browsers of ES become typed paths: zfront has no file browser)
  if (p == "input" || p == "password" || p == "image" || p == "video" || p == "folder" || p == "document" || p == "files") {
    conf_text(s.b(), cx, label, storage, p == "password");
    return true;
  }
  // presets that need tools the OS does not have here
  if (p == "videomodes" || p == "videomode" || p == "runners") return false;

  std::string stored = conf_get(cx, storage);
  Ctx* c = &cx;

  if (p == "switch" || p == "switch_default_off" || p == "switchon" || p == "switch_default_on" || p == "switchoff" || p == "switch_default_off_reverse_value") {
    // "switch": on = "1", off = no line. "switchon": on = no line (default), off = "0". "switchoff": on = no line, off = "1".
    bool reverse_on = (p == "switchon" || p == "switch_default_on");
    bool reverse_off = (p == "switchoff" || p == "switch_default_off_reverse_value");
    Entry* e;
    if (reverse_on) e = &s.b().toggle(label, [c, storage] { return conf_get(*c, storage) != "0"; }, [c, storage](bool on) { conf_put(*c, storage, on ? "" : "0"); });
    else if (reverse_off) e = &s.b().toggle(label, [c, storage] { return conf_get(*c, storage) != "1"; }, [c, storage](bool on) { conf_put(*c, storage, on ? "" : "1"); });
    else e = &s.b().toggle(label, [c, storage] { return conf_get(*c, storage) == "1"; }, [c, storage](bool on) { conf_put(*c, storage, on ? "1" : ""); });
    std::string h = hint_for(f, {});
    if (!h.empty()) e->hint(h);
    return true;
  }

  if (p == "switchauto") {
    std::vector<Option> o = {{"", tr("AUTO")}, {"1", tr("ON")}, {"0", tr("OFF")}};
    Entry& e = conf_choice(s.b(), cx, label, storage, o, "");
    std::string h = hint_for(f, {});
    if (!h.empty()) e.hint(h);
    return true;
  }

  if (p == "slider" || p == "sliderauto") {
    bool autov = p == "sliderauto";
    std::vector<std::string> t = split_space(f.preset_parameters);
    int from = t.size() > 0 ? to_int(t[0], 0) : 0, to = t.size() > 1 ? to_int(t[1], 100) : 100, step = t.size() > 2 ? std::max(to_int(t[2], 1), 1) : 1;
    int def = (!autov && t.size() > 3) ? to_int(t[3], 0) : 0;
    std::string suffix = autov ? (t.size() > 3 ? t[3] : std::string()) : (t.size() > 4 ? t[4] : std::string());
    int lo = autov ? from - step : from;  // the value below the first step means AUTO
    int hi = std::max(to, from);
    Entry& e = s.b().slider(label, lo, hi, step,
                            [c, storage, autov, lo, def] {
                              std::string v = conf_get(*c, storage);
                              if (v.empty()) return autov ? lo : def;
                              return to_int(v, autov ? lo : def);
                            },
                            [c, storage, autov, lo](int v) {
                              if (autov && v <= lo) conf_put(*c, storage, "");
                              else conf_put(*c, storage, std::to_string(v));
                            });
    e.unit(suffix);
    if (autov) {
      std::string au = tr("AUTO");
      e.format([lo, au, suffix](int v) { return v <= lo ? au : std::to_string(v) + suffix; });
    }
    std::string h = hint_for(f, {});
    if (!h.empty()) e.hint(h);
    return true;
  }

  // choices
  std::vector<Option> opts;
  bool needs_filters = false, needs_shaders = false;
  if (p == "shaders" || p == "shaderset") {
    bool l;
    opts = shader_options(&l);
    needs_shaders = true;
    if (!shaders_available()) return false;
  } else if (p == "videofilters") {
    bool l;
    opts = filter_options(&l);
    needs_filters = true;
    if (!filters_available()) return false;
  } else if (p == "decorations" || p == "bezel") {
    opts = decoration_options();
    if (opts.size() <= 2) return false;
    opts[0].value = "auto";
  } else {
    opts.push_back({"", tr("AUTO")});
    for (const FeatureChoice& ch : f.choices) opts.push_back({ch.value, feature_label(ch.name)});
  }
  (void)needs_filters;
  (void)needs_shaders;
  std::string def = opts.empty() ? std::string() : opts[0].value;
  std::string inherited = inherited_value(cx, storage, stored);
  Entry& e = conf_choice(s.b(), cx, label, storage, opts, def);
  std::string h = hint_for(f, inherited.empty() ? std::string() : display_of(opts, inherited));
  if (!h.empty()) e.hint(h);
  return true;
}

std::string submenu_summary(const Ctx& cx, const std::string& cfg, const std::vector<Feature>& items) {
  std::string d;
  for (const Feature& f : items) {
    std::string stored = conf_get(cx, cfg + "." + f.value);
    if (stored.empty()) continue;
    std::string shown = stored;
    for (const FeatureChoice& ch : f.choices)
      if (ch.value == stored) {
        shown = feature_label(ch.name);
        break;
      }
    if (f.preset == "switchoff" || f.preset == "switch_default_off_reverse_value") shown = stored == "0" ? tr("ON") : stored == "1" ? tr("OFF") : shown;
    else if (starts_with(f.preset, "switch")) shown = stored == "0" ? tr("OFF") : stored == "1" ? tr("ON") : shown;
    if (!d.empty()) d += "  ·  ";
    d += feature_label(f.name) + ": " + shown;
  }
  return d;
}

}  // namespace

std::string feat_arg(char kind, const std::string& system, const std::string& rom_path, const std::string& submenu) {
  std::string a(1, kind);
  a += '\t';
  a += system;
  a += '\t';
  a += rom_path;
  a += '\t';
  a += submenu;
  return a;
}

void add_features(Sec& s, Ctx& cx, const std::vector<Feature>& items, const std::string& cfg, const std::string& default_group, bool group_only_if_not_first, const std::string& feat_target) {
  // groups in order of first appearance
  std::vector<std::string> groups;
  for (const Feature& f : items)
    if (std::find(groups.begin(), groups.end(), f.group) == groups.end()) groups.push_back(f.group);
  bool first = true;
  for (const std::string& g : groups) {
    if (!g.empty()) s.header(feature_label(g));
    else if (!default_group.empty() && (!group_only_if_not_first || !first)) s.header(default_group);
    first = false;
    std::set<std::string> done;
    for (const Feature& f : items) {
      if (f.group != g) continue;
      if (f.submenu.empty()) {
        add_item(s, cx, f, cfg);
        continue;
      }
      if (!done.insert(f.submenu).second) continue;
      std::vector<Feature> sub;
      for (const Feature& x : items)
        if (x.preset != "hidden" && x.submenu == f.submenu) sub.push_back(x);
      if (sub.empty()) continue;
      Entry& e = s.b().submenu(feature_label(f.submenu), "menu.game.features", feat_target + f.submenu);
      std::string d = submenu_summary(cx, cfg, sub);
      if (!d.empty()) e.hint(d);
    }
  }
}

}  // namespace detail

// ---- the screens -----------------------------------------------------------------------------------------------------
namespace {

using namespace detail;

struct Target {
  std::string system, rom_path, rom_file, cfg;
  bool game = false;
};

bool make_target(const std::string& arg, bool game, Target& t) {
  if (game) {
    if (!split_game_arg(arg, t.system, t.rom_path)) return false;
    t.rom_file = path_filename(t.rom_path);
    t.cfg = game_cfg(t.system, t.rom_file);
    t.game = true;
  } else {
    t.system = arg;
    t.cfg = arg;
  }
  return !t.system.empty();
}

std::string default_core(const System& sys, const std::string& emu) {
  if (emu == sys.emulator) return sys.core;
  for (const auto& e : sys.emus)
    if (e.first == emu) return e.second.empty() ? std::string() : e.second.front();
  return {};
}

std::vector<Option> emulator_options(const System& sys) {
  std::vector<Option> o = {{"", tr("AUTO")}};
  for (const auto& e : sys.emus) {
    if (e.second.empty()) {
      o.push_back({e.first, e.first});
      continue;
    }
    for (const std::string& c : e.second) {
      std::string nice = c;
      std::replace(nice.begin(), nice.end(), '_', ' ');
      o.push_back({e.first + "/" + c, e.first == c ? e.first : e.first + ": " + nice});
    }
  }
  return o;
}

const char* kColorizations[] = {
    "GB - DMG", "GB - Light", "GB - Pocket", "GBC - Blue", "GBC - Brown", "GBC - Dark Blue", "GBC - Dark Brown", "GBC - Dark Green", "GBC - Grayscale",
    "GBC - Green", "GBC - Inverted", "GBC - Orange", "GBC - Pastel Mix", "GBC - Red", "GBC - Yellow", "SGB - 1A", "SGB - 1B", "SGB - 1C", "SGB - 1D",
    "SGB - 1E", "SGB - 1F", "SGB - 1G", "SGB - 1H", "SGB - 2A", "SGB - 2B", "SGB - 2C", "SGB - 2D", "SGB - 2E", "SGB - 2F", "SGB - 2G", "SGB - 2H",
    "SGB - 3A", "SGB - 3B", "SGB - 3C", "SGB - 3D", "SGB - 3E", "SGB - 3F", "SGB - 3G", "SGB - 3H", "SGB - 4A", "SGB - 4B", "SGB - 4C", "SGB - 4D",
    "SGB - 4E", "SGB - 4F", "SGB - 4G", "SGB - 4H", "Special 1", "Special 2", "Special 3", "TWB01 - 756 Production", "TWB02 - AKB48 Pink",
    "TWB03 - Angry Volcano", "TWB04 - Anime Expo", "TWB05 - Aqours Blue", "TWB06 - Aquatic Iro", "TWB07 - Bandai Namco", "TWB08 - Blossom Pink",
    "TWB09 - Bubbles Blue", "TWB10 - Builder Yellow", "TWB11 - Buttercup Green", "TWB12 - Camouflage", "TWB13 - Cardcaptor Pink", "TWB14 - Christmas",
    "TWB15 - Crunchyroll Orange", "TWB16 - Digivice", "TWB17 - Do The Dew", "TWB18 - Eevee Brown", "TWB19 - Fruity Orange", "TWB20 - Game.com",
    "TWB21 - Game Grump Orange", "TWB22 - GameKing", "TWB23 - Game Master", "TWB24 - Ghostly Aoi", "TWB25 - Golden Wild", "TWB26 - Green Banana",
    "TWB27 - Greenscale", "TWB28 - Halloween", "TWB29 - Hero Yellow", "TWB30 - Hokage Orange", "TWB31 - Labo Fawn", "TWB32 - Legendary Super Saiyan",
    "TWB33 - Lemon Lime Green", "TWB34 - Lime Midori", "TWB35 - Mania Plus Green", "TWB36 - Microvision", "TWB37 - Million Live Gold",
    "TWB38 - Miraitowa Blue", "TWB39 - NASCAR", "TWB40 - Neo Geo Pocket", "TWB41 - Neon Blue", "TWB42 - Neon Green", "TWB43 - Neon Pink",
    "TWB44 - Neon Red", "TWB45 - Neon Yellow", "TWB46 - Nick Orange", "TWB47 - Nijigasaki Orange", "TWB48 - Odyssey Gold", "TWB49 - Patrick Star Pink",
    "TWB50 - Pikachu Yellow", "TWB51 - Pocket Tales", "TWB52 - Pokemon mini", "TWB53 - Pretty Guardian Gold", "TWB54 - S.E.E.S. Blue",
    "TWB55 - Saint Snow Red", "TWB56 - Scooby-Doo Mystery", "TWB57 - Shiny Sky Blue", "TWB58 - Sidem Green", "TWB59 - Slime Blue",
    "TWB60 - Spongebob Yellow", "TWB61 - Stone Orange", "TWB62 - Straw Hat Red", "TWB63 - Superball Ivory", "TWB64 - Super Saiyan Blue",
    "TWB65 - Super Saiyan Rose", "TWB66 - Supervision", "TWB67 - Survey Corps Brown", "TWB68 - Tea Midori", "TWB69 - TI-83", "TWB70 - Tokyo Midtown",
    "TWB71 - Travel Wood", "TWB72 - Virtual Boy", "TWB73 - VMU", "TWB74 - Wisteria Murasaki", "TWB75 - WonderSwan", "TWB76 - Yellow Banana"};

const System* system_or_toast(const Ctx& cx, const std::string& name) { return find_system(cx, name); }

std::string game_title(const Ctx& cx, const System& sys, const Target& t) {
  for (const Game& g : sys.games)
    if (g.path == t.rom_path) return g.name;
  (void)cx;
  return path_stem(t.rom_file);
}

/// The body of the system / game options screen.
void build_config(MenuBuilder& m, Ctx& cx, const Target& t) {
  const System* sys = system_or_toast(cx, t.system);
  if (!sys) {
    m.info(tr("UNKNOWN SYSTEM"), t.system);
    return;
  }
  const FeatureDb& db = features();
  Pick rp = resolved_pick(*sys, cx.conf.map(), t.game ? t.rom_file : std::string());
  Resolved fe = db.resolve(*sys, rp.emu, rp.core);
  Sec s(m);
  Ctx* c = &cx;
  const std::string cfg = t.cfg;

  // emulator and core
  if (has_emulator_selection(*sys)) {
    std::vector<Option> opts = emulator_options(*sys);
    const System* sp = sys;
    Refresh refresh = m.refresh;
    m.choice(tr("EMULATOR"), opts,
             [c, sp, cfg] {
               Pick x = explicit_pick(*sp, c->conf.map(), cfg);
               if (x.emu.empty() && x.core.empty()) return std::string();
               Pick r = resolved_pick(*sp, c->conf.map(), std::string());
               std::string emu = x.emu.empty() ? r.emu : x.emu;
               std::string core = x.core.empty() ? default_core(*sp, emu) : x.core;
               return core.empty() ? emu : emu + "/" + core;
             },
             [c, cfg, refresh](const std::string& v) {
               std::string emu, core;
               size_t sl = v.find('/');
               emu = v.substr(0, sl);
               if (sl != std::string::npos) core = v.substr(sl + 1);
               conf_put(*c, cfg + ".emulator", emu);
               conf_put(*c, cfg + ".core", core);
               if (refresh) refresh();  // the options below depend on the emulator and core
             })
        .no_cycle()
        .hint(tr("The emulator and core that run this."));
  }
  m.submenu(tr("SYSTEM OPTIONS"), "menu.game.sysopt", cfg);

  // display
  s.header(tr("DISPLAY"));
  if (fe.has(F_ratio)) {
    std::vector<Option> o;
    for (const auto& r : ratios()) o.push_back({r.second, r.first[0] >= 'A' && r.first[0] <= 'Z' ? tr(r.first) : r.first});
    conf_choice(s.b(), cx, tr("GAME ASPECT RATIO"), cfg + ".ratio", o, "auto").hint(tr("Force the game to render in this aspect ratio."));
  }
  if (fe.has(F_smooth)) conf_tri(s.b(), cx, tr("SMOOTH GAMES (BILINEAR FILTERING)"), cfg + ".smooth");
  if (fe.has(F_pixel_perfect)) conf_tri(s.b(), cx, tr("INTEGER SCALING (PIXEL PERFECT)"), cfg + ".integerscale");
  if (fe.has(F_shaders)) {
    if (shaders_available()) {
      bool l;
      conf_choice(s.b(), cx, tr("SHADER SET"), cfg + ".shaderset", shader_options(&l), "auto");
    }
    if (filters_available()) {
      bool l;
      conf_choice(s.b(), cx, tr("VIDEO FILTER"), cfg + ".videofilter", filter_options(&l), "auto");
    }
  }
  if (fe.has(F_decoration)) {
    std::vector<Option> o = decoration_options();
    if (o.size() > 2) conf_choice(s.b(), cx, tr("DECORATION SET"), cfg + ".bezel", o, "");
  }
  if (fe.has(F_colorization)) {
    std::vector<Option> o = {{"auto", tr("AUTO")}, {"none", tr("NONE")}};
    for (const char* k : kColorizations) o.push_back({k, k});
    conf_choice(s.b(), cx, tr("COLORIZATION"), cfg + ".renderer.colorization", o, "auto");
  }
  // gameplay
  s.header(tr("GAMEPLAY"));
  if (fe.has(F_rewind)) conf_tri(s.b(), cx, tr("REWIND"), cfg + ".rewind").hint(tr("Store past states to rewind to in realtime, if the core supports it."));
  if (fe.has(F_autosave) && !fe.has_custom("autosave"))
    conf_tri(s.b(), cx, tr("AUTO SAVE/LOAD ON GAME LAUNCH"), cfg + ".autosave").hint(tr("Load latest save state on game launch and save state when exiting game."));
  if (fe.has(F_latency_reduction)) s.b().submenu(tr("LATENCY REDUCTION"), "menu.game.latency", cfg);
  // what es_features.cfg adds for this emulator / core
  add_features(s, cx, fe.custom, cfg, tr("SETTINGS"), true, feat_arg(t.game ? 'r' : 's', t.system, t.rom_path, std::string()));

  if (!s.any && !has_emulator_selection(*sys)) m.info(tr("NO OPTIONS"), std::string());
}

std::unique_ptr<Screen> make_config(Ctx& cx, const std::string& arg, bool game) {
  Target t;
  MenuSpec spec;
  if (!make_target(arg, game, t)) {
    spec.title = tr("ADVANCED SYSTEM OPTIONS");
    spec.title_key = N_("ADVANCED SYSTEM OPTIONS");
    spec.build = [](MenuBuilder& m, Ctx&) { m.info(tr("UNKNOWN SYSTEM"), std::string()); };
    return make_menu(std::move(spec));
  }
  const System* sys = find_system(cx, t.system);
  spec.title = sys ? (game ? game_title(cx, *sys, t) : sys->fullname) : t.system;
  spec.build = [t](MenuBuilder& m, Ctx& cx2) { build_config(m, cx2, t); };
  spec.on_open = [](Ctx& cx2, const Refresh& refresh) { ensure_lists(cx2, refresh, true, true); };
  return make_menu(std::move(spec));
}

// ---- menu.game.features (a submenu of custom options) ------------------------------------------------------------------
std::unique_ptr<Screen> make_features(Ctx& cx, const std::string& arg) {
  // arg = kind TAB system TAB rom TAB submenu
  std::vector<std::string> f;
  size_t i = 0;
  while (true) {
    size_t e = arg.find('\t', i);
    if (e == std::string::npos || f.size() == 3) {
      f.push_back(arg.substr(i));
      break;
    }
    f.push_back(arg.substr(i, e - i));
    i = e + 1;
  }
  MenuSpec spec;
  if (f.size() < 4 || f[0].empty()) {
    spec.title = tr("SETTINGS");
    spec.title_key = N_("SETTINGS");
    spec.build = [](MenuBuilder& m, Ctx&) { m.info(tr("NO OPTIONS"), std::string()); };
    return make_menu(std::move(spec));
  }
  char kind = f[0][0];
  std::string system = f[1], rom = f[2], sub = f[3];
  spec.title = feature_label(sub);
  spec.build = [kind, system, rom, sub](MenuBuilder& m, Ctx& cx2) {
    std::vector<Feature> items;
    std::string cfg;
    if (kind == 'g') {
      items = features().global();
      cfg = "global";
    } else {
      const System* sys = find_system(cx2, system);
      if (!sys) return;
      std::string file = path_filename(rom);
      cfg = kind == 'r' ? game_cfg(system, file) : system;
      Pick rp = resolved_pick(*sys, cx2.conf.map(), kind == 'r' ? file : std::string());
      items = features().resolve(*sys, rp.emu, rp.core).custom;
    }
    Sec s(m);
    for (const Feature& ft : items)
      if (ft.preset != "hidden" && ft.submenu == sub) add_item(s, cx2, ft, cfg);
    if (!s.any) m.info(tr("NO OPTIONS"), std::string());
  };
  spec.on_open = [](Ctx& cx2, const Refresh& refresh) { ensure_lists(cx2, refresh, true, true); };
  (void)cx;
  return make_menu(std::move(spec));
}

// ---- menu.game.sysopt ---------------------------------------------------------------------------------------------------
std::vector<Option> governor_options(const std::vector<std::string>& names, const std::string& current) {
  std::vector<Option> o;
  bool found = false;
  for (const std::string& n : names) {
    o.push_back({n, n});
    if (n == current) found = true;
  }
  if (!found && !current.empty()) o.push_back({current, current});
  return o;
}

std::unique_ptr<Screen> make_sysopt(Ctx&, const std::string& cfg) {
  MenuSpec spec;
  spec.title = tr("SYSTEM OPTIONS");
  spec.title_key = N_("SYSTEM OPTIONS");
  spec.build = [cfg](MenuBuilder& m, Ctx& cx) {
    Sec s(m);
    if (const char* fan = std::getenv("DEVICE_HAS_FAN"); fan && std::string(fan) == "true")
      conf_choice(s.b(), cx, tr("COOLING PROFILE"), cfg + ".cooling.profile",
                  {{"default", tr("DEFAULT")}, {"auto", tr("AUTO")}, {"quiet", tr("QUIET")}, {"moderate", tr("MODERATE")}, {"aggressive", tr("AGGRESSIVE")}, {"custom", tr("CUSTOM")}},
                  "default");
    std::string cur = conf_get(cx, cfg + ".cpugovernor");
    conf_choice(s.b(), cx, tr("CPU SCALING GOVERNOR"), cfg + ".cpugovernor", governor_options(cpu_governors(), cur), "default");
    cur = conf_get(cx, cfg + ".gpuperf");
    conf_choice(s.b(), cx, tr("GPU SCALING GOVERNOR"), cfg + ".gpuperf", governor_options(gpu_governors(), cur), "default");
    if (const char* mh = std::getenv("DEVICE_MANGOHUD_SUPPORT"); mh && std::string(mh) == "true")
      conf_choice(s.b(), cx, tr("MANGOHUD OVERLAY"), cfg + ".rocknix.mangohud.enabled", {{"", tr("DEFAULT")}, {"1", tr("ENABLED")}, {"0", tr("DISABLED")}}, "");
  };
  return make_menu(std::move(spec));
}

// ---- menu.game.latency ----------------------------------------------------------------------------------------------------
std::unique_ptr<Screen> make_latency(Ctx&, const std::string& cfg) {
  MenuSpec spec;
  spec.title = tr("LATENCY REDUCTION");
  spec.title_key = N_("LATENCY REDUCTION");
  spec.build = [cfg](MenuBuilder& m, Ctx& cx) {
    std::vector<Option> frames = {{"", tr("AUTO")}, {"0", tr("NONE")}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}, {"5", "5"}, {"6", "6"}};
    conf_choice(m, cx, tr("RUN-AHEAD FRAMES"), cfg + ".runahead", frames, "").hint(tr("High numbers can result in visible jitter."));
    std::vector<Option> onoff = {{"", tr("AUTO")}, {"1", tr("ON")}, {"0", tr("OFF")}};
    conf_choice(m, cx, tr("USE SECOND INSTANCE FOR RUN-AHEAD"), cfg + ".secondinstance", onoff, "").hint(tr("Can prevent audio skips on button presses."));
  };
  return make_menu(std::move(spec));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.system_options", [](Ctx& cx, const std::string& arg) { return make_config(cx, arg, false); });
ZF_REGISTER_SCREEN("menu.game_options", [](Ctx& cx, const std::string& arg) { return make_config(cx, arg, true); });
ZF_REGISTER_SCREEN("menu.game.features", [](Ctx& cx, const std::string& arg) { return make_features(cx, arg); });
ZF_REGISTER_SCREEN("menu.game.sysopt", [](Ctx& cx, const std::string& arg) { return make_sysopt(cx, arg); });
ZF_REGISTER_SCREEN("menu.game.latency", [](Ctx& cx, const std::string& arg) { return make_latency(cx, arg); });

}  // namespace zf::mg
