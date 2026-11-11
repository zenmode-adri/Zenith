// `menu.ui`: USER INTERFACE SETTINGS.
//
// ES's screen (GuiMenu::openUISettings + GuiGeneralScreensaverOptions) against zfront. Only what has an effect is shown:
//
//   ES entry                                 key (store)                     zfront
//   LANGUAGE (REGION) *                       system.language (system.cfg)    shown: i18n::set_language, live
//   SWITCH CONFIRM & CANCEL BUTTONS           InvertButtons (es)              shown: Pad::set_invert_buttons, live
//   SCREEN BRIGHTNESS *                       system.brightness (script)      shown when a backlight exists
//   SHOW CLOCK                                DrawClock (es)                  shown: ui_prefs, live
//   SHOW CLOCK IN 12-HOUR FORMAT              ClockMode12 (es)                shown: ui_prefs, live
//   CAROUSEL TRANSITIONS                      MoveCarousel (es)               shown: off = the carousel camera jumps
//   QUICK SYSTEM SELECT                       QuickSystemSelect (es)          shown: left / right in a game list = previous / next system
//   ON-SCREEN HELP                            ShowHelpPrompts (es)            shown: ui_prefs, the help bars are not drawn when off
//   SHOW BATTERY STATUS                       ShowBattery (es)                shown when the console has a battery
//   START SCREENSAVER AFTER (0 = never)       ScreenSaverTime (es, ms)        shown: oscontract reads it every loop
//   SCREENSAVER TYPE                          ScreenSaverBehavior (es)        dim | black | suspend (suspend only where the sleep.sh hook exists)
//   THEME SET                                 ThemeSet (es)                   shown; the choices of each set are kept like ES does (themecfg::switch_theme)
//   THEME CONFIGURATION                       Theme*, subset.* (es)           shown when the active ES theme has subsets (menu_theme_cfg.cpp); Zenith has none
//   GAMELIST VIEW STYLE                       GamelistViewStyle               hidden: zfront picks the view itself
//   ACCESS GAME OPTIONS WITH NORTH BUTTON     GameOptionsAtNorth              hidden: no game options screen yet
//   DISABLE TOUCHSCREEN                       DisableTouchscreen              hidden: zfront has no touch input
//   LIST / GAME LAUNCH TRANSITION             TransitionStyle, Game...        hidden: zfront has one screen transition (no styles to pick)
//   GAME MEDIAS DURING FAST SCROLLING         ScrollLoadMedias                hidden: fixed settle delay
//   GAMELIST OPTIONS (favourites on top, hidden files, file names, articles)  shown: listopts.cpp
//   STOP MUSIC ON SCREENSAVER                 StopMusicOnScreenSaver (es)     shown when frontend music is on and libSDL2_mixer loads (music.cpp)
//   video/slideshow screensavers, ICONS       hidden: not in zfront
//   (* ES keeps these two in SYSTEM SETTINGS; the task puts them here.)
//
// Stores: `cx.es.set_*` + `save()` for es_settings.cfg, `cx.conf.persist` for system.cfg (shared lock), the `brightness`
// script (same one the keys and 006-display use) for the backlight. Nothing here starts a thread or a process by itself.
#include "menu_ui.hpp"

#include "menu_groups.hpp"

#include <dirent.h>

#include "bootargs.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>

#include "listopts.hpp"
#include "menu_lang.hpp"
#include "music.hpp"
#include "menu_theme_cfg.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "remake.hpp"
#include "ssmedia.hpp"
#include "ui_osd.hpp"
#include "ui_prefs.hpp"
#include "util.hpp"

namespace zf {

i18n::Options& ui_language_options() {
  static i18n::Options o = i18n::default_options();
  return o;
}

const std::vector<LanguageName>& language_names() {
  // The code is what ES stores in system.language; the name is the language's own, as ES shows it (but in normal case).
  static const std::vector<LanguageName> v = {
      {"ar_YE", "العربية"},       {"ca_ES", "Català"},          {"cs_CZ", "Čeština"},      {"cy_GB", "Cymraeg"},
      {"de_DE", "Deutsch"},       {"el_GR", "Ελληνικά"},        {"en_US", "English (US)"}, {"en_GB", "English (UK)"},
      {"es_ES", "Español"},       {"es_MX", "Español mexicano"}, {"eu_ES", "Euskara"},     {"fi_FI", "Suomi"},
      {"fr_FR", "Français"},      {"gl_ES", "Galego"},          {"he_IL", "עברית"},        {"hu_HU", "Magyar"},
      {"id_ID", "Bahasa Indonesia"}, {"it_IT", "Italiano"},     {"ja_JP", "日本語"},        {"ko_KR", "한국어"},
      {"nb_NO", "Norsk bokmål"},  {"nl_NL", "Nederlands"},      {"nn_NO", "Norsk nynorsk"}, {"oc_FR", "Occitan"},
      {"pl_PL", "Polski"},        {"pt_BR", "Português brasileiro"}, {"pt_PT", "Português"}, {"ro_RO", "Română"},
      {"ru_RU", "Русский"},       {"sk_SK", "Slovenčina"},      {"sv_SE", "Svenska"},      {"tr_TR", "Türkçe"},
      {"uk_UA", "Українська"},    {"vi_VN", "Tiếng Việt"},      {"zh_CN", "简体中文"},      {"zh_TW", "正體中文"},
  };
  return v;
}

std::vector<Option> available_language_options(const i18n::Options& opts) {
  std::vector<std::string> have = i18n::available_languages(opts);
  auto installed = [&](const char* code) {
    for (const std::string& c : i18n::candidates(code))
      if (std::find(have.begin(), have.end(), c) != have.end()) return true;
    return false;
  };
  std::vector<Option> out;
  for (const LanguageName& l : language_names())
    if (std::string(l.code) == "en_US" || installed(l.code)) out.push_back({l.code, l.name});
  return out;
}

namespace {

// ---- language ---------------------------------------------------------------------------------------------------------

/// The option whose code ES would select for the stored value (`es` and `es_ES` are both Spanish).
std::string selected_language(const std::vector<Option>& opts, const std::string& stored) {
  if (stored.empty()) return "en_US";
  for (const Option& o : opts)
    if (o.value == stored) return o.value;
  for (const Option& o : opts)
    if (starts_with(o.value, stored + "_")) return o.value;  // "es" -> "es_ES", "en" -> "en_US"
  return stored;
}

// ---- backlight --------------------------------------------------------------------------------------------------------

struct Backlight {
  std::string file;  // .../brightness
  long max = 0;
};

/// First backlight (sorted by name, like ES) that has both files.
std::optional<Backlight> find_backlight() {
  const std::string dir = oscontract::paths().backlight_dir;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return std::nullopt;
  std::vector<std::string> names;
  while (dirent* e = ::readdir(d))
    if (e->d_name[0] != '.') names.emplace_back(e->d_name);
  ::closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string& n : names) {
    std::string base = path_join(dir, n), t;
    long mx = 0;
    if (read_file(base + "/max_brightness", t) && parse_int(trim(t), mx) && mx > 0 && file_exists(base + "/brightness")) return Backlight{base + "/brightness", mx};
  }
  return std::nullopt;
}

/// What the dry-run "wrote" last (nothing touches /sys in the harness and in tests, but the slider must keep its value).
std::optional<int> g_dry_percent;

int read_percent(const Backlight& b, const Ctx& cx) {
  if (cx.sv->cmd.dry_run && g_dry_percent) return *g_dry_percent;
  std::string t;
  long v = 0;
  if (!read_file(b.file, t) || !parse_int(trim(t), v)) return 50;
  return int(double(v) / double(b.max) * 100.0 + 0.5);
}

/// The `brightness` script's arithmetic (floor(percent * max / 100)); the value is stored by the script, see the commit.
void write_percent(Ctx& cx, const Backlight& b, int percent) {
  long raw = long(percent) * b.max / 100;
  CmdRunner& c = cx.sv->cmd;
  if (c.dry_run || c.keep_log) c.log.push_back("echo " + std::to_string(raw) + " > " + b.file);
  if (c.dry_run) {
    g_dry_percent = percent;
    return;
  }
  if (FILE* f = std::fopen(b.file.c_str(), "w")) {
    std::fprintf(f, "%ld\n", raw);
    std::fclose(f);
  }
}

// ---- the screen -------------------------------------------------------------------------------------------------------

void save_es(Ctx& cx) {
  std::string err;
  if (!cx.es.save(&err)) cx.sv->toasts.push(tr("Could not save the settings"));
}

MenuSpec make_ui_spec(unsigned parts) {
  MenuSpec s;
  s.title = tr("USER INTERFACE SETTINGS");
  s.title_key = N_("USER INTERFACE SETTINGS");
  // A look option was changed: the frontend restarts ONCE, when the menu is left (it restarted at every pick, so trying the four options
  // meant four restarts). Shared by `build` (which runs again on every refresh) and `on_close`.
  auto look_dirty = std::make_shared<bool>(false);
  s.on_close = [look_dirty](Ctx& cx) {
    if (!*look_dirty) return;
    *look_dirty = false;
    cx.sv->toasts.push(tr("Restarting to apply the theme"));
    oscontract::quit(oscontract::Mode::Quit);  // systemd (Restart=always) starts the frontend again with the new look
  };
  s.build = [parts, look_dirty](MenuBuilder& m, Ctx& cx) {
    using namespace ui_part;
    // ---- language (live: the catalogs are mapped, the menus rebuild with the new strings)
    std::vector<Option> langs = available_language_options(ui_language_options());
    bool appearance = false;  // the APPEARANCE header is already there
    if ((parts & kLang) && langs.size() > 1) {
      m.header(tr("APPEARANCE"));
      appearance = true;
      m.choice(
           tr("LANGUAGE (REGION)"), langs, [&cx, langs] { return selected_language(langs, cx.conf.get_or("system.language", "")); },
           [&cx](const std::string& code) {
             std::string err;
             if (!cx.conf.persist("system.language", code, &err)) {
               cx.sv->toasts.push(tr("Could not save the settings"));
               return;
             }
             i18n::set_language(code, ui_language_options());  // LangMenu rebuilds the open menus
           })
          .no_cycle();
    }

    // ---- theme: ThemeSet (es_settings.cfg). The theme is read once at start, so the frontend restarts to show it.
    if (parts & kTheme) {
      std::vector<boot::ThemeChoice> sets = boot::list_theme_sets();
      if (sets.size() > 1) {
        if (!appearance) m.header(tr("APPEARANCE"));
        appearance = true;
        std::vector<Option> opts;
        for (const boot::ThemeChoice& t : sets) opts.push_back({t.value, t.label});
        m.choice(
             tr("THEME SET"), opts,
             [&cx, opts] {
               const std::string* v = cx.es.get("ThemeSet");
               const std::string cur = v ? *v : std::string("es-theme-zenith");
               for (const Option& o : opts)
                 if (o.value == cur) return cur;
               return opts.front().value;
             },
             [&cx](const std::string& v) {
               const std::string* cur = cx.es.get("ThemeSet");
               if (cur && *cur == v) return;
               themecfg::switch_theme(cx, v);  // ES: ThemeSet plus the choices of the old set saved and those of the new one loaded
               save_es(cx);
               cx.sv->toasts.push(tr("Restarting to apply the theme"));
               oscontract::quit(oscontract::Mode::Quit);  // systemd (Restart=always) starts the frontend again with the new theme
             })
            .no_cycle()
            .hint(tr("The frontend restarts to apply the theme"));
      }

      // ES: THEME CONFIGURATION when the theme hasSubsets(). Not for a theme zfront draws itself: its drawing does not read them
      if (!remake::active(cx.th) && themecfg::available(cx, m.refresh)) {
        if (!appearance) m.header(tr("APPEARANCE"));
        appearance = true;
        m.submenu(tr("THEME CONFIGURATION"), "menu.ui.theme");
      }

      // The user's own look over the theme's: variables only (no images), so any theme can be bent a little. Empty = the theme's own.
      if (!appearance) m.header(tr("APPEARANCE"));
      appearance = true;
      auto own = [&](const std::string& title, const std::string& key, std::vector<Option> opts) {
        opts.insert(opts.begin(), Option{"", tr("Theme default")});
        m.choice(
             title, opts,
             [&cx, key, opts] {
               const std::string cur = cx.conf.get_or(key, "");
               for (const Option& o : opts)
                 if (o.value == cur) return cur;
               return std::string();
             },
             [&cx, key, look_dirty](const std::string& v) {
               if (cx.conf.get_or(key, "") == v) return;
               std::string err;
               if (!(v.empty() ? cx.conf.persist_remove(key, &err) : cx.conf.persist(key, v, &err))) {
                 cx.sv->toasts.push(tr("Could not save the settings"));
                 return;
               }
               *look_dirty = true;  // saved now; the restart waits for the menu to close
             })
            .no_cycle()
            .hint(tr("The frontend restarts to apply the theme"));
      };
      own(tr("SELECTOR STYLE"), "zfront.sel_style",
          {{"fill", tr("Filled")}, {"outline", tr("Outline")}, {"underline", tr("Underline")}, {"bar", tr("Side bar")}});
      own(tr("TITLE ALIGNMENT"), "zfront.header_align", {{"left", tr("Left")}, {"center", tr("Centered")}});
      own(tr("CAROUSEL CARDS"), "zfront.car_count", {{"3", "3"}, {"5", "5"}, {"7", "7"}});
      own(tr("LIST TEXT CASE"), "zfront.list_case", {{"upper", tr("Uppercase")}, {"lower", tr("Lowercase")}});

      // Accessibility: both live (es_settings.cfg; ui_prefs::reapply publishes them and every open menu lays out again).
      m.choice(
           tr("TEXT SIZE"), {{"small", tr("SMALL")}, {"normal", tr("NORMAL")}, {"large", tr("LARGE")}, {"xlarge", tr("EXTRA LARGE")}},
           [&cx] {
             const std::string* v = cx.es.get("TextSize");
             if (v && (*v == "small" || *v == "large" || *v == "xlarge")) return *v;
             return std::string("normal");
           },
           [&cx](const std::string& v) {
             cx.es.set_string("TextSize", v);
             save_es(cx);
             ui_prefs::reapply(cx);
           })
          .no_cycle()
          .hint(tr("Size of the text in menus, dialogs and help bars"));
      m.toggle(
           tr("HIGH CONTRAST"), [&cx] { return cx.es.get_bool("HighContrast", false); },
           [&cx](bool on) {
             cx.es.set_bool("HighContrast", on);
             save_es(cx);
             ui_prefs::reapply(cx);
           })
          .hint(tr("Pure white or black text, solid panels and a thicker selector"));
    }

    // ---- controls
    if (parts & kControl) {
    m.header(tr("CONTROL OPTIONS"));
    m.toggle(
         tr("SWITCH CONFIRM & CANCEL BUTTONS"), [&cx] { return cx.es.get_bool("InvertButtons", true); },
         [&cx](bool on) {
           cx.es.set_bool("InvertButtons", on);
           save_es(cx);
           if (cx.pad) cx.pad->set_invert_buttons(on);
         })
        .hint(tr("Switches the South and East buttons' functionality"));
    }

    // ---- display
    if (parts & (kBright | kDisplay)) m.header(tr("DISPLAY OPTIONS"));
    else if (parts & kTopBar) m.header(tr("TOP BAR"));
    if (std::optional<Backlight> bl = (parts & kBright) ? find_backlight() : std::nullopt) {
      Backlight b = *bl;
      m.slider(
           tr("SCREEN BRIGHTNESS"), 5, 100, 1, [&cx, b] { return read_percent(b, cx); },
           [&cx, b](int v) {
             write_percent(cx, b, v);
             osd_show_level(cx, "brightness", float(v) / 100.0f);
           })
          .unit("%")
          // stores system.brightness (what the keys, 006-display and sleep.sh read) through the same script they use
          .commit([&cx](int v) { cx.sv->cmd.spawn("brightness set " + std::to_string(v)); });
    }
    if (parts & (kDisplay | kTopBar)) m.toggle(
        tr("SHOW CLOCK"), [&cx] { return cx.es.get_bool("DrawClock", true); },
        [&cx](bool on) {
          cx.es.set_bool("DrawClock", on);
          save_es(cx);
          ui_prefs::reapply(cx);
        });
    if (parts & (kDisplay | kTopBar)) {
      m.toggle(
          tr("SHOW CLOCK IN 12-HOUR FORMAT"), [&cx] { return cx.es.get_bool("ClockMode12", false); },
          [&cx](bool on) {
            cx.es.set_bool("ClockMode12", on);
            save_es(cx);
            ui_prefs::reapply(cx);
          });
      if ((parts & (kDisplay | kTopBar)) && ui_prefs::has_battery())
        m.choice(
            tr("SHOW BATTERY STATUS"), {{"", tr("NO")}, {"icon", tr("ICON")}, {"text", tr("ICON AND TEXT")}},
            [&cx] {
              const std::string* v = cx.es.get("ShowBattery");  // ES: empty = no, "text" = icon and percentage, anything else = icon
              if (!v) return std::string("text");
              if (v->empty() || *v == "text") return *v;
              return std::string("icon");
            },
            [&cx](const std::string& v) {
              cx.es.set_string("ShowBattery", v);
              save_es(cx);
              ui_prefs::reapply(cx);
            });

      m.header(tr("NAVIGATION"));  // not about the top bar: they were under its header
      m.toggle(
          tr("CAROUSEL TRANSITIONS"), [&cx] { return cx.es.get_bool("MoveCarousel", true); },
          [&cx](bool on) {
            cx.es.set_bool("MoveCarousel", on);
            save_es(cx);
          });
      m.toggle(
          tr("QUICK SYSTEM SELECT"), [&cx] { return cx.es.get_bool("QuickSystemSelect", true); },
          [&cx](bool on) {
            cx.es.set_bool("QuickSystemSelect", on);
            save_es(cx);
          });
      m.toggle(
          tr("ON-SCREEN HELP"), [&cx] { return cx.es.get_bool("ShowHelpPrompts", true); },
          [&cx](bool on) {
            cx.es.set_bool("ShowHelpPrompts", on);
            save_es(cx);
            ui_prefs::reapply(cx);
          });
    }
    if (parts == kAll) listopts::build_group(m, cx);  // GAMELIST OPTIONS (listopts.cpp)

    // ---- screensaver: oscontract starts it when ScreenSaverTime ms pass without input (0 = never)
    if (!(parts & kSaver)) return;
    if (!(parts & kNoHdr)) m.header(tr("SCREENSAVER SETTINGS"));
    m.slider(
         tr("START SCREENSAVER AFTER"), 0, 120, 1, [&cx] { return int(std::min(std::max((cx.es.get_int("ScreenSaverTime", 300000) + 30000) / 60000, 0L), 120L)); },
         [&cx](int v) { cx.es.set_int("ScreenSaverTime", long(v) * 60000); })
        .format([](int v) { return v == 0 ? std::string(tr("OFF")) : std::to_string(v) + " min"; })
        .commit([&cx](int) { save_es(cx); });
    {  // ES: dim, black, random video, slideshow and suspend when the system can suspend
      auto shown = [](const char* t) { return i18n::sentence_case(utf8_upper(tr(t))); };  // ES's lower-case words: "Dim", "Random video"
      std::vector<Option> types = {{"dim", shown("dim")}, {"black", shown("black")}, {"random video", shown("random video")}, {"slideshow", shown("slideshow")}};
      const bool can_suspend = oscontract::suspend_supported();
      if (can_suspend) types.push_back({"suspend", shown("suspend")});
      m.choice(
          tr("SCREENSAVER TYPE"), types,
          [&cx, can_suspend] {
            const std::string* v = cx.es.get("ScreenSaverBehavior");
            if (v && (*v == "dim" || *v == "random video" || *v == "slideshow")) return *v;
            return v && *v == "suspend" && can_suspend ? std::string("suspend") : std::string("black");
          },
          [&cx, again = m.refresh](const std::string& v) {
            cx.es.set_string("ScreenSaverBehavior", v);
            save_es(cx);
            if (again) again();  // the rows of the slideshow / random video appear or go
          });
    }
    if (music::available()) music::build_saver_row(m, cx);
    ssmedia::build_rows(m, cx);  // IMAGE/VIDEO DURATION, SHOW GAME INFO, ... for the chosen type (nothing for dim / black / suspend)
  };
  return s;
}

}  // namespace

MenuSpec ui_spec(unsigned parts) { return make_ui_spec(parts); }

ZF_REGISTER_SCREEN("menu.ui", [](Ctx&, const std::string&) { return std::make_unique<LangMenu>(make_ui_spec(ui_part::kAll)); });

void ui_reset_dry_state() { g_dry_percent.reset(); }

}  // namespace zf
