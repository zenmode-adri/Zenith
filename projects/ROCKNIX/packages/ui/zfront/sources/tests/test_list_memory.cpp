// The game list keeps where the cursor was when it is left and puts it back (ES does, per system), L / R page by the rows drawn, the
// "continue playing" tile finds its ROM without loading the gamelist, picture lookups are asked once and bounded, the XMB home never draws a
// system twice. Nothing here needs a font or a theme folder.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "gamemeta.hpp"
#include "remake.hpp"
#include "screens.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

// One system "gb" with the given games; nothing is written to disk.
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

void test_cursor_memory() {
  Ctx cx;
  make_ctx(cx, {"Alpha", "Bravo", "Charlie", "Delta"});
  {
    ListScreen s(0, cx);
    CHECK_EQ(s.cursor_path(), std::string("/roms/gb/Alpha.gb"));
    s.handle(Action::Down, cx);
    s.handle(Action::Down, cx);
    CHECK_EQ(s.cursor_path(), std::string("/roms/gb/Charlie.gb"));
    // a list opened on top of this one while it is still alive (a test, or the options) does not see a cursor that was not left yet
    ListScreen other(0, cx);
    CHECK_EQ(other.cursor_path(), std::string("/roms/gb/Alpha.gb"));
    CHECK(s.handle(Action::B, cx).kind == Transition::Pop);  // leaving the list: the cursor is kept
  }
  {
    ListScreen again(0, cx);  // the same system again: on the game it was left on
    CHECK_EQ(again.cursor_path(), std::string("/roms/gb/Charlie.gb"));
    again.handle(Action::Down, cx);
    CHECK(again.handle(Action::B, cx).kind == Transition::Pop);
  }
  {
    ListScreen again(0, cx);
    CHECK_EQ(again.cursor_path(), std::string("/roms/gb/Delta.gb"));
    CHECK(again.handle(Action::B, cx).kind == Transition::Pop);
  }
  cx.systems[0].games.erase(cx.systems[0].games.begin() + 3);  // the game it was left on is gone: the first row
  cx.systems[0].count = cx.systems[0].games.size();
  {
    ListScreen gone(0, cx);
    CHECK_EQ(gone.cursor_path(), std::string("/roms/gb/Alpha.gb"));
  }
  // another Ctx (another run of the frontend) knows nothing of it
  Ctx fresh;
  make_ctx(fresh, {"Alpha", "Bravo", "Charlie", "Delta"});
  ListScreen s2(0, fresh);
  CHECK_EQ(s2.cursor_path(), std::string("/roms/gb/Alpha.gb"));
}

void test_paging() {
  CHECK_EQ(list_page_rows(8, 0), size_t(8));    // before the first draw: what the theme says
  CHECK_EQ(list_page_rows(8, 19), size_t(19));  // after it: the rows on screen
  CHECK_EQ(list_page_rows(0, 0), size_t(1));    // never a page of nothing
}

void test_rom_in_folder() {
  char tmpl[] = "/tmp/zf-rom-XXXXXX";
  const std::string root = mkdtemp(tmpl);
  std::ofstream(root + "/Tetris.GB") << "x";
  std::ofstream(root + "/Zelda.gb") << "x";
  ::mkdir((root + "/sub").c_str(), 0755);
  std::ofstream(root + "/sub/Hidden.gb") << "x";
  System sys;
  sys.name = "gb";
  sys.rom_dir = root;
  sys.exts = {".gb"};  // upper case ".GB" is not listed
  CHECK(rom_in_folder(sys, "Zelda"));
  CHECK(rom_in_folder(sys, "Tetris"));  // found without loading anything: the upper-case extension
  CHECK(!rom_in_folder(sys, "Missing"));
  CHECK(!rom_in_folder(sys, "Hidden"));  // a sub folder is the gamelist's business
  CHECK(!sys.loaded);
  System none;
  none.rom_dir = root + "/nowhere";
  none.exts = {".gb"};
  CHECK(!rom_in_folder(none, "Zelda"));
  std::string cmd = "rm -rf " + root;
  if (std::system(cmd.c_str()) != 0) {
  }
}

void test_exists_cache() {
  char tmpl[] = "/tmp/zf-exists-XXXXXX";
  const std::string root = mkdtemp(tmpl);
  const std::string p = root + "/logo.webp";
  CHECK(!remake::exists_cached(p));
  std::ofstream(p) << "x";
  CHECK(!remake::exists_cached(p));  // asked once: the answer is kept, no stat per frame
  games_changed();                   // a scrape or an edit: the pictures may have appeared
  CHECK(remake::exists_cached(p));
  // never more than kExistsCache paths (it used to stop remembering at 4096 and stat again for ever)
  for (int i = 0; i < int(remake::kExistsCache) + 500; ++i) remake::exists_cached(root + "/none" + std::to_string(i));
  CHECK(remake::exists_cache_size() <= remake::kExistsCache);
  CHECK(remake::exists_cache_size() > 0);
  const std::string q = root + "/late.png";
  CHECK(!remake::exists_cached(q));
  std::ofstream(q) << "x";
  games_changed();
  CHECK(remake::exists_cached(q));
  std::string cmd = "rm -rf " + root;
  if (std::system(cmd.c_str()) != 0) {
  }
}

void test_xmb_span() {
  for (int n = 1; n <= 14; ++n)
    for (int mid : {0, 3, -2, 100}) {
      int first = 0, last = 0;
      remake::xmb_span(n, mid, first, last);
      const int count = last - first + 1;
      CHECK(count >= 1 && count <= n);  // never more icons than systems
      std::set<int> seen;
      for (int i = first; i <= last; ++i) seen.insert(((i % n) + n) % n);
      CHECK_EQ(int(seen.size()), count);  // and none of them twice
      CHECK(first <= mid && last >= mid);  // the chosen one is always there
    }
  int first = 0, last = 0;
  remake::xmb_span(30, 5, first, last);  // many systems: as always, one before and the rest after
  CHECK_EQ(first, 4);
  CHECK_EQ(last, 14);
  remake::xmb_span(3, 0, first, last);
  CHECK_EQ(last - first + 1, 3);
}

}  // namespace

int main() {
  test_cursor_memory();
  test_paging();
  test_rom_in_folder();
  test_exists_cache();
  test_xmb_span();
  return zt::done();
}
