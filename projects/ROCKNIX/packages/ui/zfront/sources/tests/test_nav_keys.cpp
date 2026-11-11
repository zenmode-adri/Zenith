// Navigation keys of ES that zfront lacked: left / right in a game list = previous / next system (QuickSystemSelect, default on;
// L / R keep paging), X on the carousel = a random system, Select = the QUICK ACCESS menu, MoveCarousel (CAROUSEL TRANSITIONS) off =
// the camera jumps, ShowHelpPrompts (ON-SCREEN HELP).
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "oscontract.hpp"
#include "screens.hpp"
#include "services.hpp"
#include "settings.hpp"
#include "status.hpp"
#include "theme.hpp"
#include "ui_prefs.hpp"
#include "util.hpp"

using namespace zf;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

// Three systems a, b, c with three games each, one carousel card per system unless `hide_b` (b is not on the carousel).
void make_ctx(Ctx& cx, bool hide_b = false, const std::string& es_xml = "<config/>") {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = true;
  cx.es = EsSettings::from_text(es_xml, "/nonexistent/es_settings.cfg");
  cx.conf = SystemConf::from_text("", "/nonexistent/system.cfg");
  for (const char* name : {"a", "b", "c"}) {
    System s;
    s.name = name;
    s.fullname = name;
    s.theme = name;
    s.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR%";
    s.emulator = "retroarch";
    s.core = "gambatte";
    s.emus = {{"retroarch", {"gambatte"}}};
    s.rom_dir = std::string("/roms/") + name;
    s.gamelist = s.rom_dir + "/gamelist.xml";
    for (const char* g : {"One", "Two", "Three"}) {
      Game game;
      game.name = g;
      game.path = s.rom_dir + "/" + g + ".gb";
      s.games.push_back(std::move(game));
    }
    s.count = s.games.size();
    s.loaded = true;
    cx.systems.push_back(std::move(s));
  }
  for (size_t i = 0; i < cx.systems.size(); ++i) {
    if (hide_b && i == 1) continue;
    SystemItem it;
    it.theme_name = cx.systems[i].theme;
    it.full_name = cx.systems[i].fullname;
    it.games = uint32_t(cx.systems[i].count);
    it.sys = i;
    cx.items.push_back(it);
  }
}

// The system a Replace transition's list shows (as a carousel card), -1 when it is not a replacement.
long replaced_with(Transition& t, Ctx& cx) { return t.kind == Transition::Replace && t.screen ? t.screen->shown_item(cx) : -1; }

void test_list_left_right_change_system() {
  Ctx cx;
  make_ctx(cx);
  ListScreen s(0, cx);
  Transition t = s.handle(Action::Right, cx);
  CHECK_EQ(replaced_with(t, cx), 1L);
  t = s.handle(Action::Left, cx);  // wraps to the last system
  CHECK_EQ(replaced_with(t, cx), 2L);
  ListScreen last(2, cx);
  t = last.handle(Action::Right, cx);  // and back to the first
  CHECK_EQ(replaced_with(t, cx), 0L);
  // L / R still page (Left / Right did before): the cursor goes to the last of three games, no transition
  t = s.handle(Action::R, cx);
  CHECK(t.kind == Transition::None);
  CHECK_EQ(s.launch_target(cx).find("Three.gb") != std::string::npos, true);
}

void test_list_hidden_system_is_skipped() {
  Ctx cx;
  make_ctx(cx, true);  // the carousel shows a and c
  ListScreen s(0, cx);
  Transition t = s.handle(Action::Right, cx);
  CHECK(t.kind == Transition::Replace);
  CHECK_EQ(replaced_with(t, cx), 1L);  // card 1 = system c
  ListScreen c(2, cx);
  CHECK_EQ(c.shown_item(cx), 1L);
  Transition u = c.handle(Action::Left, cx);
  CHECK_EQ(replaced_with(u, cx), 0L);
}

void test_quick_system_select_off_pages() {
  Ctx cx;
  make_ctx(cx, false, "<bool name=\"QuickSystemSelect\" value=\"false\" />");
  ListScreen s(0, cx);
  Transition t = s.handle(Action::Right, cx);
  CHECK(t.kind == Transition::None);
  CHECK(s.launch_target(cx).find("Three.gb") != std::string::npos);  // paged to the end
}

size_t count_events(const char* what) {
  size_t n = 0;
  for (const std::string& l : oscontract::dry_log())
    if (l.rfind(what, 0) == 0) ++n;
  return n;
}

// Back on the carousel after Right in the list: it is on the new system (announced again).
void test_carousel_follows_the_list() {
  oscontract::startup(std::make_shared<Services>(), true);
  oscontract::dry_log().clear();
  Ctx cx;
  make_ctx(cx);
  App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
  app.tick(0.0f);
  app.handle(Action::A);      // list of a
  app.handle(Action::Right);  // list of b
  app.handle(Action::B);
  app.tick(0.0f);
  CHECK_EQ(count_events("event system-selected b"), 1ul);
  oscontract::reset_for_test();
}

void test_carousel_x_is_random_system() {
  Ctx cx;
  make_ctx(cx);
  CarouselScreen c(1);
  std::set<long> seen;
  for (int i = 0; i < 40; ++i) {
    const long before = c.system_cursor(cx);
    Transition t = c.handle(Action::X, cx);
    CHECK(t.kind == Transition::None);
    const long after = c.system_cursor(cx);
    CHECK(after != before);  // another system each time
    CHECK(after >= 0 && after < 3);
    seen.insert(after);
  }
  CHECK_EQ(seen.size(), 3ul);
  // one system: nothing to pick
  Ctx one;
  make_ctx(one);
  one.items.resize(1);
  CarouselScreen c1(0);
  c1.handle(Action::X, one);
  CHECK_EQ(c1.system_cursor(one), 0L);
}

void test_carousel_select_opens_quick_access() {
  Ctx cx;
  make_ctx(cx);
  CarouselScreen c(0);
  Transition t = c.handle(Action::Select, cx);
  CHECK(t.kind == Transition::Open);
  CHECK_EQ(t.name, std::string("menu.quit"));
  CHECK_EQ(t.arg, std::string("quick"));
}

void test_move_carousel_off_jumps() {
  Ctx on;
  make_ctx(on);
  CarouselScreen a(0);
  a.handle(Action::Right, on);
  a.tick(0.016f, on);
  CHECK(a.camera() > 0.0f && a.camera() < 1.0f);  // gliding
  Ctx off;
  make_ctx(off, false, "<bool name=\"MoveCarousel\" value=\"false\" />");
  CarouselScreen b(0);
  b.handle(Action::Right, off);
  b.tick(0.016f, off);
  CHECK_EQ(b.camera(), 1.0f);  // jumped
}

void test_help_prompts_flag() {
  Ctx cx;
  make_ctx(cx, false, "<bool name=\"ShowHelpPrompts\" value=\"false\" />");
  ui_prefs::reapply(cx);
  CHECK(!ui_prefs::help_prompts());
  cx.es.set_bool("ShowHelpPrompts", true);
  ui_prefs::reapply(cx);
  CHECK(ui_prefs::help_prompts());
  CHECK(ui_prefs::read(EsSettings()).help);  // ES default: on
}

}  // namespace

int main() {
  test_list_left_right_change_system();
  test_list_hidden_system_is_skipped();
  test_quick_system_select_off_pages();
  test_carousel_follows_the_list();
  test_carousel_x_is_random_system();
  test_carousel_select_opens_quick_access();
  test_move_carousel_off_jumps();
  test_help_prompts_flag();
  return zt::done();
}
