// menu-collections: savestates (file naming, launch arguments, state preparation and undo, renumbering) and the manager screen.
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "registry.hpp"
#include "savestates.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;
namespace ss = zf::savestates;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

const unsigned char kPng[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01,
                              0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41,
                              0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
                              0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

std::string g_tmp;

std::string slurp(const std::string& p) {
  std::string t;
  read_file(p, t);
  return t;
}
void spit(const std::string& p, const std::string& t) { write_atomic(p, t); }

void set_mtime(const std::string& p, time_t t) {
  struct utimbuf u;
  u.actime = u.modtime = t;
  ::utime(p.c_str(), &u);
}

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
  cx.systems.push_back(make_system("gb", "retroarch", {"Tetris", "Zelda"}));
  cx.systems.push_back(make_system("pc", "dosbox", {"Doom"}));
}

void reset(const std::string& dir) {
  std::string cmd = "rm -rf '" + dir + "' && mkdir -p '" + dir + "'";
  if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "reset failed\n");
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-ss-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  SystemConf::lock_path() = g_tmp + "/lock";
  const std::string dir = g_tmp + "/roms/savestates/gb";
  ::mkdir((g_tmp + "/roms").c_str(), 0755);

  // ---- names
  {
    std::string stem;
    int slot = -2;
    CHECK(ss::parse_name("Tetris.state", stem, slot));
    CHECK_EQ(stem, std::string("Tetris"));
    CHECK_EQ(slot, 0);
    CHECK(ss::parse_name("Tetris.state12", stem, slot));
    CHECK_EQ(slot, 12);
    CHECK(ss::parse_name("Tetris.state.auto", stem, slot));
    CHECK_EQ(slot, -1);
    CHECK(ss::parse_name("A.B.state3", stem, slot));  // the stem keeps its dots
    CHECK_EQ(stem, std::string("A.B"));
    CHECK(!ss::parse_name("Tetris.state.png", stem, slot));
    CHECK(!ss::parse_name("Tetris.state3.png", stem, slot));
    CHECK(!ss::parse_name("Tetris.sav", stem, slot));
    CHECK(!ss::parse_name(".state", stem, slot));
    CHECK(!ss::parse_name(".state.auto", stem, slot));
    CHECK_EQ(ss::state_path("/d", "T", 0), std::string("/d/T.state"));
    CHECK_EQ(ss::state_path("/d", "T", 3), std::string("/d/T.state3"));
    CHECK_EQ(ss::state_path("/d", "T", -1), std::string("/d/T.state.auto"));
    System s = make_system("gb", "retroarch", {});
    CHECK_EQ(ss::state_dir(s), g_tmp + "/roms/savestates/gb");  // beside the ROM folder, like ES
    std::vector<ss::State> v;
    CHECK_EQ(ss::next_free_slot(v), 0);
    v.push_back({-1, "", "", 0});
    CHECK_EQ(ss::next_free_slot(v), 0);  // only an autosave: slot 0 (ES computes -99 here)
    v.push_back({0, "", "", 0});
    v.push_back({4, "", "", 0});
    CHECK_EQ(ss::next_free_slot(v), 5);
  }

  // ---- listing, arguments
  {
    reset(dir);
    Ctx cx;
    make_ctx(cx, "");
    const System& gb = cx.systems[0];
    const Game& tetris = gb.games[0];
    CHECK(ss::enabled(gb, tetris, cx.conf.map()));
    CHECK(!ss::enabled(cx.systems[1], cx.systems[1].games[0], cx.conf.map()));  // not RetroArch
    CHECK(ss::plain_args(gb, tetris, cx.conf.map()).empty());                    // nothing saved yet
    CHECK(ss::plain_args(cx.systems[1], cx.systems[1].games[0], cx.conf.map()).empty());
    CHECK(ss::list(gb, tetris).empty());

    spit(dir + "/Tetris.state", "A");
    spit(dir + "/Tetris.state1", "B");
    spit(dir + "/Tetris.state1.png", std::string(reinterpret_cast<const char*>(kPng), sizeof kPng));
    spit(dir + "/Tetris.state.auto", "AUTO");
    spit(dir + "/Zelda.state", "Z");  // another game: not listed for Tetris
    spit(dir + "/notes.txt", "x");
    ss::invalidate();
    auto v = ss::list(gb, tetris);
    CHECK_EQ(v.size(), size_t(3));
    CHECK_EQ(v[0].slot, -1);  // autosave first, then the slots ascending
    CHECK_EQ(v[1].slot, 0);
    CHECK_EQ(v[2].slot, 1);
    CHECK(v[0].shot.empty());
    CHECK_EQ(v[2].shot, dir + "/Tetris.state1.png");
    set_mtime(dir + "/Tetris.state", 1700000000);
    ss::with_times(v);
    CHECK_EQ(v[1].mtime, time_t(1700000000));
    CHECK_EQ(ss::plain_args(gb, tetris, cx.conf.map()), std::string(" -state_slot 2"));  // incremental: next free slot
    CHECK_EQ(ss::plain_args(cx, 0, 0), std::string(" -state_slot 2"));
    CHECK(ss::plain_args(cx, 7, 0).empty());

    // the list is cached by the folder's identity: a new file shows up
    spit(dir + "/Tetris.state2", "C");
    CHECK_EQ(ss::plain_args(gb, tetris, cx.conf.map()), std::string(" -state_slot 3"));

    Ctx cx2;
    make_ctx(cx2, "global.incrementalsavestates=2\n");
    CHECK(ss::plain_args(cx2.systems[0], cx2.systems[0].games[0], cx2.conf.map()).empty());  // "current slot" mode
    Ctx cx3;
    make_ctx(cx3, "global.autosave=1\n");
    CHECK_EQ(ss::plain_args(cx3.systems[0], cx3.systems[0].games[1], cx3.conf.map()), std::string(" -autosave 1 -state_slot 1"));
    Ctx cx4;
    make_ctx(cx4, "gb[\"Zelda.gb\"].autosave=1\n");  // per-game setting beats the global one
    CHECK_EQ(ss::plain_args(cx4.systems[0], cx4.systems[0].games[1], cx4.conf.map()), std::string(" -autosave 1 -state_slot 1"));
    CHECK(ss::plain_args(cx4.systems[0], cx4.systems[0].games[0], cx4.conf.map()) == " -state_slot 3");

    // the three kinds of "new" launch
    CHECK_EQ(ss::begin(gb, tetris, cx.conf.map(), ss::Pick::NewGame, 0), std::string(" -autosave 0 -state_slot 3"));
    CHECK_EQ(ss::begin(gb, tetris, cx.conf.map(), ss::Pick::Auto, 0), std::string(" -autosave 1 -state_slot 3"));
    CHECK(!ss::plan_active());  // nothing was touched
    CHECK(ss::begin(cx.systems[1], cx.systems[1].games[0], cx.conf.map(), ss::Pick::NewGame, 0).empty());
  }

  // ---- loading a state: preparation and undo
  {
    reset(dir);
    Ctx cx;
    make_ctx(cx, "");
    const System& gb = cx.systems[0];
    const Game& g = gb.games[0];
    spit(dir + "/Tetris.state", "A");
    spit(dir + "/Tetris.state1", "B");
    spit(dir + "/Tetris.state.auto", "AUTO");
    spit(dir + "/Tetris.state.auto.png", "AUTOPNG");
    ss::invalidate();

    // state 0 (not the newest): the autosave is replaced, an extra copy is made at the next slot
    std::string args = ss::begin(gb, g, cx.conf.map(), ss::Pick::Slot, 0);
    CHECK_EQ(args, std::string(" -state_slot 2 -autosave 1"));
    CHECK(ss::plan_active());
    CHECK_EQ(slurp(dir + "/Tetris.state.auto"), std::string("A"));
    CHECK_EQ(slurp(dir + "/Tetris.state.auto.bak"), std::string("AUTO"));
    CHECK_EQ(slurp(dir + "/Tetris.state2"), std::string("A"));
    CHECK(!file_exists(dir + "/Tetris.state.auto.png"));  // the old screenshot does not describe the loaded state
    // the game ended without saving: everything goes back
    ss::after_game();
    CHECK(!ss::plan_active());
    CHECK_EQ(slurp(dir + "/Tetris.state.auto"), std::string("AUTO"));
    CHECK_EQ(slurp(dir + "/Tetris.state.auto.png"), std::string("AUTOPNG"));
    CHECK(!file_exists(dir + "/Tetris.state.auto.bak"));
    CHECK(!file_exists(dir + "/Tetris.state2"));  // untouched copy dropped
    CHECK_EQ(slurp(dir + "/Tetris.state1"), std::string("B"));
    ss::after_game();  // idempotent

    // the game saved into the extra slot: it stays
    ss::begin(gb, g, cx.conf.map(), ss::Pick::Slot, 0);
    spit(dir + "/Tetris.state2", "A2");
    ss::after_game();
    CHECK_EQ(slurp(dir + "/Tetris.state2"), std::string("A2"));
    CHECK_EQ(slurp(dir + "/Tetris.state.auto"), std::string("AUTO"));

    // the newest state needs no extra copy (slot + 1 == next free slot)
    reset(dir);
    spit(dir + "/Tetris.state", "A");
    spit(dir + "/Tetris.state1", "B");
    ss::invalidate();
    args = ss::begin(gb, g, cx.conf.map(), ss::Pick::Slot, 1);
    CHECK_EQ(args, std::string(" -state_slot 2 -autosave 1"));
    CHECK(!file_exists(dir + "/Tetris.state2"));
    CHECK_EQ(slurp(dir + "/Tetris.state.auto"), std::string("B"));
    ss::after_game();
    CHECK(!file_exists(dir + "/Tetris.state.auto"));  // there was none before

    // renumbering closes the holes a deleted/moved state leaves
    reset(dir);
    spit(dir + "/Tetris.state", "A");
    spit(dir + "/Tetris.state3", "D");
    spit(dir + "/Tetris.state3.png", "dpng");
    spit(dir + "/Tetris.state7", "H");
    spit(dir + "/Tetris.state.auto", "AUTO");
    ss::renumber(dir, "Tetris");
    CHECK_EQ(slurp(dir + "/Tetris.state1"), std::string("D"));
    CHECK_EQ(slurp(dir + "/Tetris.state1.png"), std::string("dpng"));
    CHECK_EQ(slurp(dir + "/Tetris.state2"), std::string("H"));
    CHECK(!file_exists(dir + "/Tetris.state3") && !file_exists(dir + "/Tetris.state7"));
    CHECK_EQ(slurp(dir + "/Tetris.state.auto"), std::string("AUTO"));  // the autosave never moves

    // delete and copy
    ss::invalidate();
    auto v = ss::list(gb, g);
    CHECK_EQ(v.size(), size_t(4));
    CHECK(ss::remove_state(gb, g, v[1]));  // slot 0
    CHECK_EQ(slurp(dir + "/Tetris.state"), std::string("D"));
    CHECK_EQ(slurp(dir + "/Tetris.state1"), std::string("H"));
    CHECK(!file_exists(dir + "/Tetris.state2"));
    v = ss::list(gb, g);
    unsigned gen = ss::generation();
    CHECK(ss::copy_to_free_slot(gb, g, v[1]));
    CHECK_EQ(slurp(dir + "/Tetris.state2"), std::string("D"));
    CHECK(ss::generation() != gen);
    CHECK(file_exists(dir + "/Tetris.state"));  // a copy: the original stays

    // a game that never reported back (power cut): the next time the manager opens, the autosave is put back
    spit(dir + "/Tetris.state.auto.bak", "OLDAUTO");
    ss::recover(gb, g);
    CHECK_EQ(slurp(dir + "/Tetris.state.auto"), std::string("OLDAUTO"));
    CHECK(!file_exists(dir + "/Tetris.state.auto.bak"));
  }

  // ---- hooks of the list: A opens the manager only when the setting says so
  {
    reset(dir);
    spit(dir + "/Tetris.state", "A");
    ss::invalidate();
    Ctx a, b, c, d;
    make_ctx(a, "");
    make_ctx(b, "global.savestates=1\n");
    make_ctx(c, "global.savestates=2\n");
    make_ctx(d, "gb.savestates=2\n");
    CHECK(ss::launch_request(a, 0, 0).kind == Transition::None);  // default: just launch
    Transition t = ss::launch_request(b, 0, 0);
    CHECK(t.kind == Transition::Open);
    CHECK_EQ(t.name, std::string("menu.savestates"));
    CHECK_EQ(t.arg, ss::make_arg(b.systems[0], b.systems[0].games[0]));
    CHECK(ss::launch_request(c, 0, 0).kind == Transition::Open);   // "2": only when it has states
    CHECK(ss::launch_request(c, 0, 1).kind == Transition::None);   // Zelda has none
    CHECK(ss::launch_request(d, 0, 0).kind == Transition::Open);   // system level setting
    CHECK(ss::launch_request(b, 1, 0).kind == Transition::None);   // not RetroArch
    CHECK(ss::launch_request(b, 9, 0).kind == Transition::None);
    CHECK(ss::open_manager(a, 0, 1).kind == Transition::Open);     // L3 opens it regardless of the setting
    CHECK(ss::open_manager(a, 1, 0).kind == Transition::None);
  }

  // ---- the manager screen
  {
    reset(dir);
    Ctx cx;
    make_ctx(cx, "global.savestates=1\n");
    const System& gb = cx.systems[0];
    const Game& g = gb.games[0];
    spit(dir + "/Tetris.state", "OLD");
    spit(dir + "/Tetris.state1", "NEW");
    spit(dir + "/Tetris.state1.png", std::string(reinterpret_cast<const char*>(kPng), sizeof kPng));
    set_mtime(dir + "/Tetris.state", 1700000000);
    set_mtime(dir + "/Tetris.state1", 1700100000);
    ss::invalidate();
    std::string arg = ss::make_arg(gb, g);
    CHECK(Registry::instance().has("menu.savestates"));
    CHECK(Registry::instance().make("menu.savestates", cx, "gb|/nowhere.gb") == nullptr);
    CHECK(Registry::instance().make("menu.savestates", cx, "pc|" + cx.systems[1].games[0].path) == nullptr);  // not RetroArch

    // drawing needs the theme's font: the harness renders it (see the review PNGs), not this test

    // first card = START NEW GAME (flush left): A launches a new game at the next free slot
    auto s = Registry::instance().make("menu.savestates", cx, arg);
    CHECK(s != nullptr);
    CHECK(s->overlay());
    Transition t = s->handle(Action::A, cx);
    CHECK(t.kind == Transition::Launch);
    CHECK(t.cmd.find("runemu.sh") == 0);
    CHECK(t.cmd.find(" -autosave 0 -state_slot 2") != std::string::npos);
    CHECK_EQ(t.sys, 0);
    CHECK_EQ(t.game, 0);

    // second card = the newest state (incremental mode sorts by date, newest first): A loads it
    s->handle(Action::Right, cx);
    t = s->handle(Action::A, cx);
    CHECK(t.kind == Transition::Launch);
    CHECK(t.cmd.find(" -state_slot 2 -autosave 1") != std::string::npos);
    CHECK_EQ(slurp(dir + "/Tetris.state.auto"), std::string("NEW"));
    ss::after_game();
    CHECK(!file_exists(dir + "/Tetris.state.auto"));

    // the older one
    s->handle(Action::Right, cx);
    t = s->handle(Action::A, cx);
    CHECK(t.cmd.find(" -state_slot 2 -autosave 1") != std::string::npos);
    CHECK_EQ(slurp(dir + "/Tetris.state.auto"), std::string("OLD"));
    CHECK_EQ(slurp(dir + "/Tetris.state2"), std::string("OLD"));
    ss::after_game();

    // the cursor stops at both ends
    for (int i = 0; i < 6; ++i) s->handle(Action::Right, cx);
    for (int i = 0; i < 6; ++i) s->handle(Action::Left, cx);
    t = s->handle(Action::A, cx);
    CHECK(t.cmd.find(" -autosave 0") != std::string::npos);

    // X copies the selected state, Y asks before deleting, B closes
    CHECK(s->handle(Action::Y, cx).kind == Transition::None);  // on the "new game" card: nothing to delete
    s->handle(Action::Right, cx);
    s->handle(Action::X, cx);
    CHECK_EQ(slurp(dir + "/Tetris.state2"), std::string("NEW"));
    Transition ask = s->handle(Action::Y, cx);
    CHECK(ask.kind == Transition::Push && ask.screen);
    CHECK(file_exists(dir + "/Tetris.state2"));  // still there: nothing happens until the answer
    Transition drop = s->handle(Action::B, cx);
    CHECK(drop.kind == Transition::Pop);

    // delete (YES is picked on the dialog): the file and its screenshot go, the others close the gap, the screen follows
    CHECK(file_exists(dir + "/Tetris.state1.png"));
    ask.screen->handle(Action::Left, cx);
    ask.screen->handle(Action::A, cx);
    CHECK(file_exists(dir + "/Tetris.state1.png"));  // the cursor was on slot 1 (kept through the re-sort): its screenshot went, the copy's moved down
    CHECK(!file_exists(dir + "/Tetris.state2.png"));
    CHECK_EQ(slurp(dir + "/Tetris.state1"), std::string("NEW"));  // slot 2 (the copy) moved down
    CHECK(!file_exists(dir + "/Tetris.state2"));
  }

  // ---- drawing at the four sizes (needs the real theme and font; skipped where the theme is not installed)
  {
    const std::string tdir = "/home/epsil/zenith-frontend/theme-dark";
    if (is_dir(tdir)) {
      reset(dir);
      spit(dir + "/Tetris.state", "OLD");
      spit(dir + "/Tetris.state1", "NEW");
      spit(dir + "/Tetris.state1.png", std::string(reinterpret_cast<const char*>(kPng), sizeof kPng));
      spit(dir + "/Tetris.state.auto", "AUTO");
      ss::invalidate();
      const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
      for (const auto& wh : sizes) {
        Ctx cx;
        make_ctx(cx, "global.savestates=1\n");
        std::string err;
        cx.w = float(wh[0]);
        cx.h = float(wh[1]);
        CHECK(Theme::load(tdir, ratio_name(wh[0], wh[1]), cx.th, err));
        CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
        auto s = Registry::instance().make("menu.savestates", cx, ss::make_arg(cx.systems[0], cx.systems[0].games[0]));
        CHECK(s != nullptr);
        for (int step = 0; step < 5; ++step) {
          std::vector<Op> a1, a2;
          s->draw(cx, a1);
          s->draw(cx, a2);
          CHECK(!a1.empty());
          CHECK_EQ(a1.size(), a2.size());  // nothing is rebuilt between frames
          s->handle(Action::Right, cx);
        }
        Transition ask = s->handle(Action::Y, cx);  // the delete confirmation draws too
        CHECK(ask.kind == Transition::Push && ask.screen);
        std::vector<Op> d;
        ask.screen->draw(cx, d);
        CHECK(!d.empty());
      }
    }
  }

  int rc = zt::done();
  std::string cmd = "rm -rf " + g_tmp;
  if (std::system(cmd.c_str()) != 0) return 1;
  return rc;
}
