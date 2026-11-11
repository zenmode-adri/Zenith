// LAUNCH THIS GAME AT STARTUP (ES global.bootgame.*): the once-per-boot start (`boot::startup_game`), where it is allowed, and the
// per-game switch and the STOP row that write the keys ES reads (menu_startup.cpp). Temp dirs only; nothing is launched.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "bootargs.hpp"
#include "check.hpp"
#include "i18n.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "menu_startup.hpp"
#include "registry.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;

namespace {

std::string d;  // temp dir

std::string T(const char* k) { return std::string(tr(k)); }

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

bool exists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

std::string slurp(const std::string& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

Game game(const std::string& name, const std::string& path) {
  Game g;
  g.name = name;
  g.path = path;
  return g;
}

System make_gb() {
  System s;
  s.name = "gb";
  s.fullname = "Game Boy";
  s.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR%";
  s.emulator = "libretro";
  s.core = "gambatte";
  s.emus = {{"libretro", {"gambatte"}}};
  s.rom_dir = "/roms/gb";
  s.loaded = true;
  s.games = {game("Tetris", "/roms/gb/Tetris.gb"), game("Zelda", "/roms/gb/Zelda.gb")};
  s.count = 2;
  return s;
}

/// A fresh context over temp settings files, with the Game Boy system and no RetroAchievements.
void fresh(Ctx& cx) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640;
  cx.h = 480;
  cx.sv->cmd.dry_run = false;
  { std::ofstream(d + "/system.cfg") << "audio.volume=50\n"; }
  cx.conf = SystemConf::load(d + "/system.cfg");
  cx.es = EsSettings::from_text("<config/>", d + "/es_settings.cfg");
  cx.systems = {make_gb()};
  SystemConf::lock_path() = d + "/lock";
}

Menu& as_menu(std::unique_ptr<Screen>& s) { return *static_cast<Menu*>(s.get()); }

int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}

/// Moves the cursor onto `row` and presses A (a switch flips, an action runs).
void press_row(Menu& m, Ctx& cx, int row) {
  for (size_t i = 0; i < m.size() + 1 && m.cursor() != size_t(row); ++i) m.handle(Action::Down, cx);
  m.handle(Action::A, cx);
}

}  // namespace

int main() {
  char tpl[] = "/tmp/zf-startup-XXXXXX";
  CHECK(::mkdtemp(tpl) != nullptr);
  d = tpl;

  // ---- which runs may start the boot game: never a preview, a dry run, a dump or a harness run (--script, demo systems)
  {
    CHECK(boot::startup_game_allowed(false, false, false, false));
    CHECK(!boot::startup_game_allowed(true, false, false, false));
    CHECK(!boot::startup_game_allowed(false, true, false, false));
    CHECK(!boot::startup_game_allowed(false, false, true, false));
    CHECK(!boot::startup_game_allowed(false, false, false, true));
  }

  // ---- once per boot: the first start of the boot gets the game, every later start of the frontend gets nothing
  {
    const std::string marker = d + "/run/zfront/startup-game";  // the folder does not exist yet: it is made
    const std::string rom = "/roms/gb/Tetris.gb";
    auto yes = [](const std::string&) { return true; };
    auto no = [](const std::string&) { return false; };
    std::string why;
    CHECK(boot::startup_game("", marker, yes, why).empty());  // no game set
    CHECK(why.empty());
    CHECK(boot::startup_game(rom, marker, no, why).empty());  // the ROM is not there (partition not mounted, file removed)
    CHECK(!why.empty());
    CHECK(!exists(marker));  // ...and then nothing is claimed, so a later start can still play it
    CHECK_EQ(boot::startup_game(rom, marker, yes, why), rom);
    CHECK(why.empty());
    CHECK(exists(marker));
    CHECK(boot::startup_game(rom, marker, yes, why).empty());  // systemd restarts zfront after a quit or a crash: no second start
    CHECK(why.empty());
    CHECK(boot::startup_game(rom, marker, yes, why).empty());
    // the marker cannot be written (a file where its folder should be): nothing starts, and the reason says so
    { std::ofstream(d + "/plain") << "x"; }
    CHECK(boot::startup_game(rom, d + "/plain/startup-game", yes, why).empty());
    CHECK(!why.empty());
  }

  // ---- the per-game switch writes the ES keys; another game replaces it; off removes both lines
  {
    Ctx cx;
    fresh(cx);
    const std::string cfg = d + "/system.cfg";
    CHECK(!mg::is_startup_game(cx, "/roms/gb/Tetris.gb"));

    auto game_options = [&](const std::string& rom) {
      std::unique_ptr<Screen> s = Registry::instance().make("menu.game_options", cx, mg::game_arg("gb", rom));
      CHECK(s != nullptr);
      as_menu(s).rebuild(cx);
      return s;
    };
    {
      auto s = game_options("/roms/gb/Tetris.gb");
      Menu& m = as_menu(s);
      const int row = find(m, T("LAUNCH THIS GAME AT STARTUP"));
      CHECK(row >= 0);
      press_row(m, cx, row);
      CHECK(mg::is_startup_game(cx, "/roms/gb/Tetris.gb"));
      const std::string text = slurp(cfg);
      CHECK(contains(text, "global.bootgame.path=/roms/gb/Tetris.gb\n"));
      CHECK(contains(text, "global.bootgame.cmd=runemu.sh /roms/gb/Tetris.gb"));  // ES starts the same command if the user goes back to ES
    }
    {
      auto s = game_options("/roms/gb/Zelda.gb");
      Menu& m = as_menu(s);
      CHECK(find(m, T("LAUNCH THIS GAME AT STARTUP")) >= 0);
      press_row(m, cx, find(m, T("LAUNCH THIS GAME AT STARTUP")));
      CHECK(mg::is_startup_game(cx, "/roms/gb/Zelda.gb"));
      CHECK(!mg::is_startup_game(cx, "/roms/gb/Tetris.gb"));  // one game at a time, as in ES
      CHECK(contains(slurp(cfg), "global.bootgame.path=/roms/gb/Zelda.gb\n"));
    }
    {
      auto s = game_options("/roms/gb/Zelda.gb");
      Menu& m = as_menu(s);
      press_row(m, cx, find(m, T("LAUNCH THIS GAME AT STARTUP")));  // off
      CHECK(!mg::is_startup_game(cx, "/roms/gb/Zelda.gb"));
      const std::string text = slurp(cfg);
      CHECK(!contains(text, "global.bootgame.path"));
      CHECK(!contains(text, "global.bootgame.cmd"));
    }
    // a system's options do not offer it, and a game that is not in the system cannot be set
    {
      std::unique_ptr<Screen> s = Registry::instance().make("menu.system_options", cx, "gb");
      as_menu(s).rebuild(cx);
      CHECK(find(as_menu(s), T("LAUNCH THIS GAME AT STARTUP")) < 0);
      CHECK(!mg::set_startup_game(cx, "gb", "/roms/gb/Nope.gb"));
      CHECK(!mg::is_startup_game(cx, "/roms/gb/Nope.gb"));
    }
  }

  // ---- a system.cfg that cannot be written arms nothing and says so
  {
    Ctx cx;
    fresh(cx);
    cx.conf = SystemConf();  // no path: every write fails
    CHECK(!mg::set_startup_game(cx, "gb", "/roms/gb/Zelda.gb"));
    CHECK(!mg::is_startup_game(cx, "/roms/gb/Zelda.gb"));
    CHECK(!cx.sv->toasts.items().empty());
  }

  // ---- the STOP row of the games menu: only while a game is set, and it clears it
  {
    Ctx cx;
    fresh(cx);
    auto games_menu = [&] {
      std::unique_ptr<Screen> s = Registry::instance().make("menu.game", cx, "");
      as_menu(s).rebuild(cx);
      return s;
    };
    {
      auto s = games_menu();
      CHECK(find(as_menu(s), T("STOP LAUNCHING THIS GAME AT STARTUP")) < 0);
    }
    CHECK(mg::set_startup_game(cx, "gb", "/roms/gb/Zelda.gb"));
    {
      auto s = games_menu();
      const int row = find(as_menu(s), T("STOP LAUNCHING THIS GAME AT STARTUP"));
      CHECK(row >= 0);
      press_row(as_menu(s), cx, row);
    }
    CHECK(!mg::is_startup_game(cx, "/roms/gb/Zelda.gb"));
    CHECK(!contains(slurp(d + "/system.cfg"), "global.bootgame"));
  }

  std::string cleanup = "rm -rf " + d;
  if (std::system(cleanup.c_str()) != 0) {
  }
  return zt::done();
}
