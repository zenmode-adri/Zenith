// `menu.ui`: USER INTERFACE SETTINGS.
//
// ES's screen (GuiMenu::openUISettings + GuiGeneralScreensaverOptions) against zfront. Only what has an effect is shown:
//
//   ES entry                                 key (store)                     zfront
//   LANGUAGE (REGION) *                       system.language (system.cfg)    shown: i18n::set_language, live
//   SWITCH CONFIRM & CANCEL BUTTONS           InvertButtons (es)              shown: Pad::set_invert_buttons, live
//   SCREEN BRIGHTNESS *                       system.brightness (script)      shown when a backlight exists
//   SHOW CLOCK                                DrawClock (es)                  shown: ui_prefs, live
//   SHOW BATTERY STATUS                       ShowBattery (es)                shown when the console has a battery
//   START SCREENSAVER AFTER (0 = never)       ScreenSaverTime (es, ms)        shown: oscontract reads it every loop
//   SCREENSAVER TYPE                          ScreenSaverBehavior (es)        dim | black | suspend (suspend only where the sleep.sh hook exists)
//   THEME SET / THEME CONFIGURATION           ThemeSet, Theme*                hidden: zfront has one theme (Zenith)
//   GAMELIST VIEW STYLE                       GamelistViewStyle               hidden: zfront picks the view itself
//   ACCESS GAME OPTIONS WITH NORTH BUTTON     GameOptionsAtNorth              hidden: no game options screen yet
//   DISABLE TOUCHSCREEN                       DisableTouchscreen              hidden: zfront has no touch input
//   LIST / GAME LAUNCH TRANSITION             TransitionStyle, Game...        hidden: no transition styles
//   GAME MEDIAS DURING FAST SCROLLING         ScrollLoadMedias                hidden: fixed settle delay
//   ON-SCREEN HELP                            ShowHelpPrompts                 hidden: the help bars ignore it (see the report)
//   STOP MUSIC ON SCREENSAVER, video/slideshow screensavers, GAMELIST OPTIONS, ICONS    hidden: not in zfront
//   (* ES keeps these two in SYSTEM SETTINGS; the task puts them here.)
//
// Stores: `cx.es.set_*` + `save()` for es_settings.cfg, `cx.conf.persist` for system.cfg (shared lock), the `brightness`
// script (same one the keys and 006-display use) for the backlight. Nothing here starts a thread or a process by itself.
#include "menu_ui.hpp"

#include "menu_groups.hpp"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <optional>

#include "menu_lang.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
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
  s.build = [parts](MenuBuilder& m, Ctx& cx) {
    using namespace ui_part;
    // ---- language (live: the catalogs are mapped, the menus rebuild with the new strings)
    std::vector<Option> langs = available_language_options(ui_language_options());
    if ((parts & kLang) && langs.size() > 1) {
      m.header(tr("APPEARANCE"));
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

    // ---- screensaver: oscontract starts it when ScreenSaverTime ms pass without input (0 = never)
    if (!(parts & kSaver)) return;
    if (!(parts & kNoHdr)) m.header(tr("SCREENSAVER SETTINGS"));
    m.slider(
         tr("START SCREENSAVER AFTER"), 0, 120, 1, [&cx] { return int(std::min(std::max((cx.es.get_int("ScreenSaverTime", 300000) + 30000) / 60000, 0L), 120L)); },
         [&cx](int v) { cx.es.set_int("ScreenSaverTime", long(v) * 60000); })
        .format([](int v) { return v == 0 ? std::string(tr("OFF")) : std::to_string(v) + " min"; })
        .commit([&cx](int) { save_es(cx); });
    {  // ES: dim, black (+ video / slideshow, not ported) and suspend when the system can suspend
      std::vector<Option> types = {{"dim", tr("dim")}, {"black", tr("black")}};
      const bool can_suspend = oscontract::suspend_supported();
      if (can_suspend) types.push_back({"suspend", tr("suspend")});
      m.choice(
          tr("SCREENSAVER TYPE"), types,
          [&cx, can_suspend] {
            const std::string* v = cx.es.get("ScreenSaverBehavior");
            if (v && *v == "dim") return std::string("dim");
            return v && *v == "suspend" && can_suspend ? std::string("suspend") : std::string("black");
          },
          [&cx](const std::string& v) {
            cx.es.set_string("ScreenSaverBehavior", v);
            save_es(cx);
          });
    }
  };
  return s;
}

}  // namespace

MenuSpec ui_spec(unsigned parts) { return make_ui_spec(parts); }

ZF_REGISTER_SCREEN("menu.ui", [](Ctx&, const std::string&) { return std::make_unique<LangMenu>(make_ui_spec(ui_part::kAll)); });

void ui_reset_dry_state() { g_dry_percent.reset(); }

}  // namespace zf
