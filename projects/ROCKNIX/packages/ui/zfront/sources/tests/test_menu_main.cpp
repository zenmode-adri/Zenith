// menu.main, menu.quit and menu.ui (module `menu-main`): which entries show, what each action stores or runs, the language
// switch, the clock / battery preferences. Everything runs in a temp dir with a dry-run command runner.
// With ZF_MENU_PNG=<dir> (and the Zenith theme next to the checkout) it also renders the review PNGs of every screen at the
// four sizes, dark and light.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "alloc_nothrow.hpp"
#include "cpu.hpp"
#include "i18n.hpp"
#include "menu.hpp"
#include "menu_main.hpp"
#include "menu_ui.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "ui_osd.hpp"
#include "ui_prefs.hpp"
#include "util.hpp"

using namespace zf;

// Allocation counter: the steady frame of these menus must not allocate (they add only a string compare to Menu::draw).
static thread_local size_t g_allocs = 0;
void* operator new(size_t n) {
  ++g_allocs;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

std::string d;  // temp dir

struct Stub : Screen {
  Transition handle(Action, Ctx&) override { return Transition::none(); }
  void draw(Ctx&, std::vector<Op>&) override {}
};

struct FakePad : Pad {
  int calls = 0;
  bool last = true;
  void set_invert_buttons(bool invert) override { ++calls, last = invert; }
};

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string slurp(const std::string& p) {
  std::string s;
  read_file(p, s);
  return s;
}
void put(const std::string& p, const std::string& t) { std::ofstream(p) << t; }
void sh(const std::string& c) {
  if (std::system(c.c_str()) != 0) {
  }
}

bool g_suspend_helper = true;

/// A clean OS-contract state bound to `cx.sv` (dry-run): paths in the temp dir, commands only logged.
void bind_os(Ctx& cx) {
  oscontract::reset_for_test();
  ui_reset_dry_state();
  oscontract::Paths& p = oscontract::paths();
  p.backlight_dir = d + "/w/bl";
  p.ready_file = d + "/w/ready";
  p.kill_file = d + "/w/kill";
  p.tmp_dir = d + "/w";
  p.suspend_helper = d + (g_suspend_helper ? "/w/pm-suspend" : "/w/no-pm-suspend");
  oscontract::startup(cx.sv, true);
}

/// Fresh Ctx over temp settings files and a backlight; dry-run commands.
void fresh(Ctx& cx, bool backlight = true, bool suspend = true) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = true;
  sh("rm -rf " + d + "/w && mkdir -p " + d + "/w/bl/panel");
  put(d + "/w/system.cfg", "# test\nsystem.language=en_US\naudio.volume=40\n");
  put(d + "/w/es_settings.cfg", "");
  cx.conf = SystemConf::load(d + "/w/system.cfg");
  cx.es = EsSettings::load(d + "/w/es_settings.cfg");
  SystemConf::lock_path() = d + "/w/lock";
  if (backlight) {
    put(d + "/w/bl/panel/brightness", "128\n");
    put(d + "/w/bl/panel/max_brightness", "255\n");
  }
  g_suspend_helper = suspend;
  if (suspend) put(d + "/w/pm-suspend", "");
  bind_os(cx);
  cx.status = Status::demo("13:05");
  ui_prefs::on_status(cx, cx.status);
  ui_language_options() = i18n::Options{{std::string(ZF_TEST_DATA) + "/i18n"}, {"zfront"}};
  i18n::set_language("", ui_language_options());
}

Transition press(Screen& s, Ctx& cx, Action a) { return s.handle(a, cx); }

Menu& as_menu(std::unique_ptr<Screen>& s) { return *static_cast<Menu*>(s.get()); }

std::vector<std::string> labels(const Menu& m) {
  std::vector<std::string> v;
  for (size_t i = 0; i < m.size(); ++i) v.push_back(m.entry(i).label);
  return v;
}

/// What the UI shows for a label key (the English upper-case text production passes to tr()): translated, or sentence-cased.
std::string T(const char* key) { return std::string(tr(key)); }
std::vector<std::string> T(std::initializer_list<const char*> keys) {
  std::vector<std::string> v;
  for (const char* k : keys) v.push_back(T(k));
  return v;
}

/// Row whose shown text is exactly `text`; -1 when none.
int index_of_text(const Menu& m, const std::string& text) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == text) return int(i);
  return -1;
}

/// Row for the label key `key` (compared with what the menu shows for it, i.e. `tr(key)`).
int index_of(const Menu& m, const std::string& key) { return index_of_text(m, T(key.c_str())); }

/// Moves the cursor onto `label` with Down; false when it is not there.
bool goto_label(Menu& m, Ctx& cx, const std::string& label) {
  press(m, cx, Action::X);  // the first handle builds the menu (X does nothing)
  int want = index_of(m, label);
  if (want < 0) return false;
  for (size_t i = 0; i < m.size() + 1 && m.cursor() != want; ++i) press(m, cx, Action::Down);
  return m.cursor() == want;
}

void wait_commit(Menu& m, Ctx& cx) {
  m.tick(0.3f, cx);
  m.tick(0.3f, cx);
}

bool log_has(const Ctx& cx, const std::string& needle) {
  for (const std::string& l : cx.sv->cmd.log)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

const char* kAllScreens[] = {"menu.game",    "menu.g.screen",    "menu.g.power",  "menu.controllers", "menu.network",
                             "menu.g.library", "menu.g.system", "menu.quit"};

void render_all(const std::string& out);

}  // namespace

ZF_REGISTER_SCREEN("test.main.here", [](Ctx&, const std::string&) { return std::make_unique<Stub>(); });

int main() {
  char tpl[] = "/tmp/zf-mm-XXXXXX";
  CHECK(::mkdtemp(tpl) != nullptr);
  d = tpl;

  // ---- ui_prefs: the keys, the 12-hour text, the filter
  {
    CHECK_EQ(ui_prefs::clock12("13:05"), std::string("01:05 PM"));
    CHECK_EQ(ui_prefs::clock12("00:30"), std::string("12:30 AM"));
    CHECK_EQ(ui_prefs::clock12("12:00"), std::string("12:00 PM"));
    CHECK_EQ(ui_prefs::clock12("09:07"), std::string("09:07 AM"));
    CHECK_EQ(ui_prefs::clock12(""), std::string(""));
    CHECK_EQ(ui_prefs::clock12("24:00"), std::string("24:00"));
    CHECK_EQ(ui_prefs::clock12("ab:cd"), std::string("ab:cd"));
    ui_prefs::Prefs p = ui_prefs::read(EsSettings());
    CHECK(p.clock && !p.clock12 && p.battery == ui_prefs::BatteryMode::Text);
    EsSettings es = EsSettings::from_text("<bool name=\"DrawClock\" value=\"false\" /><bool name=\"ClockMode12\" value=\"true\" /><string name=\"ShowBattery\" value=\"\" />");
    p = ui_prefs::read(es);
    CHECK(!p.clock && p.clock12 && p.battery == ui_prefs::BatteryMode::Off);
    es.set_string("ShowBattery", "icon");
    CHECK(ui_prefs::read(es).battery == ui_prefs::BatteryMode::Icon);
    es.set_string("ShowBattery", "weird");  // ES: anything but "" and "text" is the icon
    CHECK(ui_prefs::read(es).battery == ui_prefs::BatteryMode::Icon);
    Status raw = Status::demo("15:42");
    ui_prefs::Prefs q;
    CHECK(ui_prefs::filter(raw, q) == raw);
    q.clock12 = true;
    CHECK_EQ(ui_prefs::filter(raw, q).time, std::string("03:42 PM"));
    q.clock = false;
    CHECK(ui_prefs::filter(raw, q).time.empty());
    q.battery = ui_prefs::BatteryMode::Off;
    CHECK(!ui_prefs::filter(raw, q).battery.has_value());
    CHECK(ui_prefs::filter(raw, q).wifi == raw.wifi);

    // App hands every status through it (freeze_status is the harness path; poll_status / the constructor use the same call)
    Ctx cx;
    fresh(cx);
    cx.es.set_bool("DrawClock", false);
    App app(std::move(cx), std::make_unique<Stub>(), make_status_source("/nonexistent"));
    app.freeze_status(Status::demo("08:15"));
    CHECK(app.cx.status.time.empty());
    CHECK(app.cx.status.battery.has_value());
    CHECK(ui_prefs::has_battery());
    app.cx.es.set_bool("DrawClock", true);
    ui_prefs::reapply(app.cx);
    CHECK_EQ(app.cx.status.time, std::string("08:15"));
  }

  // ---- menu.main: ES rows, exact screen names, hidden while the screen does not exist, demo only in dry-run
  {
    std::vector<MainRow> rows = main_rows();
    CHECK_EQ(rows.size(), size_t(8));
    for (size_t i = 0; i < rows.size(); ++i) CHECK_EQ(rows[i].screen, std::string(kAllScreens[i]));
    CHECK_EQ(rows[0].label, T("GAMES"));
    CHECK_EQ(rows[2].icon, std::string("battery"));

    Ctx cx;
    fresh(cx);
    CHECK(Registry::instance().has("menu.main"));
    CHECK(Registry::instance().has("menu.ui"));
    CHECK(Registry::instance().has("menu.quit"));
    auto real = Registry::instance().make("menu.main", cx, "");
    CHECK(real != nullptr);
    Menu& rm = as_menu(real);
    press(rm, cx, Action::X);
    CHECK_EQ(rm.title(), T("MAIN MENU"));
    size_t expect = 0;
    for (const char* s : kAllScreens) expect += Registry::instance().has(s) ? 1 : 0;
    expect += 1;  // WIDGET DEMO: dry-run
    CHECK(rm.size() >= expect);
    CHECK(index_of(rm, "USER INTERFACE SETTINGS") < 0);  // clock and battery are under SCREEN & SOUND, the buttons under CONTROLLER
    CHECK(index_of(rm, "SCREEN & SOUND") >= 0);
    CHECK(index_of(rm, "UPDATES & DOWNLOADS") < 0);  // now under SYSTEM
    CHECK(index_of(rm, "SCRAPER") < 0);              // now under LIBRARY
    CHECK(index_of(rm, "QUIT") >= 0);
    CHECK(index_of(rm, "WIDGET DEMO") >= 0);
    // the entries open their screens
    int ui = index_of(rm, "SCREEN & SOUND");
    for (int i = 0; i < ui + 2 && rm.cursor() != ui; ++i) press(rm, cx, Action::Down);
    Transition t = press(rm, cx, Action::A);
    CHECK(t.kind == Transition::Open && t.name == "menu.g.screen");

    // not on the device: no dry-run, no demo entry
    cx.sv->cmd.dry_run = false;
    auto dev = Registry::instance().make("menu.main", cx, "");
    press(*dev, cx, Action::X);
    CHECK(index_of(as_menu(dev), "WIDGET DEMO") < 0);
    CHECK_EQ(as_menu(dev).size(), rm.size() - 1);
    cx.sv->cmd.dry_run = true;

    // rows whose screen is not registered are dropped (a name nobody registers; one that exists)
    auto two = make_main_menu({{"HERE", "test.main.here", "ui"}, {"GONE", "test.main.gone", "quit"}}, false);
    press(*two, cx, Action::X);
    CHECK_EQ(as_menu(two).size(), size_t(1));
    CHECK_EQ(as_menu(two).entry(0).label, std::string("HERE"));
    // every screen the other agents write is registered later: a stub makes its row appear
    Registry::instance().add("test.main.gone", [](Ctx&, const std::string&) { return std::make_unique<Stub>(); });
    two = make_main_menu({{"HERE", "test.main.here", "ui"}, {"GONE", "test.main.gone", "quit"}}, false);
    press(*two, cx, Action::X);
    CHECK_EQ(as_menu(two).size(), size_t(2));
  }

  // ---- menu.quit: restart / reboot / suspend / shutdown, each behind a YES/NO question
  {
    Ctx cx;
    fresh(cx);
    auto q = Registry::instance().make("menu.quit", cx, "");
    Menu& m = as_menu(q);
    press(m, cx, Action::X);
    CHECK_EQ(m.title(), T("QUIT"));
    CHECK_EQ(labels(m), T({"RESTART THE FRONTEND", "RESTART SYSTEM", "SUSPEND SYSTEM", "SHUTDOWN SYSTEM"}));
    CHECK_EQ(m.entry(0).icon_name, std::string("restart"));
    CHECK_EQ(m.entry(3).icon_name, std::string("shutdown"));
    for (size_t i = 0; i < m.size(); ++i) CHECK(!m.entry(i).confirm_msg.empty());

    struct Case {
      int idx;
      oscontract::Mode mode;
      const char* event;
    } cases[] = {{0, oscontract::Mode::Quit, nullptr}, {1, oscontract::Mode::Reboot, "reboot"}, {3, oscontract::Mode::Shutdown, "shutdown"}};
    for (const Case& c : cases) {
      bind_os(cx);
      CHECK(!oscontract::quit_requested());
      // the question first; NO and B leave things as they are
      while (m.cursor() != c.idx) press(m, cx, Action::Down);
      Transition t = press(m, cx, Action::A);
      CHECK(t.kind == Transition::Push && t.screen);
      auto* dlg = static_cast<ui::ConfirmDialog*>(t.screen.get());
      CHECK_EQ(dlg->focus(), 1);
      CHECK(dlg->handle(Action::A, cx).kind == Transition::Pop);
      CHECK(!oscontract::quit_requested());
      // YES leaves the loop and remembers what to do after the window is gone
      dlg->handle(Action::Left, cx);
      Transition r = dlg->handle(Action::A, cx);
      CHECK(r.kind == Transition::Quit);
      CHECK(oscontract::quit_requested());
      CHECK_EQ(int(oscontract::quit_mode()), int(c.mode));
      if (c.event) {
        bool seen = false;
        for (const std::string& l : oscontract::dry_log()) seen = seen || l.find(c.event) != std::string::npos;
        CHECK(seen);
      }
    }

    // suspend: one `systemctl suspend`, no leaving
    bind_os(cx);
    cx.sv->cmd.log.clear();
    while (m.cursor() != 2) press(m, cx, Action::Down);
    Transition t = press(m, cx, Action::A);
    auto* dlg = static_cast<ui::ConfirmDialog*>(t.screen.get());
    dlg->handle(Action::Left, cx);
    CHECK(dlg->handle(Action::A, cx).kind == Transition::Pop);
    CHECK(log_has(cx, "systemctl suspend"));
    CHECK(!oscontract::quit_requested());

    // without pm-suspend ES hides SUSPEND SYSTEM
    fresh(cx, true, false);
    auto q2 = Registry::instance().make("menu.quit", cx, "");
    press(*q2, cx, Action::X);
    CHECK_EQ(labels(as_menu(q2)), T({"RESTART THE FRONTEND", "RESTART SYSTEM", "SHUTDOWN SYSTEM"}));
    oscontract::reset_for_test();
  }

  // ---- menu.ui: entries, stores, live effects
  {
    Ctx cx;
    fresh(cx);
    FakePad pad;
    cx.pad = &pad;
    auto u = Registry::instance().make("menu.ui", cx, "");
    Menu& m = as_menu(u);
    press(m, cx, Action::X);
    CHECK_EQ(m.title(), T("USER INTERFACE SETTINGS"));
    // no catalogs for ES languages beyond the fixtures (es, ru_RU, zh_CN) + English
    CHECK_EQ(index_of(m, "LANGUAGE (REGION)") >= 0, true);
    CHECK(index_of(m, "SWITCH CONFIRM & CANCEL BUTTONS") >= 0);
    CHECK(index_of(m, "SCREEN BRIGHTNESS") >= 0);
    CHECK(index_of(m, "SHOW CLOCK") >= 0);
    CHECK(index_of(m, "SHOW BATTERY STATUS") >= 0);
    CHECK(index_of(m, "START SCREENSAVER AFTER") >= 0);
    CHECK(index_of(m, "SCREENSAVER TYPE") >= 0);
    // things zfront does not have are not there
    for (const char* gone : {"THEME SET", "THEME CONFIGURATION", "GAMELIST VIEW STYLE", "LIST TRANSITION", "GAME LAUNCH TRANSITION", "ON-SCREEN HELP",
                             "DISABLE TOUCHSCREEN IN EMULATIONSTATION", "SHOW FAVORITES ON TOP", "GAME MEDIAS DURING FAST SCROLLING"})
      CHECK(index_of(m, gone) < 0);

    // InvertButtons: stored in es_settings.cfg and applied to the pad at once
    CHECK(goto_label(m, cx, "SWITCH CONFIRM & CANCEL BUTTONS"));
    CHECK(m.entry(size_t(m.cursor())).b);  // ES default: true
    press(m, cx, Action::A);
    CHECK_EQ(pad.calls, 1);
    CHECK(!pad.last);
    CHECK(!EsSettings::load(d + "/w/es_settings.cfg").get_bool("InvertButtons", true));
    press(m, cx, Action::A);
    CHECK(pad.last);

    // SHOW CLOCK: stored, and the status bar loses the clock now
    CHECK(!cx.status.time.empty());
    CHECK(goto_label(m, cx, "SHOW CLOCK"));
    press(m, cx, Action::A);
    CHECK(!EsSettings::load(d + "/w/es_settings.cfg").get_bool("DrawClock", true));
    CHECK(cx.status.time.empty());
    press(m, cx, Action::A);
    CHECK_EQ(cx.status.time, std::string("13:05"));
    // 12 hours (the SYSTEM menu owns the switch; the status follows the key)
    cx.es.set_bool("ClockMode12", true);
    ui_prefs::reapply(cx);
    CHECK_EQ(cx.status.time, std::string("01:05 PM"));
    cx.es.set_bool("ClockMode12", false);
    ui_prefs::reapply(cx);

    // SHOW BATTERY STATUS: text -> (cycle) no -> icon -> text; NO is the empty string and survives the file
    CHECK(goto_label(m, cx, "SHOW BATTERY STATUS"));
    const Entry& be = m.entry(size_t(m.cursor()));
    CHECK_EQ(be.value_text, T("ICON AND TEXT"));
    CHECK(ui_prefs::battery_text());
    press(m, cx, Action::Right);
    CHECK(!cx.status.battery.has_value());
    {
      EsSettings f = EsSettings::load(d + "/w/es_settings.cfg");
      CHECK(f.get("ShowBattery") != nullptr);
      CHECK(f.get("ShowBattery") && f.get("ShowBattery")->empty());
    }
    CHECK_EQ(m.entry(size_t(m.cursor())).value_text, T("NO"));
    press(m, cx, Action::Right);
    CHECK(cx.status.battery.has_value());
    CHECK(!ui_prefs::battery_text());
    CHECK_EQ(*EsSettings::load(d + "/w/es_settings.cfg").get("ShowBattery"), std::string("icon"));
    press(m, cx, Action::Right);
    CHECK(ui_prefs::battery_text());
    CHECK_EQ(*EsSettings::load(d + "/w/es_settings.cfg").get("ShowBattery"), std::string("text"));

    // SCREEN BRIGHTNESS: 128/255 -> 50 %; one step writes the panel (floor(51 * 255 / 100) = 130) and, after the pause, runs the script
    CHECK(goto_label(m, cx, "SCREEN BRIGHTNESS"));
    CHECK_EQ(m.entry(size_t(m.cursor())).value_text, std::string("50%"));
    cx.sv->cmd.log.clear();
    press(m, cx, Action::Right);
    CHECK(log_has(cx, "echo 130 > " + d + "/w/bl/panel/brightness"));
    CHECK(!log_has(cx, "brightness set"));
    CHECK_EQ(m.entry(size_t(m.cursor())).value_text, std::string("51%"));
    press(m, cx, Action::Right);
    wait_commit(m, cx);
    CHECK(log_has(cx, "brightness set 52"));
    CHECK(!log_has(cx, "brightness set 51"));  // one commit for the whole adjustment
    // lower limit 5 like the script
    for (int i = 0; i < 70; ++i) press(m, cx, Action::Left);
    CHECK_EQ(m.entry(size_t(m.cursor())).value_text, std::string("5%"));

    // screensaver: minutes in the UI, milliseconds in the file; 0 = off
    CHECK(goto_label(m, cx, "START SCREENSAVER AFTER"));
    CHECK_EQ(m.entry(size_t(m.cursor())).value_text, std::string("5 min"));
    press(m, cx, Action::Right);
    CHECK_EQ(cx.es.get_int("ScreenSaverTime", -1), 360000L);
    wait_commit(m, cx);
    CHECK_EQ(EsSettings::load(d + "/w/es_settings.cfg").get_int("ScreenSaverTime", -1), 360000L);
    for (int i = 0; i < 6; ++i) press(m, cx, Action::Left);
    CHECK_EQ(m.entry(size_t(m.cursor())).value_text, T("OFF"));
    wait_commit(m, cx);
    CHECK_EQ(EsSettings::load(d + "/w/es_settings.cfg").get_int("ScreenSaverTime", -1), 0L);
    CHECK_EQ(oscontract::idle_deadline(cx.es).has_value(), false);  // the timer the loop reads: never

    CHECK(goto_label(m, cx, "SCREENSAVER TYPE"));
    CHECK_EQ(m.entry(size_t(m.cursor())).value_text, std::string("black"));
    press(m, cx, Action::Right);  // dim, black, suspend: the last one only exists with pm-suspend (this fixture has it)
    CHECK_EQ(*EsSettings::load(d + "/w/es_settings.cfg").get("ScreenSaverBehavior"), std::string("suspend"));
    // "dim" is a real type now; the ES types zfront does not port (video, slideshow) show as black
    cx.es.set_string("ScreenSaverBehavior", "dim");
    auto u3 = Registry::instance().make("menu.ui", cx, "");
    CHECK(goto_label(as_menu(u3), cx, "SCREENSAVER TYPE"));
    CHECK_EQ(as_menu(u3).entry(size_t(as_menu(u3).cursor())).value_text, std::string("dim"));
    cx.es.set_string("ScreenSaverBehavior", "slideshow");
    auto u4 = Registry::instance().make("menu.ui", cx, "");
    CHECK(goto_label(as_menu(u4), cx, "SCREENSAVER TYPE"));
    CHECK_EQ(as_menu(u4).entry(size_t(as_menu(u4).cursor())).value_text, std::string("black"));
  }

  // ---- menu.ui without hardware: no backlight row, no battery row
  {
    Ctx cx;
    fresh(cx, false, false);
    Status nb = Status::demo("10:00");
    nb.battery.reset();
    ui_prefs::on_status(cx, nb);
    auto u = Registry::instance().make("menu.ui", cx, "");
    press(*u, cx, Action::X);
    Menu& m = as_menu(u);
    CHECK(index_of(m, "SCREEN BRIGHTNESS") < 0);
    CHECK(index_of(m, "SHOW BATTERY STATUS") < 0);
    CHECK(index_of(m, "SCREENSAVER TYPE") >= 0);  // dim / black exist everywhere; suspend needs pm-suspend
    CHECK(index_of(m, "START SCREENSAVER AFTER") >= 0);
    CHECK(index_of(m, "SHOW CLOCK") >= 0);
  }

  // ---- language: choices come from the catalogs, the change is stored, loaded and reaches the open menus
  {
    Ctx cx;
    fresh(cx);
    std::vector<Option> langs = available_language_options(ui_language_options());
    CHECK_EQ(langs.size(), size_t(5));  // en_US + es_ES and es_MX (both fall back to the "es" fixture) + ru_RU + zh_CN
    CHECK_EQ(langs[0].value, std::string("en_US"));
    bool has_es = false, has_ru = false, has_zh = false;
    for (const Option& o : langs) {
      has_es = has_es || (o.value == "es_ES" && o.label == "Español");
      has_ru = has_ru || o.value == "ru_RU";
      has_zh = has_zh || o.value == "zh_CN";
    }
    CHECK(has_es && has_ru && has_zh);
    CHECK_EQ(language_names().size(), size_t(36));

    auto main_menu = Registry::instance().make("menu.main", cx, "");
    press(*main_menu, cx, Action::X);
    Menu& mm = as_menu(main_menu);
    CHECK(index_of(mm, "QUIT") >= 0);

    auto u = Registry::instance().make("menu.ui", cx, "");
    Menu& m = as_menu(u);
    CHECK(goto_label(m, cx, "LANGUAGE (REGION)"));
    CHECK_EQ(m.entry(size_t(m.cursor())).value_text, std::string("English (US)"));
    Transition t = press(m, cx, Action::A);
    CHECK(t.kind == Transition::Push && t.screen);
    auto* dlg = static_cast<ui::ChoiceDialog*>(t.screen.get());
    int want = -1;
    for (size_t i = 0; i < langs.size(); ++i)
      if (langs[i].value == "es_ES") want = int(i);
    CHECK(want > 0);
    for (int i = 0; i < 6 && dlg->cursor() != want; ++i) dlg->handle(Action::Down, cx);
    CHECK_EQ(dlg->cursor(), want);
    CHECK(dlg->handle(Action::A, cx).kind == Transition::Pop);
    CHECK(slurp(d + "/w/system.cfg").find("system.language=es_ES") != std::string::npos);
    CHECK(slurp(d + "/w/system.cfg").find("audio.volume=40") != std::string::npos);  // the rest is kept
    CHECK(!i18n::language().empty());
    CHECK_EQ(T("QUIT"), std::string("Salir"));
    // the menus that were already open rebuild with the new strings at their next frame
    press(mm, cx, Action::X);  // (a frame would do the same, but this test has no font)
    CHECK(index_of_text(mm, "Salir") >= 0);
    CHECK(index_of_text(mm, i18n::display_text("QUIT")) < 0);  // the English text is gone
    press(m, cx, Action::X);
    CHECK_EQ(m.entry(size_t(m.cursor())).value_text, std::string("Español"));
    // and back to English
    i18n::set_language("", ui_language_options());
    press(mm, cx, Action::X);
    CHECK(index_of(mm, "QUIT") >= 0);

    // a failed save does not change the language
    sh("chmod 555 " + d + "/w");
    i18n::set_language("", ui_language_options());
    auto u2 = Registry::instance().make("menu.ui", cx, "");
    Menu& m2 = as_menu(u2);
    CHECK(goto_label(m2, cx, "LANGUAGE (REGION)"));
    Transition t2 = press(m2, cx, Action::A);
    auto* dlg2 = static_cast<ui::ChoiceDialog*>(t2.screen.get());
    dlg2->handle(Action::Down, cx);
    dlg2->handle(Action::A, cx);
    sh("chmod 755 " + d + "/w");
    if (::access((d + "/w").c_str(), W_OK) == 0 && ::geteuid() != 0) {
      CHECK(i18n::language().empty());
      CHECK(!cx.sv->toasts.items().empty());
    }
    i18n::set_language("", ui_language_options());
  }

  // ---- drawing (real theme): the three screens draw, and a frame at rest allocates nothing
  {
    const std::string dir = "/home/epsil/zenith-frontend/theme-dark";
    if (is_dir(dir)) {
      Ctx cx;
      fresh(cx);
      cx.status = Status();  // (the status bar's clock/battery text goes through Assets::text, outside what these menus add)
      std::string err;
      cx.w = 640, cx.h = 480;
      CHECK(Theme::load(dir, ratio_name(640, 480), cx.th, err));
      CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
      for (const char* name : {"menu.main", "menu.quit", "menu.ui"}) {
        auto s = Registry::instance().make(name, cx, "");
        std::vector<Op> a;
        s->draw(cx, a);
        CHECK(a.size() > 20);
        for (int i = 0; i < 4; ++i) s->tick(1.0f, cx);
        a.clear();
        s->draw(cx, a);
        size_t before = g_allocs;
        for (int i = 0; i < 50; ++i) {
          a.clear();
          s->draw(cx, a);
        }
        CHECK_EQ(g_allocs - before, size_t(0));
        CHECK(!s->tick(0.016f, cx));
        CHECK(!s->wake_in().has_value());
      }
    }
  }

  if (const char* out = std::getenv("ZF_MENU_PNG")) render_all(out);
  sh("rm -rf " + d);
  return zt::done();
}

// ---- review PNGs ------------------------------------------------------------------------------------------------------

namespace {

void shot(Ctx& cx, std::initializer_list<Screen*> stack, const std::string& path, bool toasts = false) {
  std::vector<Op> ops;
  for (Screen* s : stack) {
    s->tick(1.0f, cx);
    s->draw(cx, ops);
  }
  if (toasts) draw_toasts(cx, ops);
  Canvas cv = cpu_render(ops, cx.assets, uint32_t(cx.w), uint32_t(cx.h));
  std::string err;
  if (!save_png(cv, path, err)) std::fprintf(stderr, "png: %s\n", err.c_str());
}

void render_all(const std::string& out) {
  sh("mkdir -p " + out);
  const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
  // every Ctx stays alive: the toast/OSD caches are keyed on the address of the Assets they were built for
  std::vector<std::unique_ptr<Ctx>> keep;
  for (const char* theme : {"dark", "light"}) {
    const std::string dir = std::string("/home/epsil/zenith-frontend/theme-") + theme;
    if (!is_dir(dir)) continue;
    for (const auto& wh : sizes) {
      keep.push_back(std::make_unique<Ctx>());
      Ctx& cx = *keep.back();
      fresh(cx);
      std::string err;
      cx.w = float(wh[0]), cx.h = float(wh[1]);
      CHECK(Theme::load(dir, ratio_name(wh[0], wh[1]), cx.th, err));
      CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
      cx.status = Status::demo("12:04");
      ui_prefs::on_status(cx, cx.status);
      char tag[64];
      std::snprintf(tag, sizeof tag, "%s-%ux%u", theme, wh[0], wh[1]);
      auto name = [&](const char* n) { return out + "/" + n + "-" + tag + ".png"; };

      // main menu: every screen registered (stubs), then the natural registry of this branch
      for (const char* s : kAllScreens)
        if (!Registry::instance().has(s)) Registry::instance().add(s, [](Ctx&, const std::string&) { return std::make_unique<Stub>(); });
      auto full = make_main_menu({}, false);
      shot(cx, {full.get()}, name("01-main-all"));
      for (int i = 0; i < 9; ++i) full->handle(Action::Down, cx);
      shot(cx, {full.get()}, name("02-main-all-last"));
      auto dem = make_main_menu({}, true);
      for (int i = 0; i < 10; ++i) dem->handle(Action::Down, cx);
      shot(cx, {dem.get()}, name("03-main-demo-entry"));
      auto few = make_main_menu({{tr("USER INTERFACE SETTINGS"), "menu.ui", "ui"}, {tr("QUIT"), "menu.quit", "quit"}}, false);
      shot(cx, {few.get()}, name("04-main-two-entries"));

      // quit + its question
      auto q = Registry::instance().make("menu.quit", cx, "");
      shot(cx, {q.get()}, name("05-quit"));
      Transition t = q->handle(Action::A, cx);
      shot(cx, {q.get(), t.screen.get()}, name("06-quit-confirm"));
      for (int i = 0; i < 3; ++i) q->handle(Action::Down, cx);
      Transition t2 = q->handle(Action::A, cx);
      shot(cx, {q.get(), t2.screen.get()}, name("07-quit-confirm-shutdown"));

      // user interface settings
      auto u = Registry::instance().make("menu.ui", cx, "");
      shot(cx, {u.get()}, name("08-ui-top"));
      Transition lt = u->handle(Action::A, cx);
      shot(cx, {u.get(), lt.screen.get()}, name("09-ui-language-list"));
      for (int i = 0; i < 4; ++i) u->handle(Action::Down, cx);
      shot(cx, {u.get()}, name("10-ui-middle"));
      for (int i = 0; i < 12; ++i) u->handle(Action::Down, cx);
      shot(cx, {u.get()}, name("11-ui-bottom"));
      cx.sv->toasts.push("Saved", 3000);
      osd_show_level(cx, "brightness", 0.51f, 3000);
      shot(cx, {u.get()}, name("12-ui-brightness-osd"), true);
    }
  }
}

}  // namespace
