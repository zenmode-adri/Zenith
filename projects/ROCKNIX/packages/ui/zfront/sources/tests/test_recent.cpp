#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

#include "check.hpp"
#include "recent.hpp"

using namespace zf;

static void put(const std::string& path, const std::string& content, time_t mtime) {
  std::ofstream(path, std::ios::trunc) << content;
  struct timeval tv[2] = {{mtime, 0}, {mtime, 0}};
  utimes(path.c_str(), tv);
}

int main() {
  const time_t now = 1'800'000'000;
  // ago_text: minutes, hours, days, nothing beyond 30 days or in the future
  CHECK(ago_text(now - 20, now).find('1') != std::string::npos);  // under a minute counts as 1 min
  CHECK(!ago_text(now - 12 * 60, now).empty());
  CHECK(!ago_text(now - 5 * 3600, now).empty());
  CHECK(!ago_text(now - 3 * 86400, now).empty());
  CHECK(ago_text(now - 31 * 86400, now).empty());
  CHECK(ago_text(now + 60, now).empty());
  CHECK(ago_text(0, now).empty());

  // recent_states: <rom_dir>/../savestates/<system>/<stem>.state*.png, newest per game, newest game first, other pngs ignored
  char tmpl[] = "/tmp/zf-recent-XXXXXX";
  std::string root = mkdtemp(tmpl);
  mkdir((root + "/roms").c_str(), 0755);
  mkdir((root + "/roms/gb").c_str(), 0755);
  mkdir((root + "/roms/savestates").c_str(), 0755);
  std::string dir = root + "/roms/savestates/gb";
  mkdir(dir.c_str(), 0755);
  put(dir + "/Alleyway (World).state.auto.png", "x", now - 3600);
  put(dir + "/Alleyway (World).state1.png", "x", now - 600);  // newer slot of the same game
  put(dir + "/Tetris.state.auto.png", "x", now - 2 * 86400);
  put(dir + "/Mario.state.auto.png", "x", now - 40 * 86400);  // old: still listed, with no "ago"
  put(dir + "/Wario.state.auto", "x", now - 10);               // a state without screenshot: no tile
  put(dir + "/screenshot-0001.png", "x", now - 5);             // not a savestate picture
  System sys;
  sys.name = "gb";
  sys.rom_dir = root + "/roms/gb";
  std::vector<RecentItem> r = recent_states(sys, now, 3);
  CHECK(r.size() == 3);
  if (r.size() == 3) {
    CHECK(r[0].name == "Alleyway (World)");
    CHECK(r[0].shot == dir + "/Alleyway (World).state1.png");
    CHECK(!r[0].ago.empty());
    CHECK(r[1].name == "Tetris");
    CHECK(r[2].name == "Mario");
    CHECK(r[2].ago.empty());
  }
  CHECK(recent_states(sys, now, 1).size() == 1);
  System none;
  none.name = "psp";
  none.rom_dir = root + "/roms/psp";
  CHECK(recent_states(none, now, 3).empty());  // no folder: no row

  // short_title: the region/dump tags of a rom name are clutter on a small tile
  CHECK(short_title("God of War (USA)") == "God of War");
  CHECK(short_title("Alleyway (World) (Rev 1)") == "Alleyway");
  CHECK(short_title("Tekken [!]") == "Tekken");
  CHECK(short_title("Super Mario Bros. Deluxe") == "Super Mario Bros. Deluxe");
  CHECK(short_title("(Prototype)") == "(Prototype)");  // never empty

  std::string cmd = "rm -rf " + root;
  if (std::system(cmd.c_str()) != 0) {}
  return zt::done();
}
