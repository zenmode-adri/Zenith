#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

#include "check.hpp"
#include "cpu.hpp"
#include "gfx.hpp"
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
  const std::string shot(200, 'x');
  put(dir + "/Alleyway (World).state.auto.png", shot, now - 3600);
  put(dir + "/Alleyway (World).state1.png", shot, now - 600);  // newer slot of the same game
  put(dir + "/Tetris.state.auto.png", shot, now - 2 * 86400);
  put(dir + "/Tetris.state1.png", "x", now - 3 * 3600);        // newer but no picture in it: the older one stays
  put(dir + "/Mario.state.auto.png", shot, now - 40 * 86400);  // old: still listed, with no "ago"
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
    CHECK(r[1].shot == dir + "/Tetris.state.auto.png");
    CHECK(r[2].name == "Mario");
    CHECK(r[2].ago.empty());
  }
  CHECK(recent_states(sys, now, 1).size() == 1);
  System none;
  none.name = "psp";
  none.rom_dir = root + "/roms/psp";
  CHECK(recent_states(none, now, 3).empty());  // no folder: no row

  // the picture key carries the time the file was written: a state saved again under the same name shows its new screenshot
  CHECK(r[0].mtime == now - 600);
  CHECK(r[0].shot_key() != r[0].shot);
  CHECK(r[0].shot_key().find(r[0].shot) == 0);
  RecentItem by_hand;  // an item made by hand (no time) keeps its plain path
  by_hand.shot = "/x.png";
  CHECK(by_hand.shot_key() == "/x.png");
  {
    const std::string pic = root + "/roms/savestates/gb/Redo.state.auto.png";
    auto make = [&](bool red, time_t when) {
      Canvas c;
      c.w = c.h = 32;
      c.px.assign(size_t(c.w) * c.h * 4, 255);
      for (uint32_t y = 0; y < c.h; ++y)
        for (uint32_t x = 0; x < c.w; ++x) {
          uint8_t* p = &c.px[(size_t(y) * c.w + x) * 4];
          p[0] = red ? 255 : 0;
          p[2] = red ? 0 : 255;
          if (x || y) p[1] = uint8_t((x * 13 + y * 7) % 256);  // noise: a real picture (and over 128 bytes)
          else p[1] = 0;
        }
      std::string err;
      CHECK(save_png(c, pic, err));
      struct timeval tv[2] = {{when, 0}, {when, 0}};
      utimes(pic.c_str(), tv);
    };
    make(true, now - 100);
    auto find_redo = [&](std::vector<RecentItem>& items) -> const RecentItem* {
      for (const RecentItem& it : items)
        if (it.name == "Redo") return &it;
      return nullptr;
    };
    std::vector<RecentItem> first = recent_states(sys, now, 10);
    const RecentItem* f = find_redo(first);
    CHECK(f != nullptr);
    Assets assets;
    ImgId id1 = 0, id2 = 0;
    if (f) {
      CHECK(assets.file_fit(f->shot_key(), 32, 32, id1));
      CHECK_EQ(assets.img(id1).rgba[0], uint8_t(255));  // red
      make(false, now - 50);                            // played again: the same path, blue, newer
      std::vector<RecentItem> second = recent_states(sys, now, 10);
      const RecentItem* g = find_redo(second);
      CHECK(g != nullptr);
      if (g) {
        CHECK(g->shot == f->shot);
        CHECK(g->shot_key() != f->shot_key());
        CHECK(assets.file_fit(g->shot_key(), 32, 32, id2));
        CHECK_EQ(assets.img(id2).rgba[2], uint8_t(255));  // blue: not the picture cached under the old key
        CHECK_EQ(assets.img(id2).rgba[0], uint8_t(0));
      }
    }
    std::remove(pic.c_str());
  }

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
