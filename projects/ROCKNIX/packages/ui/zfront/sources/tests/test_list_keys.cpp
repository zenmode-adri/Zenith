// Game list keys: X opens the options of the game under the cursor (as Select does), L2 / R2 jump to the previous / next
// letter, Start picks a random game, and the game options link to the collections of that game. Y (search) is at the end.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "registry.hpp"
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

// One system "gb" with the given games; nothing is written to disk (dry run, no lock, no settings file).
void make_ctx(Ctx& cx, const std::vector<std::string>& games) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = true;
  cx.es = EsSettings::from_text("<config/>", "/nonexistent/es_settings.cfg");
  cx.conf = SystemConf::from_text("", "/nonexistent/system.cfg");
  System s;
  s.name = "gb";
  s.fullname = "gb";
  s.theme = "gb";
  s.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR%";
  s.emulator = "retroarch";
  s.core = "gambatte";
  s.emus = {{"retroarch", {"gambatte"}}};
  s.rom_dir = "/roms/gb";
  s.gamelist = s.rom_dir + "/gamelist.xml";
  for (const std::string& n : games) {
    Game g;
    g.name = n;
    g.path = s.rom_dir + "/" + n + ".gb";
    s.games.push_back(std::move(g));
  }
  s.count = s.games.size();
  s.loaded = true;
  cx.systems.push_back(std::move(s));
}

// The game the list launches when A is pressed on its cursor (-1: no launch).
long cursor_game(ListScreen& s, Ctx& cx) {
  Transition t = s.handle(Action::A, cx);
  return t.kind == Transition::Launch ? long(t.game) : -1;
}

// ---- X opens the game options, Select too ----------------------------------------------------------------------------------------
void test_x_opens_game_options() {
  Ctx cx;
  make_ctx(cx, {"Tetris", "Zelda", "Metroid"});
  ListScreen s(0, cx);
  s.handle(Action::Down, cx);  // Zelda
  for (Action a : {Action::Select, Action::X}) {
    Transition t = s.handle(a, cx);
    CHECK(t.kind == Transition::Open);
    CHECK(t.name == "menu.game_options");
    CHECK(t.arg == "gb\t/roms/gb/Zelda.gb");
  }
  // an empty system has no game to open
  Ctx e;
  make_ctx(e, {});
  ListScreen empty(0, e);
  CHECK(empty.handle(Action::X, e).kind == Transition::None);
}

// ---- L2 / R2 jump to the previous / next letter (no wrap) -----------------------------------------------------------------------
void test_letter_jump() {
  Ctx cx;
  make_ctx(cx, {"Alpha", "Atom", "Bravo", "Cobra", "Cobalt"});
  ListScreen s(0, cx);
  CHECK_EQ(cursor_game(s, cx), 0L);  // Alpha
  s.handle(Action::R2, cx);
  CHECK_EQ(cursor_game(s, cx), 2L);  // Bravo
  s.handle(Action::R2, cx);
  CHECK_EQ(cursor_game(s, cx), 3L);  // Cobra
  s.handle(Action::R2, cx);          // last letter: stays
  CHECK_EQ(cursor_game(s, cx), 3L);
  s.handle(Action::L2, cx);          // at the start of C: back to the start of B
  CHECK_EQ(cursor_game(s, cx), 2L);
  s.handle(Action::L2, cx);          // at the start of B: back to the start of A
  CHECK_EQ(cursor_game(s, cx), 0L);
  s.handle(Action::L2, cx);          // first letter: stays
  CHECK_EQ(cursor_game(s, cx), 0L);
  s.handle(Action::Down, cx);        // Atom, in the middle of A
  s.handle(Action::L2, cx);          // the start of its letter
  CHECK_EQ(cursor_game(s, cx), 0L);
  s.handle(Action::Down, cx);
  s.handle(Action::Down, cx);        // Bravo (two rows down from Atom)
  CHECK_EQ(cursor_game(s, cx), 2L);
  s.handle(Action::L2, cx);          // Bravo starts its letter: A
  CHECK_EQ(cursor_game(s, cx), 0L);
}

// ---- Start opens the main menu, as on the carousel ------------------------------------------------------------------------------
void test_start_opens_menu() {
  Ctx cx;
  make_ctx(cx, {"Alpha", "Bravo"});
  ListScreen s(0, cx);
  Transition t = s.handle(Action::Start, cx);
  CHECK(t.kind == Transition::Open);
  CHECK(t.name == "menu.main");
}

// ---- RANDOM GAME in the game options picks another game than the cursor's --------------------------------------------------------
void test_random_game_entry() {
  Ctx cx;
  make_ctx(cx, {"Alpha", "Bravo", "Cobra", "Delta", "Echo"});
  ListScreen s(0, cx);
  long prev = cursor_game(s, cx);
  CHECK_EQ(prev, 0L);
  std::unique_ptr<Screen> opts = Registry::instance().make("menu.game_options", cx, mg::game_arg("gb", "/roms/gb/Alpha.gb"));
  Menu* m = static_cast<Menu*>(opts.get());
  m->rebuild(cx);
  int at = -1;
  for (size_t i = 0; i < m->size(); ++i)
    if (m->entry(i).label == "RANDOM GAME") at = int(i);
  CHECK(at >= 0);
  if (at < 0) return;
  for (int i = 0; i < 200; ++i) {
    // the entry closes the options (back to the list) and asks the list for a random game
    Transition back = m->entry(size_t(at)).on_a(cx);
    CHECK(back.kind == Transition::Pop);
    long now = cursor_game(s, cx);
    CHECK(now >= 0 && now < 5);
    CHECK(now != prev);  // never the same game twice in a row
    prev = now;
  }
  // a single game: nothing else to pick, the cursor stays
  Ctx one;
  make_ctx(one, {"Alpha"});
  ListScreen s1(0, one);
  request_random_game();
  CHECK_EQ(cursor_game(s1, one), 0L);
  CHECK_EQ(one.sv->toasts.items().size(), size_t(1));  // the key says there is no other game
  // and the options of a one-game system do not offer the entry
  std::unique_ptr<Screen> o1 = Registry::instance().make("menu.game_options", one, mg::game_arg("gb", "/roms/gb/Alpha.gb"));
  Menu* m1 = static_cast<Menu*>(o1.get());
  m1->rebuild(one);
  for (size_t i = 0; i < m1->size(); ++i) CHECK(m1->entry(i).label != "RANDOM GAME");
}

// ---- Y: quick search keeps the games whose name contains the text (no case); B clears it before leaving the list -------------
void test_quick_search() {
  Ctx cx;
  make_ctx(cx, {"Tetris", "Zelda", "Metroid", "Super Zelda"});
  ListScreen s(0, cx);
  Transition k = s.handle(Action::Y, cx);
  CHECK(k.kind == Transition::Push);  // the keyboard
  s.search(cx, "ZEL");                // case-insensitive
  CHECK_EQ(cursor_game(s, cx), 1L);   // Zelda, the first match
  s.handle(Action::Down, cx);
  CHECK_EQ(cursor_game(s, cx), 3L);   // Super Zelda: the index is the real game's, not the row's
  s.handle(Action::Up, cx);
  Transition o = s.handle(Action::Select, cx);
  CHECK(o.kind == Transition::Open);
  CHECK(o.arg == "gb\t/roms/gb/Zelda.gb");  // the options of the game under the cursor of the filtered list
  s.search(cx, "zzz");                // no match: an empty list, nothing launches
  CHECK_EQ(cursor_game(s, cx), -1L);
  CHECK(s.handle(Action::B, cx).kind == Transition::None);  // B clears the search and stays
  CHECK_EQ(cursor_game(s, cx), 0L);   // Tetris: the whole list again
  CHECK(s.handle(Action::B, cx).kind == Transition::Pop);   // without a search, B leaves the list
}

// ---- the keyboard of Y hands its text to the list: edited to empty and accepted, the next key shows the whole list again -------
void test_search_keyboard_path() {
  Ctx cx;
  make_ctx(cx, {"Tetris", "Zelda", "Metroid", "Super Zelda"});
  ListScreen s(0, cx);
  s.search(cx, "tet");
  Transition k = s.handle(Action::Y, cx);
  CHECK(k.kind == Transition::Push && k.screen != nullptr);
  if (!k.screen) return;
  for (int i = 0; i < 3; ++i) k.screen->handle(Action::B, cx);  // "tet" -> ""
  k.screen->handle(Action::Start, cx);                          // accept: the callback runs
  s.handle(Action::Down, cx);  // the search applies before this key: all four games, cursor on Zelda
  CHECK_EQ(cursor_game(s, cx), 1L);
}

// ---- a random request left by an earlier list's options does not move a new list -----------------------------------------------
void test_stale_random_request() {
  Ctx cx;
  make_ctx(cx, {"Alpha", "Bravo", "Cobra"});
  request_random_game();
  ListScreen fresh(0, cx);  // the list opened after the request: the request is gone
  CHECK_EQ(cursor_game(fresh, cx), 0L);
}

// ---- the game options link to the collections of the game (favourite and custom collections) --------------------------------
void test_game_options_link_collections() {
  Ctx cx;
  make_ctx(cx, {"Tetris", "Zelda"});
  std::unique_ptr<Screen> s = Registry::instance().make("menu.game_options", cx, mg::game_arg("gb", "/roms/gb/Zelda.gb"));
  CHECK(s != nullptr);
  Menu* m = static_cast<Menu*>(s.get());
  m->rebuild(cx);
  int at = -1;
  for (size_t i = 0; i < m->size(); ++i)
    if (m->entry(i).label == "COLLECTIONS") at = int(i);
  CHECK(at >= 0);
  if (at >= 0) {
    Transition t = m->entry(size_t(at)).on_a(cx);
    CHECK(t.kind == Transition::Open);
    CHECK(t.name == "menu.collections.game");
    CHECK(t.arg == "gb|/roms/gb/Zelda.gb");  // the argument of menu.collections.game: "<system>|<game path>"
  }
  // the system options have no game to put in a collection
  std::unique_ptr<Screen> sys = Registry::instance().make("menu.system_options", cx, "gb");
  Menu* sm = static_cast<Menu*>(sys.get());
  sm->rebuild(cx);
  bool any = false;
  for (size_t i = 0; i < sm->size(); ++i)
    if (sm->entry(i).label == "COLLECTIONS") any = true;
  CHECK(!any);
}

}  // namespace

int main() {
  test_x_opens_game_options();
  test_letter_jump();
  test_start_opens_menu();
  test_random_game_entry();
  test_quick_search();
  test_search_keyboard_path();
  test_stale_random_request();
  test_game_options_link_collections();
  return zt::done();
}
