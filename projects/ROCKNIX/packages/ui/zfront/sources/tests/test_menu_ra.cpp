// menu.retroachievements, driven like the person would: what each row writes to system.cfg (the keys the OS reads), the sound list from a
// scratch tree, and the login-on-close of ES's save (token kept / cleared, RA turned off with the reason) on a fake `curl`.
// The password and the token must be nowhere on screen, in a toast or in a logged command.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "ra.hpp"
#include "registry.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
void put(const std::string& path, const std::string& text) {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream f(path, std::ios::binary);
  f << text;
}
size_t count_of(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}
bool has_line(const std::string& text, const std::string& line) {
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    if (text.substr(i, e == std::string::npos ? std::string::npos : e - i) == line) return true;
    i = e == std::string::npos ? text.size() : e + 1;
  }
  return false;
}

Menu* as_menu(const std::unique_ptr<Screen>& s) { return static_cast<Menu*>(s.get()); }
int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}
void pump(Ctx& cx) {
  for (int i = 0; i < 600; ++i) {
    cx.sv->jobs.drain();
    if (cx.sv->jobs.idle()) break;
    ::usleep(5000);
  }
  cx.sv->jobs.drain();
}
std::string toasts(Ctx& cx) {
  std::string t;
  for (const Toast& x : cx.sv->toasts.items()) t += x.text + "\n";
  return t;
}
/// Every text the menu shows: labels, values, hints.
std::string screen_text(const Menu& m) {
  std::string t;
  for (size_t i = 0; i < m.size(); ++i) {
    const Entry& e = m.entry(i);
    t += e.label + "|" + (e.password ? std::string("(dots)") : e.value_text) + "|" + e.hint_text + "\n";  // a password is drawn as dots
  }
  return t;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-menu-ra-XXXXXX";
  const char* d = ::mkdtemp(tmpl);
  CHECK(d != nullptr);
  if (!d) return 1;
  const std::string dir = d, fake = dir + "/fake", scratch = dir + "/tmp";
  fs::create_directories(scratch);

  put(fake + "/curl", R"SH(#!/bin/bash
D="$ZF_FAKE_DIR"
echo "$*" >> "$D/argv.log"
[ "$1" = "-K" ] || exit 2
echo run >> "$D/runs.log"
if [ -f "$D/down" ]; then printf '\n@@6|000\n'; exit 6; fi
data=$(grep '^data' "$2")
case "$data" in
  *"p=GOODPASS"*) printf '{"Success":true,"User":"ana","Token":"TOKEN-OF-ANA"}\n@@0|200\n' ;;
  *) printf '{"Success":false,"Error":"Invalid User/Password combination. Please try again"}\n@@0|401\n' ;;
esac
)SH");
  ::chmod((fake + "/curl").c_str(), 0755);
  ::setenv("ZF_CURL", (fake + "/curl").c_str(), 1);
  ::setenv("ZF_FAKE_DIR", fake.c_str(), 1);
  ::setenv("ZF_RA_TMP", scratch.c_str(), 1);
  ::unsetenv("DEVICE_ANALOG_STICKS_LED_CONTROL");
  mg::env().root = dir + "/root";
  put(dir + "/system.cfg", "audio.volume=50\nglobal.retroachievements=0\nglobal.retroachievements.leaderboards=disabled\n");
  SystemConf::lock_path() = dir + "/lock";

  Ctx cx;
  {
    std::string err;
    CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
    cx.w = 640;
    cx.h = 480;
  }
  cx.sv->cmd.dry_run = false;
  cx.sv->cmd.keep_log = true;
  cx.conf = SystemConf::load(dir + "/system.cfg");
  cx.es = EsSettings::from_text("<config/>", dir + "/es_settings.cfg");
  auto cfg = [&] { return slurp(dir + "/system.cfg"); };
  auto runs = [&] { return count_of(slurp(fake + "/runs.log"), "run"); };
  auto open = [&]() {
    std::unique_ptr<Screen> s = Registry::instance().make("menu.retroachievements", cx, "");
    CHECK(s != nullptr);
    if (s) as_menu(s)->rebuild(cx);
    return s;
  };

  // ---- registered, and GAMES offers it among its links at the end (diseno-menus/PROPUESTA-REORDEN-10nov.md)
  CHECK(Registry::instance().has("menu.retroachievements"));
  {
    std::unique_ptr<Screen> g = Registry::instance().make("menu.game", cx, "");
    Menu* gm = as_menu(g);
    gm->rebuild(cx);
    int more = find(*gm, "MORE"), row = find(*gm, "RETROACHIEVEMENTS SETTINGS");
    CHECK(more >= 0 && row == more + 1);
    CHECK(row >= 0 && gm->entry(size_t(row)).show_arrow);
  }

  // ---- rows: ES's list, ES's order, the keys the OS reads
  {
    auto s = open();
    Menu* m = as_menu(s);
    CHECK_EQ(m->title(), std::string("RETROACHIEVEMENTS SETTINGS"));
    const char* order[] = {"SETTINGS", "RETROACHIEVEMENTS", "USERNAME", "PASSWORD", "TEST ACCOUNT", "OPTIONS", "HARDCORE MODE", "LEADERBOARDS",
                           "VERBOSE MODE", "RICH PRESENCE", "AUTOMATIC SCREENSHOT", "CHALLENGE INDICATORS", "UNOFFICIAL ACHIEVEMENTS"};
    int last = -1;
    for (const char* l : order) {
      int i = find(*m, l);
      CHECK(i > last);
      last = i;
    }
    CHECK_EQ(find(*m, "ENCORE MODE"), -1);  // nothing reads retroachievements.encore
    CHECK_EQ(find(*m, "UNLOCK SOUND"), -1);  // no sounds installed
    CHECK_EQ(find(*m, "LED NOTIFICATIONS"), -1);  // no stick LEDs
    CHECK(find(*m, "SHOW RETROACHIEVEMENTS ENTRY IN MAIN MENU") >= 0);  // the main menu row (menu_main.cpp) is switched here
    CHECK(m->entry(size_t(find(*m, "PASSWORD"))).password);
    CHECK(!m->entry(size_t(find(*m, "USERNAME"))).password);

    struct Row {
      const char* label;
      const char* key;
    } rows[] = {{"HARDCORE MODE", "global.retroachievements.hardcore"},
                {"LEADERBOARDS", "global.retroachievements.leaderboards"},
                {"VERBOSE MODE", "global.retroachievements.verbose"},
                {"RICH PRESENCE", "global.retroachievements.richpresence"},
                {"AUTOMATIC SCREENSHOT", "global.retroachievements.screenshot"},
                {"CHALLENGE INDICATORS", "global.retroachievements.challengeindicators"},
                {"UNOFFICIAL ACHIEVEMENTS", "global.retroachievements.testunofficial"}};
    for (const Row& r : rows) {
      Entry& e = const_cast<Entry&>(m->entry(size_t(find(*m, r.label))));
      CHECK(!e.get_b());  // defaults off ("disabled" in the shipped system.cfg counts as off)
      e.set_b(true);
      CHECK(has_line(cfg(), std::string(r.key) + "=1"));
      CHECK(e.get_b());
      e.set_b(false);
      CHECK(has_line(cfg(), std::string(r.key) + "=0"));  // ES writes an explicit 0, the shell scripts compare with 1
    }
    // the keys ES writes but nothing reads were not used
    CHECK(cfg().find("challenge_indicators") == std::string::npos && cfg().find("retroachievements.unofficial") == std::string::npos);
    CHECK(cfg().find("encore") == std::string::npos);
    CHECK(has_line(cfg(), "audio.volume=50"));  // other keys survive

    Entry& user = const_cast<Entry&>(m->entry(size_t(find(*m, "USERNAME"))));
    user.set_s("ana");
    CHECK(has_line(cfg(), "global.retroachievements.username=ana"));
    CHECK_EQ(user.get_s(), std::string("ana"));
    // untouched by the open/close: RA is off, so there is nothing to log in
    m->handle(Action::B, cx);
    pump(cx);
    CHECK_EQ(runs(), size_t(0));
  }

  // ---- unlock sound + LED row only where the device has them
  {
    put(dir + "/root/usr/share/libretro/sounds/zelda-secret.ogg", "x");
    put(dir + "/root/usr/share/libretro/sounds/mario-1up.ogg", "x");
    put(dir + "/root/usr/share/libretro/sounds/notes.txt", "x");
    put(dir + "/root/storage/roms/music/retroachievements/mario-1up.ogg", "x");  // same name in both folders: once
    put(dir + "/root/storage/roms/music/retroachievements/mine.ogg", "x");
    CHECK_EQ(ra::sound_names().size(), size_t(3));
    ::setenv("DEVICE_ANALOG_STICKS_LED_CONTROL", "true", 1);
    auto s = open();
    Menu* m = as_menu(s);
    int snd = find(*m, "UNLOCK SOUND");
    CHECK(snd >= 0);
    Entry& e = const_cast<Entry&>(m->entry(size_t(snd)));
    CHECK_EQ(e.options.size(), size_t(4));
    CHECK_EQ(e.options[0].value, std::string("none"));
    CHECK_EQ(e.options[0].label, std::string(tr("NONE")));  // "none" in lower case between MARIO-1UP and ZELDA looked like a typo
    CHECK_EQ(e.options[1].value, std::string("mario-1up"));  // sorted
    CHECK_EQ(e.options[1].label, std::string("MARIO-1UP"));
    CHECK_EQ(e.get_s(), std::string("none"));
    e.set_s("zelda-secret");
    CHECK(has_line(cfg(), "global.retroachievements.sound=zelda-secret"));
    CHECK_EQ(e.get_s(), std::string("zelda-secret"));
    e.set_s("none");
    CHECK(has_line(cfg(), "global.retroachievements.sound=none"));  // setsettings.sh treats anything but "none" as "play it"
    cx.conf.persist("global.retroachievements.sound", "gone-file");
    CHECK_EQ(e.get_s(), std::string("none"));  // a sound that was deleted
    int led = find(*m, "LED NOTIFICATIONS");
    CHECK(led >= 0);
    const_cast<Entry&>(m->entry(size_t(led))).set_b(true);
    CHECK(has_line(cfg(), "global.retroachievements.leds=1"));
    ::unsetenv("DEVICE_ANALOG_STICKS_LED_CONTROL");
  }

  // ---- login on close: bad password -> token cleared, RA off, reason shown
  {
    cx.conf.persist("global.retroachievements.token", "STALE");
    auto s = open();
    Menu* m = as_menu(s);
    const_cast<Entry&>(m->entry(size_t(find(*m, "RETROACHIEVEMENTS")))).set_b(true);
    const_cast<Entry&>(m->entry(size_t(find(*m, "USERNAME")))).set_s("ana");
    const_cast<Entry&>(m->entry(size_t(find(*m, "PASSWORD")))).set_s("WRONGPASS");
    m->handle(Action::B, cx);
    pump(cx);
    CHECK_EQ(runs(), size_t(1));
    CHECK(has_line(cfg(), "global.retroachievements=0"));
    CHECK(cfg().find("global.retroachievements.token") == std::string::npos);
    std::string t = toasts(cx);
    CHECK(t.find("UNABLE TO ACTIVATE RETROACHIEVEMENTS:") != std::string::npos);
    CHECK(t.find("Invalid User/Password combination. Please try again") != std::string::npos);
    CHECK(t.find("WRONGPASS") == std::string::npos);
  }

  // ---- good password -> token stored; a second visit with nothing changed does not ask again
  {
    cx.sv->toasts = Toasts();
    {
      auto s = open();
      Menu* m = as_menu(s);
      const_cast<Entry&>(m->entry(size_t(find(*m, "RETROACHIEVEMENTS")))).set_b(true);
      const_cast<Entry&>(m->entry(size_t(find(*m, "PASSWORD")))).set_s("GOODPASS");
      m->handle(Action::B, cx);
      pump(cx);
    }
    CHECK_EQ(runs(), size_t(2));
    CHECK(has_line(cfg(), "global.retroachievements=1"));
    CHECK(has_line(cfg(), "global.retroachievements.token=TOKEN-OF-ANA"));
    CHECK(toasts(cx).empty());  // ES is silent when it works
    {
      auto s = open();
      as_menu(s)->handle(Action::B, cx);
      pump(cx);
    }
    CHECK_EQ(runs(), size_t(2));
    CHECK(has_line(cfg(), "global.retroachievements.token=TOKEN-OF-ANA"));
    // enabled, token missing -> asks again (ES: `token.empty()`)
    cx.conf.persist_remove("global.retroachievements.token");
    {
      auto s = open();
      as_menu(s)->handle(Action::B, cx);
      pump(cx);
    }
    CHECK_EQ(runs(), size_t(3));
    CHECK(has_line(cfg(), "global.retroachievements.token=TOKEN-OF-ANA"));
    // credentials changed -> asks again
    {
      auto s = open();
      Menu* m = as_menu(s);
      const_cast<Entry&>(m->entry(size_t(find(*m, "USERNAME")))).set_s("ana2");
      m->handle(Action::B, cx);
      pump(cx);
    }
    CHECK_EQ(runs(), size_t(4));
  }

  // ---- network down: not a wrong password, but RA cannot be confirmed: off, with the reason
  {
    cx.sv->toasts = Toasts();
    cx.conf.persist("global.retroachievements.username", "ana");
    put(fake + "/down", "");
    cx.conf.persist_remove("global.retroachievements.token");
    {
      auto s = open();
      as_menu(s)->handle(Action::B, cx);
      pump(cx);
    }
    CHECK(has_line(cfg(), "global.retroachievements=0"));
    CHECK(toasts(cx).find("NO NETWORK CONNECTION") != std::string::npos);
    fs::remove(fake + "/down");
  }

  // ---- switched off -> the token goes (ES), nothing is asked
  {
    cx.conf.persist("global.retroachievements", "1");
    cx.conf.persist("global.retroachievements.token", "TOKEN-OF-ANA");
    const size_t before = runs();
    auto s = open();
    Menu* m = as_menu(s);
    const_cast<Entry&>(m->entry(size_t(find(*m, "RETROACHIEVEMENTS")))).set_b(false);
    m->handle(Action::B, cx);
    pump(cx);
    CHECK_EQ(runs(), before);
    CHECK(cfg().find("global.retroachievements.token") == std::string::npos);
  }

  // ---- TEST ACCOUNT: asks now, shows the result, keeps the token when RA is on and does not ask again on leaving
  {
    cx.sv->toasts = Toasts();
    cx.conf.persist("global.retroachievements", "1");
    cx.conf.persist("global.retroachievements.username", "ana");
    cx.conf.persist("global.retroachievements.password", "GOODPASS");
    cx.conf.persist_remove("global.retroachievements.token");
    const size_t before = runs();
    auto s = open();
    Menu* m = as_menu(s);
    // the snapshot of the open already has these credentials but no token: TEST ACCOUNT stores one
    int test = find(*m, "TEST ACCOUNT");
    CHECK(test >= 0);
    m->entry(size_t(test)).on_a(cx);
    pump(cx);
    m->rebuild(cx);
    CHECK_EQ(runs(), before + 1);
    CHECK(has_line(cfg(), "global.retroachievements.token=TOKEN-OF-ANA"));
    int ok = find(*m, "LOGGED IN");
    CHECK(ok >= 0 && m->entry(size_t(ok)).value_text == "ana");
    m->handle(Action::B, cx);
    pump(cx);
    CHECK_EQ(runs(), before + 1);  // nothing asked on close

    // a refusal is shown as a (danger) row; nothing else changes
    auto s2 = open();
    Menu* m2 = as_menu(s2);
    cx.conf.persist("global.retroachievements.password", "NOPE");
    m2->entry(size_t(find(*m2, "TEST ACCOUNT"))).on_a(cx);
    pump(cx);
    m2->rebuild(cx);
    CHECK(screen_text(*m2).find("Invalid User/Password combination. Please try again") != std::string::npos);
    CHECK(find(*m2, "LOGGED IN") < 0);
  }

  // ---- secrets: not in any command, argument, toast or row
  {
    for (const std::string& line : cx.sv->cmd.log) {
      CHECK(line.find("GOODPASS") == std::string::npos && line.find("WRONGPASS") == std::string::npos && line.find("NOPE") == std::string::npos);
      CHECK(line.find("TOKEN-OF-ANA") == std::string::npos);
    }
    std::string argv = slurp(fake + "/argv.log");
    CHECK(argv.find("GOODPASS") == std::string::npos && argv.find("WRONGPASS") == std::string::npos);
    CHECK(fs::is_empty(scratch));
    auto s = open();
    std::string t = screen_text(*as_menu(s)) + toasts(cx);
    CHECK(t.find("GOODPASS") == std::string::npos && t.find("NOPE") == std::string::npos && t.find("TOKEN-OF-ANA") == std::string::npos);
  }

  for (const char* k : {"ZF_CURL", "ZF_FAKE_DIR", "ZF_RA_TMP"}) ::unsetenv(k);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return zt::done();
}
