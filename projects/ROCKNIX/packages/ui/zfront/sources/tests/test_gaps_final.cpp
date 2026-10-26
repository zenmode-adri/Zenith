// gaps-final: launch from the savestate manager closes it, events game-selected / system-selected, BIOS check before launch,
// theme / log-path resolution, saved Wi-Fi networks.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "app.hpp"
#include "bootargs.hpp"
#include "check.hpp"
#include "launchcheck.hpp"
#include "menu_game.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "savestates.hpp"
#include "screens.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string g_tmp;

System make_system(const std::string& name, const std::string& emulator, std::vector<std::string> games) {
  System s;
  s.name = name;
  s.fullname = name;
  s.theme = name;
  s.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR%";
  s.emulator = emulator;
  s.core = "gambatte";
  s.emus = {{emulator, {"gambatte"}}};
  s.rom_dir = g_tmp + "/roms/" + name;
  s.gamelist = s.rom_dir + "/gamelist.xml";
  for (auto& n : games) {
    Game g;
    g.name = n;
    g.path = s.rom_dir + "/" + n + ".gb";
    s.games.push_back(std::move(g));
  }
  s.count = s.games.size();
  s.loaded = true;
  return s;
}

void make_ctx(Ctx& cx, const std::string& conf_text) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = true;
  cx.es = EsSettings::from_text("<config/>", g_tmp + "/es_settings.cfg");
  cx.conf = SystemConf::from_text(conf_text, g_tmp + "/system.cfg");
  cx.systems.push_back(make_system("gb", "retroarch", {"Tetris", "Zelda", "Metroid"}));
  cx.systems.push_back(make_system("pc", "dosbox", {"Doom"}));
}

// ---- 1. launching from the savestate manager closes it -------------------------------------------------------------------------
void test_manager_closes_on_launch() {
  Ctx cx;
  make_ctx(cx, "global.savestates=1\n");
  auto list = std::make_unique<ListScreen>(0, cx);
  App app(std::move(cx), std::move(list), make_status_source("/nonexistent"));
  app.handle(Action::Down);  // Zelda
  Outcome o = app.handle(Action::A);  // "savestates=1": the manager opens first
  CHECK(o.kind == Outcome::None);
  o = app.handle(Action::A);  // first card: START NEW GAME
  CHECK(o.kind == Outcome::Launch);
  CHECK_EQ(o.sys, 0);
  CHECK_EQ(o.game, 1);
  CHECK(o.cmd.find("Zelda.gb") != std::string::npos);
  savestates::after_game();
  // the manager is gone: A on the list (same cursor) opens it again instead of launching from the old one
  o = app.handle(Action::A);
  CHECK(o.kind == Outcome::None);
  o = app.handle(Action::A);
  CHECK(o.kind == Outcome::Launch);
  CHECK_EQ(o.game, 1);
  savestates::after_game();
}

// ---- 2. game-selected / system-selected ----------------------------------------------------------------------------------------
size_t count_events(const char* what) {
  size_t n = 0;
  for (const std::string& l : oscontract::dry_log())
    if (l.rfind(what, 0) == 0) ++n;
  return n;
}

void test_unbound_costs_nothing() {
  // no oscontract::startup (harness / preview): the events are never armed, so the cursor adds no timer and runs nothing
  Ctx cx;
  make_ctx(cx, "");
  auto list = std::make_unique<ListScreen>(0, cx);
  App app(std::move(cx), std::move(list), make_status_source("/nonexistent"));
  app.handle(Action::Down);
  app.handle(Action::Down);
  CHECK(!oscontract::selected_listening("game-selected") && !oscontract::selected_listening("system-selected"));
  app.tick(1.0f);
  CHECK(oscontract::dry_log().empty());
}

void test_selected_events() {
  oscontract::startup(std::make_shared<Services>(), true);  // dry: the calls land in dry_log()
  oscontract::dry_log().clear();
  {
    Ctx cx;
    make_ctx(cx, "");
    for (const System& s : cx.systems) cx.items.push_back(SystemItem{s.theme, s.fullname, uint32_t(s.count)});
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    app.tick(0.0f);
    CHECK_EQ(count_events("event system-selected gb"), 1ul);
    app.tick(0.0f);
    CHECK_EQ(count_events("event system-selected"), 1ul);  // the same system is not announced again
    app.handle(Action::Right);
    app.handle(Action::Left);
    app.handle(Action::Right);  // a held key: three steps, one event at the end
    app.tick(0.05f);
    CHECK_EQ(count_events("event system-selected"), 1ul);  // still moving
    for (int i = 0; i < 12; ++i) app.tick(0.1f);
    CHECK_EQ(count_events("event system-selected pc"), 1ul);
    CHECK_EQ(count_events("event system-selected"), 2ul);
    // into the list and back: announced again
    app.handle(Action::A);
    app.handle(Action::B);
    app.tick(0.0f);
    CHECK_EQ(count_events("event system-selected pc"), 2ul);
  }
  oscontract::dry_log().clear();
  {
    Ctx cx;
    make_ctx(cx, "");
    auto list = std::make_unique<ListScreen>(0, cx);
    App app(std::move(cx), std::move(list), make_status_source("/nonexistent"));
    CHECK(app.screen_deadline().has_value());  // the entry selection is pending: the loop wakes for it
    app.tick(0.05f);
    CHECK_EQ(count_events("event game-selected"), 0ul);
    app.tick(0.2f);
    CHECK_EQ(count_events("event game-selected gb"), 1ul);
    CHECK(oscontract::dry_log().back().find("Tetris.gb Tetris") != std::string::npos);
    app.handle(Action::Down);
    app.tick(0.05f);
    app.handle(Action::Down);  // key repeat: the timer restarts
    app.tick(0.09f);
    CHECK_EQ(count_events("event game-selected"), 1ul);
    app.tick(0.09f);
    CHECK_EQ(count_events("event game-selected"), 2ul);
    CHECK(oscontract::dry_log().back().find("Metroid.gb Metroid") != std::string::npos);
    app.tick(1.0f);
    CHECK_EQ(count_events("event game-selected"), 2ul);  // nothing without a move
  }
}

// ---- 3. CheckBiosesAtLaunch --------------------------------------------------------------------------------------------------
void test_bios_check() {
  CHECK(launchcheck::bios_missing("> psx\nMISSING 8dd7d5296a650fac7319bce665a6a53c scph5501.bin\n", "psx"));
  CHECK(!launchcheck::bios_missing("> psx\nMISSING 8dd7 scph5501.bin\n", "gb"));
  CHECK(!launchcheck::bios_missing("> gb\nUNTESTED aa b.bin\n> psx\nMISSING 8dd7 scph5501.bin\n", "gb"));  // only the system asked for
  CHECK(!launchcheck::bios_missing("No missing bios\n", "gb"));
  CHECK(!launchcheck::bios_missing("", "gb"));

  mg::env().root = g_tmp;
  mg::env().bin_dir = "/usr/bin";
  std::string cmd = "mkdir -p '" + g_tmp + "/usr/bin' '" + g_tmp + "/tmp' && : > '" + g_tmp + "/usr/bin/rocknix-systems'";
  CHECK(std::system(cmd.c_str()) == 0);
  write_atomic(g_tmp + "/tmp/rocknix-systems.out", "> gb\nMISSING 0123 gb_bios.bin\n> pc\nMISSING 4567 dos.bin\n");

  // setting off (the default): straight to the game
  {
    Ctx cx;
    make_ctx(cx, "");
    auto list = std::make_unique<ListScreen>(0, cx);
    App app(std::move(cx), std::move(list), make_status_source("/nonexistent"));
    CHECK(app.handle(Action::A).kind == Outcome::Launch);
  }
  // setting on, BIOS missing: a question first; NO (the default focus) cancels, YES launches and the dialog is gone afterwards
  {
    Ctx cx;
    make_ctx(cx, "");
    cx.es.set_bool("CheckBiosesAtLaunch", true);
    auto list = std::make_unique<ListScreen>(0, cx);
    App app(std::move(cx), std::move(list), make_status_source("/nonexistent"));
    app.handle(Action::Down);
    CHECK(app.handle(Action::A).kind == Outcome::None);  // dialog up
    CHECK(app.handle(Action::A).kind == Outcome::None);  // A on NO
    CHECK(app.handle(Action::A).kind == Outcome::None);  // dialog again
    app.handle(Action::Left);                              // focus YES
    Outcome o = app.handle(Action::A);
    CHECK(o.kind == Outcome::Launch);
    CHECK_EQ(o.sys, 0);
    CHECK_EQ(o.game, 1);
    CHECK(o.cmd.find("Zelda.gb") != std::string::npos);
    CHECK(app.handle(Action::A).kind == Outcome::None);  // the list is on top again: the question comes back
  }
  // a system whose BIOS is fine, or a script that says nothing
  {
    Ctx cx;
    make_ctx(cx, "");
    cx.es.set_bool("CheckBiosesAtLaunch", true);
    write_atomic(g_tmp + "/tmp/rocknix-systems.out", "> pc\nMISSING 4567 dos.bin\n");
    auto list = std::make_unique<ListScreen>(0, cx);
    App app(std::move(cx), std::move(list), make_status_source("/nonexistent"));
    CHECK(app.handle(Action::A).kind == Outcome::Launch);
  }
  // without the script on the console: no check at all
  {
    Ctx cx;
    make_ctx(cx, "");
    cx.es.set_bool("CheckBiosesAtLaunch", true);
    write_atomic(g_tmp + "/tmp/rocknix-systems.out", "> gb\nMISSING 0123 x.bin\n");
    std::string rm = "rm -f '" + g_tmp + "/usr/bin/rocknix-systems'";
    CHECK(std::system(rm.c_str()) == 0);
    auto list = std::make_unique<ListScreen>(0, cx);
    App app(std::move(cx), std::move(list), make_status_source("/nonexistent"));
    CHECK(app.handle(Action::A).kind == Outcome::Launch);
  }
}

void mkdirs(const std::string& d);
std::string slurp(const std::string& p) {
  std::string s;
  read_file(p, s);
  return s;
}

// ---- 3b. the manager must not prepare state files before the BIOS question is answered YES -------------------------------------
void test_manager_bios_order() {
  mg::env().root = g_tmp;
  mg::env().bin_dir = "/usr/bin";
  std::string mk = "mkdir -p '" + g_tmp + "/usr/bin' '" + g_tmp + "/tmp' && : > '" + g_tmp + "/usr/bin/rocknix-systems'";
  CHECK(std::system(mk.c_str()) == 0);
  write_atomic(g_tmp + "/tmp/rocknix-systems.out", "> gb\nMISSING 0123 gb_bios.bin\n");
  const std::string dir = g_tmp + "/roms/savestates/gb";
  mkdirs(dir);
  write_atomic(dir + "/Zelda.state", "SLOT0");
  write_atomic(dir + "/Zelda.state.auto", "AUTO");

  Ctx cx;
  make_ctx(cx, "global.savestates=1\n");
  cx.es.set_bool("CheckBiosesAtLaunch", true);
  auto list = std::make_unique<ListScreen>(0, cx);
  App app(std::move(cx), std::move(list), make_status_source("/nonexistent"));
  app.handle(Action::Down);  // Zelda
  CHECK(app.handle(Action::A).kind == Outcome::None);  // the manager opens first (the BIOS question comes with the card)
  // the manager is up. Pick the slot-0 state card (the cards are START NEW GAME, then the states)
  auto pick_state = [&]() {
    app.handle(Action::Right);
    app.handle(Action::Right);
    return app.handle(Action::A);
  };
  Outcome o = pick_state();
  CHECK(o.kind == Outcome::None);                       // BIOS question, NO focused
  CHECK(!savestates::plan_active());                    // nothing prepared yet
  CHECK_EQ(slurp(dir + "/Zelda.state.auto"), std::string("AUTO"));
  CHECK(!file_exists(dir + "/Zelda.state.auto.bak"));
  o = app.handle(Action::A);                            // A on NO: back to the manager, nothing happened
  CHECK(o.kind == Outcome::None);
  CHECK(!savestates::plan_active());
  CHECK_EQ(slurp(dir + "/Zelda.state.auto"), std::string("AUTO"));
  CHECK(!file_exists(dir + "/Zelda.state.auto.bak"));
  // YES: the files are prepared and the game starts
  o = pick_state();
  CHECK(o.kind == Outcome::None);
  app.handle(Action::Left);
  o = app.handle(Action::A);
  CHECK(o.kind == Outcome::Launch);
  CHECK(o.cmd.find("-state_slot") != std::string::npos);
  CHECK(savestates::plan_active());
  CHECK_EQ(slurp(dir + "/Zelda.state.auto"), std::string("SLOT0"));  // prepared only now
  savestates::after_game();
  CHECK(!savestates::plan_active());
  CHECK_EQ(slurp(dir + "/Zelda.state.auto"), std::string("AUTO"));
}

// ---- 4. command line: theme from ThemeSet, --log-path ----------------------------------------------------------------------------
void mkdirs(const std::string& d) {
  std::string c = "mkdir -p '" + d + "'";
  CHECK(std::system(c.c_str()) == 0);
}
void touch_layout(const std::string& d) {
  mkdirs(d);
  write_atomic(d + "/layout.xml", "<theme/>");
}

void test_boot_args() {
  const std::string r = g_tmp + "/bootroot";
  CHECK(boot::resolve_theme("", r).empty());  // nothing installed
  touch_layout(r + "/usr/share/themes/es-theme-zenith-dark");
  touch_layout(r + "/usr/share/themes/es-theme-zenith");
  CHECK_EQ(boot::resolve_theme("", r), r + "/usr/share/themes/es-theme-zenith");  // no setting: the image default, light
  CHECK_EQ(boot::resolve_theme("es-theme-zenith", r), r + "/usr/share/themes/es-theme-zenith");
  CHECK_EQ(boot::resolve_theme("es-theme-zenith-dark", r), r + "/usr/share/themes/es-theme-zenith-dark");
  CHECK_EQ(boot::resolve_theme("art-book-next", r), r + "/usr/share/themes/es-theme-zenith");  // zfront only has Zenith
  CHECK_EQ(boot::resolve_theme("art-book-next-dark", r), r + "/usr/share/themes/es-theme-zenith-dark");
  touch_layout(r + "/usr/share/zfront/theme-dark");
  touch_layout(r + "/usr/share/zfront/theme-light");
  CHECK_EQ(boot::resolve_theme("es-theme-zenith", r), r + "/usr/share/themes/es-theme-zenith");  // the user/OS folder of that set wins
  CHECK_EQ(boot::resolve_theme("Zenith-Light", r), r + "/usr/share/zfront/theme-light");         // unknown Zenith variant: by name
  CHECK_EQ(boot::resolve_theme("", r), r + "/usr/share/zfront/theme-light");
  CHECK_EQ(boot::resolve_theme("es-theme-zenith-dark", r), r + "/usr/share/themes/es-theme-zenith-dark");
  touch_layout(r + "/storage/.config/emulationstation/themes/es-theme-zenith");
  CHECK_EQ(boot::resolve_theme("es-theme-zenith", r), r + "/storage/.config/emulationstation/themes/es-theme-zenith");

  // --log-path: stderr of a child goes to <dir>/zfront.log, appended
  const std::string ld = g_tmp + "/logs";
  mkdirs(ld);
  CHECK(!boot::redirect_log(g_tmp + "/no-such-dir"));
  for (int i = 0; i < 2; ++i) {
    pid_t pid = fork();
    if (pid == 0) {
      if (!boot::redirect_log(ld)) _exit(3);
      std::fprintf(stderr, "line %d\n", i);
      _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  }
  std::string text;
  CHECK(read_file(ld + "/zfront.log", text));
  CHECK_EQ(text, std::string("line 0\nline 1\n"));
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-gf-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  SystemConf::lock_path() = g_tmp + "/lock";
  ::mkdir((g_tmp + "/roms").c_str(), 0755);
  test_manager_closes_on_launch();
  test_unbound_costs_nothing();
  test_selected_events();
  test_bios_check();
  test_manager_bios_order();
  test_boot_args();
  std::string cmd = "rm -rf '" + g_tmp + "'";
  if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
  return zt::done();
}
