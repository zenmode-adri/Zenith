// `menu.files` (menu_files.cpp) driven with the keys on a temp folder: browsing, going up, marking, the actions list (copy + paste into another
// folder through the job lane), delete with its YES/NO question (NO first), B at the top closes, nothing is offered outside the roots,
// and the Tools entry match (`fileman.sh` / `commander.sh` open the screen, a look-alike does not).
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "app.hpp"
#include "check.hpp"
#include "menu_files.hpp"
#include "registry.hpp"

using namespace zf;

namespace {

std::string g_root;
bool has(const std::string& rel) {
  struct stat st;
  return ::lstat((g_root + "/" + rel).c_str(), &st) == 0;
}

void settle(Ctx& cx, Screen& s) {
  for (int i = 0; i < 2000; ++i) {
    cx.sv->jobs.drain();
    if (cx.sv->jobs.idle()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  cx.sv->jobs.drain();
  s.tick(0.0f, cx);  // sees the job end and reads the folder again
}

std::string dbg(Ctx& cx, Screen& s, const char* what) { return files_debug_for_test(s, cx, what); }
bool ends_with(const std::string& a, const std::string& b) { return a.size() >= b.size() && a.compare(a.size() - b.size(), b.size(), b) == 0; }
// moves the cursor (Down wraps) until it is on the entry whose path ends with `suffix`
void to_entry(Ctx& cx, Screen& s, const std::string& suffix) {
  for (int i = 0; i < 40 && !ends_with(dbg(cx, s, "cursor"), suffix); ++i) s.handle(Action::Down, cx);
  CHECK(ends_with(dbg(cx, s, "cursor"), suffix));
}
// answers the dialog `t` with YES
Transition yes(Ctx& cx, Transition& t) {
  CHECK((t.kind == Transition::Push || t.kind == Transition::Replace) && t.screen != nullptr);
  if (!t.screen) return Transition::none();
  t.screen->handle(Action::Left, cx);
  return t.screen->handle(Action::A, cx);
}

// X, then the n-th entry of the list that opens (0 = first); returns what the screen then wants to show (a dialog), if anything
Transition action(Ctx& cx, Screen& s, int nth) {
  Transition t = s.handle(Action::X, cx);
  CHECK(t.kind == Transition::Push && t.screen != nullptr);
  if (!t.screen) return Transition::none();
  for (int i = 0; i < nth; ++i) t.screen->handle(Action::Down, cx);
  t.screen->handle(Action::A, cx);  // the pick is stored; the screen acts on it in take_transition
  return s.take_transition(cx);
}

}  // namespace

int main() {
  CHECK(is_file_manager_script("/storage/.config/modules/fileman.sh"));
  CHECK(is_file_manager_script("./commander.sh"));
  CHECK(is_file_manager_script("fileman.sh"));
  CHECK(!is_file_manager_script("/storage/.config/modules/myfileman.sh"));
  CHECK(!is_file_manager_script("/storage/.config/modules/fileman.sh.bak"));
  CHECK(!is_file_manager_script("/storage/roms/tools/Fileman.sh"));
  CHECK(!is_file_manager_script(""));

  char tpl[] = "/tmp/zf-files-XXXXXX";
  if (!::mkdtemp(tpl)) return 1;
  g_root = tpl;
  ::mkdir((g_root + "/a").c_str(), 0755);
  ::mkdir((g_root + "/b").c_str(), 0755);
  { std::ofstream(g_root + "/a/file1.txt") << "one"; }
  { std::ofstream(g_root + "/x.txt") << "x"; }
  ::setenv("ZF_FILES_ROOT", g_root.c_str(), 1);

  Ctx cx;
  cx.w = 640, cx.h = 480;
  auto s = Registry::instance().make("menu.files", cx, "");
  CHECK(s != nullptr);
  if (!s) return 1;
  s->tick(0.0f, cx);
  CHECK(s->is_menu());

  // marks "a", moves to "b"; the first action is Copy, and nothing leaves the screen yet
  s->handle(Action::Y, cx);
  Transition t = action(cx, *s, 0);
  CHECK(t.kind == Transition::None);

  // into "b" (cursor is on it), paste: Paste here comes first when there is nothing under the cursor but "..": go up-row
  s->handle(Action::A, cx);
  t = action(cx, *s, 0);  // on ".." there are no targets: the list starts with "Paste here"
  CHECK(t.kind == Transition::None);
  settle(cx, *s);
  CHECK(has("b/a/file1.txt"));
  CHECK(has("a/file1.txt"));  // a copy, not a move

  // back to the top and delete x.txt: NO is the default, so the file stays; then YES removes it
  s->handle(Action::Left, cx);  // up; the cursor is on "b"
  s->handle(Action::Down, cx);  // x.txt
  t = action(cx, *s, 5);        // Copy, Move, Paste here, Rename, New folder, Delete: Delete is the sixth
  if (t.kind == Transition::Push && t.screen) {
    t.screen->handle(Action::A, cx);  // NO
    settle(cx, *s);
    CHECK(has("x.txt"));
  }
  t = action(cx, *s, 5);
  CHECK(t.kind == Transition::Push);
  if (t.kind == Transition::Push && t.screen) {
    t.screen->handle(Action::Left, cx);
    Transition after = t.screen->handle(Action::A, cx);  // YES
    (void)after;
    settle(cx, *s);
    CHECK(!has("x.txt"));
  }

  // at the top B closes the screen; the roots are not left
  Transition b = s->handle(Action::B, cx);
  CHECK(b.kind == Transition::Pop);

  // ---- more of the screen: the question names what will go, a failed paste keeps the clipboard, an unreadable folder is not an empty one,
  // sort and hidden are remembered, and the system files ask twice
  {
    { std::ofstream(g_root + "/m1.txt") << "m1"; }
    { std::ofstream(g_root + "/m2.txt") << "m2"; }
    ::mkdir((g_root + "/e").c_str(), 0755);
    ::mkdir((g_root + "/.config").c_str(), 0755);
    { std::ofstream(g_root + "/.config/set.cfg") << "c"; }
    ::mkdir((g_root + "/roms").c_str(), 0755);
    ::mkdir((g_root + "/roms/gb").c_str(), 0755);
    { std::ofstream(g_root + "/roms/gb/game.gb") << "g"; }

    // "Properties" of a folder shows its real date (the listing does not read it)
    {
      auto sp = Registry::instance().make("menu.files", cx, "");
      sp->tick(0.0f, cx);
      to_entry(cx, *sp, "/e");
      const std::string props = dbg(cx, *sp, "props");
      CHECK(!props.empty() && props.find("1970") == std::string::npos);
    }

    // the folder of the roms (a system) is protected: asked, then refused
    {
      auto sr = Registry::instance().make("menu.files", cx, "");
      sr->tick(0.0f, cx);
      to_entry(cx, *sr, "/roms");
      Transition tr0 = action(cx, *sr, 5);  // Delete (the clip is not empty: Copy, Move, Paste here, Rename, New folder, Delete)
      yes(cx, tr0);
      settle(cx, *sr);
      CHECK(has("roms/gb/game.gb"));
    }

    // the delete question names the MARKED item, not the row under the cursor (Y marks and moves down)
    auto s2 = Registry::instance().make("menu.files", cx, "");
    s2->tick(0.0f, cx);
    to_entry(cx, *s2, "/a");
    s2->handle(Action::Y, cx);  // marks "a"; the cursor is now on the next row
    CHECK(!ends_with(dbg(cx, *s2, "cursor"), "/a"));
    t = action(cx, *s2, 4);  // clip is not empty (an earlier copy): Copy, Move, Paste here, New folder, Delete
    CHECK(t.kind == Transition::Push);
    CHECK(dbg(cx, *s2, "confirm").find("\"a\"") == 0);
    CHECK(dbg(cx, *s2, "confirm").find("\"b\"") == std::string::npos);
    if (t.screen) t.screen->handle(Action::A, cx);  // NO
    settle(cx, *s2);
    CHECK(has("a/file1.txt"));
    s2->handle(Action::Y, cx);  // a second mark
    t = action(cx, *s2, 4);
    CHECK(dbg(cx, *s2, "confirm").find("2 items") == 0);
    if (t.screen) t.screen->handle(Action::A, cx);

    // a paste that is refused (another job is running) does not lose what was cut
    auto s3 = Registry::instance().make("menu.files", cx, "");
    s3->tick(0.0f, cx);
    to_entry(cx, *s3, "/a");
    s3->handle(Action::Y, cx);
    t = action(cx, *s3, 1);  // Move
    CHECK(t.kind == Transition::None && dbg(cx, *s3, "clip") == "1");
    to_entry(cx, *s3, "/b");
    s3->handle(Action::A, cx);  // into "b"
    dbg(cx, *s3, "busy:1");
    t = action(cx, *s3, 0);  // on "..": Paste here is the first entry
    CHECK(dbg(cx, *s3, "clip") == "1");
    CHECK(has("a/file1.txt"));
    dbg(cx, *s3, "busy:0");
    t = action(cx, *s3, 0);
    settle(cx, *s3);
    CHECK(has("b/a (2)/file1.txt") && !has("a"));
    CHECK(dbg(cx, *s3, "clip") == "0");

    // a folder that cannot be read says so; an empty one says it is empty
    auto s4 = Registry::instance().make("menu.files", cx, "");
    s4->tick(0.0f, cx);
    to_entry(cx, *s4, "/b");
    s4->handle(Action::A, cx);
    CHECK(dbg(cx, *s4, "state") == "ok");
    {
      const std::string rmb = "rm -rf '" + g_root + "/b'";
      if (std::system(rmb.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
    }
    dbg(cx, *s4, "refill");
    CHECK(dbg(cx, *s4, "state") == "unreadable");
    s4->handle(Action::B, cx);  // up to the top: readable again
    CHECK(dbg(cx, *s4, "state") == "ok");
    to_entry(cx, *s4, "/e");
    s4->handle(Action::A, cx);
    CHECK(dbg(cx, *s4, "state") == "empty");

    // sort and "show hidden" are kept for the next time
    const std::string cfg = g_root + "/system.cfg";
    { std::ofstream(cfg) << ""; }
    SystemConf::lock_path() = g_root + "/conf.lock";
    cx.conf = SystemConf::load(cfg);
    auto s5 = Registry::instance().make("menu.files", cx, "");
    s5->tick(0.0f, cx);
    CHECK(dbg(cx, *s5, "sort") == "name" && dbg(cx, *s5, "hidden") == "0");
    to_entry(cx, *s5, "/e");
    t = action(cx, *s5, 7);  // no clip now: Copy, Move, Rename, New folder, Delete, Properties, Select all, Sort by
    CHECK(dbg(cx, *s5, "sort") == "size");
    t = action(cx, *s5, 8);  // Show hidden files
    CHECK(dbg(cx, *s5, "hidden") == "1");
    CHECK_EQ(cx.conf.get_or("zfront.files.sort", ""), std::string("size"));
    CHECK_EQ(cx.conf.get_or("zfront.files.hidden", ""), std::string("1"));
    auto s6 = Registry::instance().make("menu.files", cx, "");
    CHECK(dbg(cx, *s6, "sort") == "size" && dbg(cx, *s6, "hidden") == "1");

    // system files (.config) ask a second time to delete, and once to rename; the folder of a system under roms stays, its content does not
    to_entry(cx, *s6, "/.config");
    s6->handle(Action::A, cx);
    to_entry(cx, *s6, "/.config/set.cfg");
    t = action(cx, *s6, 4);  // Delete
    Transition second = yes(cx, t);
    CHECK(second.kind == Transition::Replace && second.screen != nullptr);
    CHECK(has(".config/set.cfg"));  // one YES is not enough
    CHECK(dbg(cx, *s6, "confirm").find("system file") != std::string::npos);
    if (second.screen) {
      second.screen->handle(Action::A, cx);  // NO again
      settle(cx, *s6);
      CHECK(has(".config/set.cfg"));
    }
    t = action(cx, *s6, 4);
    second = yes(cx, t);
    if (second.screen) yes(cx, second);
    settle(cx, *s6);
    CHECK(!has(".config/set.cfg"));
    { std::ofstream(g_root + "/.config/two.cfg") << "2"; }
    s6->handle(Action::B, cx);
    to_entry(cx, *s6, "/.config");
    s6->handle(Action::A, cx);
    settle(cx, *s6);
    to_entry(cx, *s6, "/.config/two.cfg");
    t = action(cx, *s6, 2);  // Rename: the warning comes before the keyboard
    CHECK(dbg(cx, *s6, "confirm").find("system file") != std::string::npos);
    Transition kb = yes(cx, t);
    CHECK(kb.kind == Transition::Replace && kb.screen != nullptr);
    CHECK(has(".config/two.cfg"));

    s6->handle(Action::B, cx);
    to_entry(cx, *s6, "/roms");
    s6->handle(Action::A, cx);
    to_entry(cx, *s6, "/roms/gb");
    t = action(cx, *s6, 4);  // Delete the system's folder: asked, then refused
    yes(cx, t);
    settle(cx, *s6);
    CHECK(has("roms/gb/game.gb"));
    s6->handle(Action::A, cx);  // into it
    to_entry(cx, *s6, "/roms/gb/game.gb");
    t = action(cx, *s6, 4);
    yes(cx, t);
    settle(cx, *s6);
    CHECK(!has("roms/gb/game.gb"));  // what is inside does go
  }

  const std::string cmd = "rm -rf '" + g_root + "'";
  if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
  return zt::done();
}
