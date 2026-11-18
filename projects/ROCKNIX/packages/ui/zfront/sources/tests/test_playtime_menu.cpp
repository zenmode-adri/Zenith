// PLAY TIME LIMIT with the settings and the menus (playtime.hpp): the row in GAME SETTINGS writes `zfront.playlimit`, Off by default, and
// the launch gate / TIME LEFT TODAY text read the day's file. Temp dirs only; no game is started.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "playtime.hpp"
#include "registry.hpp"
#include "screens.hpp"
#include "util.hpp"

using namespace zf;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

void spit(const std::string& p, const std::string& t) {
  std::ofstream f(p, std::ios::binary);
  f << t;
}

std::string slurp(const std::string& p) {
  std::string t;
  read_file(p, t);
  return t;
}

int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}

}  // namespace

int main() {
  std::string tmpl = zt::tmp_base() + "/zf-test-playtime-menu-XXXXXX";
  const char* dp = mkdtemp(&tmpl[0]);
  CHECK(dp != nullptr);
  if (!dp) return 1;
  const std::string dir = dp;
  mg::env().root = dir + "/root";
  ::mkdir((dir + "/root").c_str(), 0755);
  mg::reset_caches();
  ::setenv("ZF_PLAYTIME_FILE", (dir + "/playtime").c_str(), 1);

  Ctx cx;
  {
    std::string err;
    CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
    cx.w = 640;
    cx.h = 480;
  }
  cx.sv->cmd.dry_run = true;
  SystemConf::lock_path() = dir + "/lock";
  spit(dir + "/system.cfg", "audio.volume=50\n");
  cx.conf = SystemConf::load(dir + "/system.cfg");

  // Off by default: nothing blocks, nothing is read, no text
  CHECK_EQ(playtime::limit_of(cx.conf), 0);
  CHECK(!playtime::launch_blocked(cx.conf));
  CHECK_EQ(playtime::minutes_left(cx.conf), -1);
  CHECK(playtime::left_text(cx.conf).empty());
  CHECK(playtime::begin_game(cx.conf) == nullptr);  // no thread, no file when Off

  // the row in GAME SETTINGS
  std::unique_ptr<Screen> s = Registry::instance().make("menu.game", cx, "");
  CHECK(s != nullptr);
  Menu* m = static_cast<Menu*>(s.get());
  m->rebuild(cx);
  const int row = find(*m, "PLAY TIME LIMIT");
  CHECK(row >= 0);
  CHECK(row > find(*m, "WHILE PLAYING"));
  CHECK(row < find(*m, "SAVED GAMES"));
  if (row >= 0) {
    CHECK_EQ(m->entry(size_t(row)).options.size(), size_t(7));  // Off, 15, 30, 45, 60, 90, 120
    CHECK_EQ(m->entry(size_t(row)).options[0].value, std::string(""));
    CHECK_EQ(m->entry(size_t(row)).options[6].value, std::string("120"));
    m->entry(size_t(row)).set_s("30");
    CHECK_EQ(slurp(dir + "/system.cfg").find("zfront.playlimit=30") != std::string::npos, true);
    CHECK_EQ(playtime::limit_of(cx.conf), 30);
    m->entry(size_t(row)).set_s("");
    CHECK_EQ(slurp(dir + "/system.cfg").find("zfront.playlimit"), std::string::npos);
    CHECK_EQ(playtime::limit_of(cx.conf), 0);
  }

  // with a limit and today's time on file
  const playtime::Date today = playtime::local_today();
  const std::string day = today.valid() ? today.key() : std::string();
  if (!day.empty()) {  // (a PC with a lost clock: the rest cannot say anything about today)
    spit(dir + "/system.cfg", "zfront.playlimit=30\n");
    cx.conf = SystemConf::load(dir + "/system.cfg");
    spit(dir + "/playtime", "zfront-playtime 1\nday=" + day + "\nused=300\n");  // 5 min played
    playtime::reset_store();
    CHECK(!playtime::launch_blocked(cx.conf));
    CHECK_EQ(playtime::minutes_left(cx.conf), 25);
    CHECK_EQ(playtime::left_text(cx.conf), std::string("25 min"));
    spit(dir + "/playtime", "zfront-playtime 1\nday=" + day + "\nused=1800\n");  // all of it
    playtime::reset_store();
    CHECK(playtime::launch_blocked(cx.conf));
    CHECK_EQ(playtime::left_text(cx.conf), std::string("0 min"));
    spit(dir + "/system.cfg", "zfront.playlimit=60\n");  // the limit raised
    cx.conf = SystemConf::load(dir + "/system.cfg");
    CHECK(!playtime::launch_blocked(cx.conf));
    spit(dir + "/system.cfg", "zfront.playlimit=\n");  // Off again
    cx.conf = SystemConf::load(dir + "/system.cfg");
    CHECK(!playtime::launch_blocked(cx.conf));
    // yesterday's file: a new day
    spit(dir + "/playtime", "zfront-playtime 1\nday=2025-01-01\nused=100000\n");
    spit(dir + "/system.cfg", "zfront.playlimit=30\n");
    cx.conf = SystemConf::load(dir + "/system.cfg");
    playtime::reset_store();
    CHECK(!playtime::launch_blocked(cx.conf));
    CHECK_EQ(playtime::minutes_left(cx.conf), 30);
    // a corrupt file: a clean day, not a crash
    spit(dir + "/playtime", "\x01garbage");
    playtime::reset_store();
    CHECK(!playtime::launch_blocked(cx.conf));
    // begin_game with a limit makes a watcher; stopping it writes the day's file
    std::unique_ptr<playtime::Watcher> w = playtime::begin_game(cx.conf);
    CHECK(w != nullptr);
    if (w) w->stop();
    CHECK(slurp(dir + "/playtime").find("zfront-playtime 1") == 0);
  }

  playtime::reset_store();
  const std::string rm = "rm -rf '" + dir + "'";
  if (std::system(rm.c_str()) != 0) {}
  return zt::done();
}
