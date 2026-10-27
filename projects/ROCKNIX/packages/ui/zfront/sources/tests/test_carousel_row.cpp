// "Continue playing" row of the carousel: Down gives it the keys, Left/Right pick a tile (no wrap), A continues that game from its
// savestate (the same launch as the savestate manager), B/Up give the keys back; nothing changes where the row is not shown.
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "savestates.hpp"
#include "screens.hpp"
#include "util.hpp"

using namespace zf;
namespace ss = zf::savestates;

namespace {

std::string g_tmp;

std::string g_png;  // a real picture: the row decodes it

void put(const std::string& path, time_t mtime) {
  std::ofstream(path, std::ios::trunc | std::ios::binary) << (path.size() > 4 && path.substr(path.size() - 4) == ".png" ? g_png : std::string("x"));
  struct utimbuf u;
  u.actime = u.modtime = mtime;
  ::utime(path.c_str(), &u);
}

System make_system(const std::string& name, const std::string& emulator, const std::vector<std::string>& games) {
  System s;
  s.name = name;
  s.fullname = name;
  s.theme = name;
  s.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR%";
  s.emulator = emulator;
  s.core = "gambatte";
  s.emus = {{emulator, {"gambatte"}}};
  s.rom_dir = g_tmp + "/roms/" + name;
  s.exts = {".gb"};
  s.gamelist = s.rom_dir + "/gamelist.xml";
  for (const std::string& n : games) {
    Game g;
    g.name = n;
    g.path = s.rom_dir + "/" + n + ".gb";
    s.games.push_back(std::move(g));
  }
  s.count = s.games.size();
  s.loaded = true;
  return s;
}

std::string theme_dir() {
  const char* e = std::getenv("ZF_THEME_DIR");
  return e ? e : "/home/epsil/zenith-frontend/theme-dark";
}

// Savestates of "gb": Alpha (autosave, newest), Bravo (slot 1), Charlie (a screenshot whose state file is gone), Delta (state of a ROM
// that is not in the list any more, oldest, past the 3 tiles of the row).
bool setup(Ctx& cx, float w, float h) {
  std::string err;
  Theme th;
  if (!Theme::load(theme_dir(), ratio_name(unsigned(w), unsigned(h)), th, err) || !cx.assets.load_font(th.path(th.str("font")), err)) {
    std::fprintf(stderr, "test_carousel_row: theme not found (%s): test skipped\n", err.c_str());
    return false;
  }
  cx.th = th;
  cx.w = w, cx.h = h;
  cx.sv->cmd.dry_run = true;
  cx.conf = SystemConf::from_text("", g_tmp + "/system.cfg");
  CHECK(load_common(cx.th, cx.assets, cx.common, err));
  cx.systems.push_back(make_system("gb", "retroarch", {"Alpha", "Bravo", "Charlie"}));
  cx.systems.push_back(make_system("pc", "dosbox", {"Doom"}));
  cx.items.push_back(SystemItem{"gb", "Game Boy", 3});
  cx.items.push_back(SystemItem{"pc", "PC", 1});
  return true;
}

void make_states() {
  const time_t now = time(nullptr);
  const std::string d = g_tmp + "/roms/savestates/gb";
  ::mkdir((g_tmp + "/roms").c_str(), 0755);
  ::mkdir((g_tmp + "/roms/gb").c_str(), 0755);
  ::mkdir((g_tmp + "/roms/savestates").c_str(), 0755);
  ::mkdir(d.c_str(), 0755);
  put(d + "/Alpha.state.auto", now - 60);
  put(d + "/Alpha.state.auto.png", now - 60);
  put(d + "/Bravo.state1", now - 600);
  put(d + "/Bravo.state1.png", now - 600);
  put(d + "/Charlie.state.auto.png", now - 3600);  // no state file next to it
  put(d + "/Delta.state.auto", now - 7200);
  put(d + "/Delta.state.auto.png", now - 7200);
}

// a carousel at rest on system 0 with the row read
void settle(CarouselScreen& c, Ctx& cx) {
  for (int i = 0; i < 3; ++i) c.tick(1.0f / 60.0f, cx);
}

bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-carrow-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  CHECK(read_file(std::string(ZF_TEST_DATA) + "/cover-box.png", g_png));
  make_states();

  {  // before the first tick the row is not read yet: Down does nothing
    Ctx cx;
    if (!setup(cx, 720, 720)) return 0;
    CarouselScreen c(0);
    CHECK(c.handle(Action::Down, cx).kind == Transition::None);
    CHECK_EQ(c.row_focus(), -1);
  }

  {  // keys: Down enters, Left/Right move without wrapping, Up/B leave
    Ctx cx;
    if (!setup(cx, 720, 720)) return 0;
    CarouselScreen c(0);
    settle(c, cx);
    CHECK_EQ(c.recent_items().size(), size_t(3));  // Alpha, Bravo, Charlie (Delta is the 4th: past the limit of 3)
    CHECK(c.recent_items().size() == 3 && c.recent_items()[0].name == "Alpha" && c.recent_items()[0].slot == -1);
    CHECK(c.recent_items().size() == 3 && c.recent_items()[1].name == "Bravo" && c.recent_items()[1].slot == 1);
    CHECK_EQ(c.row_focus(), -1);
    CHECK(c.handle(Action::Down, cx).kind == Transition::None);
    CHECK_EQ(c.row_focus(), 0);
    CHECK_EQ(c.ring_alpha(), 1.0f);  // no motion: the ring is there at once
    c.handle(Action::Left, cx);
    CHECK_EQ(c.row_focus(), 0);  // no wrap at the left end
    c.handle(Action::Right, cx);
    CHECK_EQ(c.row_focus(), 1);
    c.handle(Action::Right, cx);
    CHECK_EQ(c.row_focus(), 2);
    c.handle(Action::Right, cx);
    CHECK_EQ(c.row_focus(), 2);  // nor at the right end
    c.handle(Action::Down, cx);
    CHECK_EQ(c.row_focus(), 2);  // Down on the row does nothing
    CHECK(c.handle(Action::B, cx).kind == Transition::None);
    CHECK_EQ(c.row_focus(), -1);
    CHECK_EQ(c.ring_alpha(), 0.0f);
    c.handle(Action::Down, cx);
    CHECK_EQ(c.row_focus(), 0);
    c.handle(Action::Up, cx);
    CHECK_EQ(c.row_focus(), -1);
    // the carousel keys are not eaten while the carousel has them
    CHECK(c.handle(Action::Start, cx).kind == Transition::Open);
    // the carousel did not move while the row had the keys
    c.handle(Action::Down, cx);
    c.handle(Action::Right, cx);
    CHECK_EQ(c.camera(), 0.0f);
    // Start keeps the keys on the row (the menu opens over it); a trigger gives them back and steps the carousel
    CHECK(c.handle(Action::Start, cx).kind == Transition::Open);
    CHECK_EQ(c.row_focus(), 1);
    c.handle(Action::R2, cx);
    CHECK_EQ(c.row_focus(), -1);
    for (int i = 0; i < 40; ++i) c.tick(1.0f / 60.0f, cx);
    CHECK_EQ(c.camera(), 1.0f);
  }

  {  // the row is not there (system without savestates, or the carousel is moving): Down does nothing, the carousel keeps its keys
    Ctx cx;
    if (!setup(cx, 720, 720)) return 0;
    CarouselScreen c(1);  // "pc": no folder of savestates
    settle(c, cx);
    CHECK(c.recent_items().empty());
    c.handle(Action::Down, cx);
    CHECK_EQ(c.row_focus(), -1);
    CarouselScreen m(0);
    settle(m, cx);
    m.handle(Action::Right, cx);  // moving
    m.handle(Action::Down, cx);
    CHECK_EQ(m.row_focus(), -1);
  }

  {  // a 4:3 screen has the room since the carousel is lifted (640x480): the row is visible and Down gives it the keys
    Ctx cx;
    if (!setup(cx, 640, 480)) return 0;
    CarouselScreen c(0);
    settle(c, cx);
    CHECK(!recent_visible(cx.th, cx.assets, cx.w, cx.h, cx.items[0], RecentRow{c.recent_items()}).empty());
    c.handle(Action::Down, cx);
    CHECK_EQ(c.row_focus(), 0);
  }

  {  // a screen without room for the row (720x405, 480x320): same as before, Down does nothing
    for (auto sz : {std::pair<float, float>{720, 405}, {480, 320}}) {
      Ctx cx;
      if (!setup(cx, sz.first, sz.second)) return 0;
      CarouselScreen c(0);
      settle(c, cx);
      CHECK_EQ(c.recent_items().size(), size_t(3));  // the data is read, the geometry says it does not fit
      CHECK(recent_visible(cx.th, cx.assets, cx.w, cx.h, cx.items[0], RecentRow{c.recent_items()}).empty());
      c.handle(Action::Down, cx);
      CHECK_EQ(c.row_focus(), -1);
    }
  }

  {  // A continues from the state: the same arguments as picking it in the savestate manager
    Ctx cx;
    if (!setup(cx, 720, 720)) return 0;
    CarouselScreen c(0);
    settle(c, cx);
    c.handle(Action::Down, cx);
    Transition t = c.handle(Action::A, cx);  // Alpha: the autosave
    CHECK(t.kind == Transition::Launch);
    CHECK(has(t.cmd, "Alpha.gb"));
    CHECK(has(t.cmd, "-autosave 1"));
    CHECK_EQ(t.sys, 0);
    CHECK_EQ(t.game, 0);
    CHECK_EQ(c.row_focus(), -1);  // the game runs: when it ends the keys return to its tile (below)
    ss::after_game();
    // back from the game: the row is read again and the keys are on the tile of Alpha
    settle(c, cx);
    CHECK_EQ(c.row_focus(), 0);
    CHECK(c.recent_items()[0].name == "Alpha");

    c.handle(Action::Right, cx);
    CHECK_EQ(c.row_focus(), 1);
    ss::invalidate();
    t = c.handle(Action::A, cx);  // Bravo: slot 1 (copied over the autosave for RetroArch to load)
    CHECK(t.kind == Transition::Launch);
    CHECK(has(t.cmd, "Bravo.gb"));
    CHECK(has(t.cmd, "-autosave 1"));
    CHECK_EQ(t.game, 1);
    CHECK(ss::plan_active());
    ss::after_game();
    CHECK(!ss::plan_active());

    // Charlie: only its screenshot is left, no state to load: a plain launch
    settle(c, cx);
    for (int i = 0; i < 6 && c.row_focus() != 2; ++i) c.handle(c.row_focus() < 0 ? Action::Down : Action::Right, cx);
    CHECK_EQ(c.row_focus(), 2);
    t = c.handle(Action::A, cx);
    CHECK(t.kind == Transition::Launch);
    CHECK(has(t.cmd, "Charlie.gb"));
    CHECK(!has(t.cmd, "-autosave 1"));
    CHECK(!ss::plan_active());
  }

  {  // a state whose ROM is gone (not in the game list, not on disk): its tile is not shown, the next game takes the place
    Ctx cx;
    if (!setup(cx, 720, 720)) return 0;
    cx.systems[0].games.erase(cx.systems[0].games.begin());  // Alpha is gone
    CarouselScreen c(0);
    settle(c, cx);
    CHECK_EQ(c.recent_items().size(), size_t(2));  // Bravo, Charlie; Delta has no ROM either
    CHECK(c.recent_items().size() == 2 && c.recent_items()[0].name == "Bravo" && c.recent_items()[1].name == "Charlie");
    c.handle(Action::Down, cx);
    CHECK_EQ(c.row_focus(), 0);
    CHECK(c.handle(Action::A, cx).kind == Transition::Launch);  // Bravo
  }

  {  // a ROM found on disk (flat folder) counts even when the game list does not have it: Delta takes the third tile
    Ctx cx;
    if (!setup(cx, 720, 720)) return 0;
    cx.systems[0].games.erase(cx.systems[0].games.begin());
    put(g_tmp + "/roms/gb/Delta.gb", time(nullptr));
    CarouselScreen c(0);
    settle(c, cx);
    CHECK_EQ(c.recent_items().size(), size_t(3));
    CHECK(c.recent_items().size() == 3 && c.recent_items()[2].name == "Delta");
    ::unlink((g_tmp + "/roms/gb/Delta.gb").c_str());
  }

  {  // motion: the ring fades in and out (it never jumps), the focus itself is immediate
    Ctx cx;
    if (!setup(cx, 720, 720)) return 0;
    cx.ui_motion = true;
    CarouselScreen c(0);
    settle(c, cx);
    c.handle(Action::Down, cx);
    CHECK_EQ(c.row_focus(), 0);
    CHECK_EQ(c.ring_alpha(), 0.0f);
    float prev = 0;
    int frames = 0;
    while (c.tick(1.0f / 60.0f, cx) && frames < 60) {
      CHECK(c.ring_alpha() >= prev);
      prev = c.ring_alpha();
      ++frames;
    }
    CHECK_EQ(c.ring_alpha(), 1.0f);
    CHECK(frames >= 4 && frames < 12);  // ~100 ms
    c.handle(Action::B, cx);
    CHECK_EQ(c.row_focus(), -1);
    CHECK_EQ(c.ring_alpha(), 1.0f);  // still fading out
    frames = 0;
    while (c.tick(1.0f / 60.0f, cx) && frames < 60) {
      CHECK(c.ring_alpha() <= prev);
      prev = c.ring_alpha();
      ++frames;
    }
    CHECK_EQ(c.ring_alpha(), 0.0f);
    CHECK(!c.tick(1.0f / 60.0f, cx));  // at rest nothing to draw
  }

  {  // drawing: the ring adds operations, the help bar names the keys of the row, and the frame without focus is untouched
    Ctx cx;
    if (!setup(cx, 720, 720)) return 0;
    CarouselScreen c(0);
    settle(c, cx);
    std::vector<Op> plain, focused;
    c.draw(cx, plain);
    c.handle(Action::Down, cx);
    c.draw(cx, focused);
    CHECK(focused.size() > plain.size());
    std::vector<Op> back;
    c.handle(Action::B, cx);
    c.draw(cx, back);
    CHECK_EQ(back.size(), plain.size());
  }

  std::string cmd = "rm -rf " + g_tmp;
  if (std::system(cmd.c_str()) != 0) {}
  return zt::done();
}
