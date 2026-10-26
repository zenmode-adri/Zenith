// Menu framework: navigation, widgets, dialogs, on-screen keyboard, async refresh, OSD. The pure logic needs no font;
// the drawing checks run only when the Zenith theme folder is next to the checkout.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "i18n.hpp"
#include "check.hpp"
#include "alloc_nothrow.hpp"
#include "menu.hpp"
#include "registry.hpp"
#include "status.hpp"
#include "ui_dialogs.hpp"
#include "ui_osd.hpp"
#include "util.hpp"

using namespace zf;

// Allocation counter (this test binary only): the steady-state frame of a menu must not allocate.
static thread_local size_t g_allocs = 0;
void* operator new(size_t n) {
  ++g_allocs;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

struct Quiet : Screen {
  Transition handle(Action, Ctx&) override { return Transition::none(); }
  void draw(Ctx&, std::vector<Op>&) override {}
};

// Geometry of the 4:3 layout of the Zenith theme (layout.xml), enough for Metrics.
const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

void make_ctx(Ctx& cx, float w = 640, float h = 480) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = w;
  cx.h = h;
  cx.sv->cmd.dry_run = true;
}

Transition press(Screen& s, Ctx& cx, Action a) { return s.handle(a, cx); }

// a list long enough to scroll, with the usual mixture of kinds
struct Fixture {
  bool flag = false;
  int vol = 50;
  std::string pick = "b";
  int flag_calls = 0, vol_calls = 0, commit_calls = 0, last_commit = -1, action_calls = 0, pick_calls = 0;
  std::string typed;
};

std::unique_ptr<Menu> make(std::shared_ptr<Fixture> f, bool slow_choice = false) {
  MenuSpec s;
  s.title = "TEST";
  s.build = [f, slow_choice](MenuBuilder& m, Ctx&) {
    m.header("HEAD");                                                               // 0 (skipped)
    m.action("ACT", [f] { ++f->action_calls; });                                     // 1
    m.toggle("TOG", [f] { return f->flag; }, [f](bool b) { f->flag = b, ++f->flag_calls; });  // 2
    m.separator();                                                                   // 3 (skipped)
    m.slider("VOL", 0, 100, 25, [f] { return f->vol; }, [f](int v) { f->vol = v, ++f->vol_calls; })  // 4
        .unit("%")
        .commit([f](int v) { ++f->commit_calls, f->last_commit = v; });
    Entry& c = m.choice("CHO", {{"a", "A"}, {"b", "B"}, {"c", "C"}}, [f] { return f->pick; }, [f](const std::string& v) { f->pick = v, ++f->pick_calls; });  // 5
    if (slow_choice) c.no_cycle();
    m.info("INFO", "x");                                                             // 6 (skipped)
    m.action("OFF", [] {}).disabled();                                               // 7 (skipped)
    m.text("TXT", [f] { return f->typed; }, [f](const std::string& v) { f->typed = v; });  // 8
    m.action("DANGER", [f] { ++f->action_calls; }).danger().confirm("SURE?");        // 9
  };
  return std::make_unique<Menu>(std::move(s));
}

}  // namespace

ZF_REGISTER_SCREEN("test.menu.exists", [](Ctx&, const std::string&) { return std::make_unique<Quiet>(); });

int main() {
  // ---- navigation: header / separator / info / disabled are skipped; wraps; first selectable at the start
  {
    Ctx cx;
    make_ctx(cx);
    auto f = std::make_shared<Fixture>();
    auto m = make(f);
    press(*m, cx, Action::Left);  // first handle builds the menu (left on an action does nothing)
    CHECK_EQ(m->size(), size_t(10));
    CHECK_EQ(m->cursor(), 1);
    press(*m, cx, Action::Down);
    CHECK_EQ(m->cursor(), 2);
    press(*m, cx, Action::Down);
    CHECK_EQ(m->cursor(), 4);  // separator skipped
    press(*m, cx, Action::Down);
    CHECK_EQ(m->cursor(), 5);
    press(*m, cx, Action::Down);
    CHECK_EQ(m->cursor(), 8);  // info and disabled skipped
    press(*m, cx, Action::Down);
    CHECK_EQ(m->cursor(), 9);
    press(*m, cx, Action::Down);
    CHECK_EQ(m->cursor(), 1);  // wraps
    press(*m, cx, Action::Up);
    CHECK_EQ(m->cursor(), 9);
    // pages stop at the ends (no wrap)
    press(*m, cx, Action::L);
    CHECK_EQ(m->cursor(), 1);
    press(*m, cx, Action::R);
    CHECK_EQ(m->cursor(), 9);
    CHECK(m->visible_rows() >= 3);
    // B leaves
    CHECK(press(*m, cx, Action::B).kind == Transition::Pop);
  }

  // ---- title: set_title on an open menu; a title_key follows the interface language and the entries are rebuilt
  {
    Ctx cx;
    make_ctx(cx);
    int builds = 0;
    MenuSpec s;
    s.title = tr("YES");
    s.title_key = "YES";
    s.build = [&builds](MenuBuilder& m, Ctx&) {
      ++builds;
      m.action(tr("NO"), [] {});
    };
    Menu m(std::move(s));
    press(m, cx, Action::Left);  // builds
    CHECK_EQ(builds, 1);
    m.set_title("Renamed");
    CHECK_EQ(m.title(), std::string("Renamed"));
    // back to a keyed title, then switch the language under the open menu
    MenuSpec s2;
    s2.title = tr("YES");
    s2.title_key = "YES";
    s2.build = [&builds](MenuBuilder& m2, Ctx&) {
      ++builds;
      m2.action(tr("NO"), [] {});
    };
    Menu m2(std::move(s2));
    press(m2, cx, Action::Left);
    int b0 = builds;
    i18n::Options o;
    o.dirs = {std::string(ZF_TEST_DATA) + "/i18n"};
    o.domains = {"zfront"};
    CHECK(i18n::set_language("es_ES", o));
    press(m2, cx, Action::Left);  // the next interaction notices the new language
    CHECK_EQ(m2.title(), std::string(tr("YES")));
    CHECK(m2.title() != "Yes" && m2.title() != "YES");
    CHECK(builds > b0);
    CHECK_EQ(m.title(), std::string("Renamed"));  // a title set by hand is not replaced
    CHECK(i18n::set_language("en_US", o));
  }

  // ---- toggle: left/right set, A flips, the setter only runs on a real change, getters are re-read
  {
    Ctx cx;
    make_ctx(cx);
    auto f = std::make_shared<Fixture>();
    auto m = make(f);
    press(*m, cx, Action::Down);  // TOG
    press(*m, cx, Action::Left);
    CHECK_EQ(f->flag_calls, 0);  // already off
    press(*m, cx, Action::Right);
    CHECK(f->flag);
    CHECK_EQ(f->flag_calls, 1);
    press(*m, cx, Action::Right);
    CHECK_EQ(f->flag_calls, 1);
    press(*m, cx, Action::A);
    CHECK(!f->flag);
    f->flag = true;  // changed behind our back, e.g. by a script: the next interaction starts from the real value
    press(*m, cx, Action::Right);
    m->tick(0.0f, cx);
    press(*m, cx, Action::A);
    CHECK(!f->flag);
  }

  // ---- slider: step, limits, setter only when the value changes, debounced commit
  {
    Ctx cx;
    make_ctx(cx);
    auto f = std::make_shared<Fixture>();
    auto m = make(f);
    press(*m, cx, Action::Down);
    press(*m, cx, Action::Down);  // VOL (cursor 4)
    CHECK_EQ(m->cursor(), 4);
    press(*m, cx, Action::Right);
    CHECK_EQ(f->vol, 75);
    press(*m, cx, Action::Right);
    CHECK_EQ(f->vol, 100);
    press(*m, cx, Action::Right);  // at the limit
    CHECK_EQ(f->vol, 100);
    CHECK_EQ(f->vol_calls, 2);
    CHECK_EQ(m->entry(4).value_text, std::string("100%"));
    // commit runs once, 0.5 s after the last change, not before
    CHECK_EQ(f->commit_calls, 0);
    CHECK(m->wake_in().has_value());
    m->tick(0.3f, cx);
    CHECK_EQ(f->commit_calls, 0);
    m->tick(0.3f, cx);
    CHECK_EQ(f->commit_calls, 1);
    CHECK_EQ(f->last_commit, 100);
    CHECK(!m->wake_in().has_value());
    for (int i = 0; i < 8; ++i) press(*m, cx, Action::Left);
    CHECK_EQ(f->vol, 0);
    CHECK_EQ(m->entry(4).value_text, std::string("0%"));
    // leaving flushes a pending commit
    press(*m, cx, Action::B);
    CHECK_EQ(f->commit_calls, 2);
    CHECK_EQ(f->last_commit, 0);
  }

  // ---- choice: cycles with left/right (wraps), A opens a popup that picks and refreshes the row
  {
    Ctx cx;
    make_ctx(cx);
    auto f = std::make_shared<Fixture>();
    auto m = make(f);
    for (int i = 0; i < 3; ++i) press(*m, cx, Action::Down);  // CHO
    CHECK_EQ(m->cursor(), 5);
    CHECK_EQ(m->entry(5).value_text, std::string("B"));
    press(*m, cx, Action::Right);
    CHECK_EQ(f->pick, std::string("c"));
    press(*m, cx, Action::Right);
    CHECK_EQ(f->pick, std::string("a"));  // wrapped
    press(*m, cx, Action::Left);
    CHECK_EQ(f->pick, std::string("c"));
    CHECK_EQ(m->entry(5).value_text, std::string("C"));
    Transition t = press(*m, cx, Action::A);  // popup, focus on the current value
    CHECK(t.kind == Transition::Push && t.screen);
    t.screen->handle(Action::Down, cx);  // c -> a
    t.screen->handle(Action::Down, cx);  // a -> b
    Transition r = t.screen->handle(Action::A, cx);
    CHECK(r.kind == Transition::Pop);
    CHECK_EQ(f->pick, std::string("b"));
    m->tick(0.0f, cx);  // the menu notices the change
    CHECK_EQ(m->entry(5).value_text, std::string("B"));
    // B cancels without calling the setter
    int calls = f->pick_calls;
    Transition t2 = press(*m, cx, Action::A);
    CHECK(t2.screen->handle(Action::B, cx).kind == Transition::Pop);
    CHECK_EQ(f->pick_calls, calls);
    // no_cycle: left/right do nothing
    auto f2 = std::make_shared<Fixture>();
    auto m2 = make(f2, true);
    for (int i = 0; i < 3; ++i) press(*m2, cx, Action::Down);
    press(*m2, cx, Action::Right);
    CHECK_EQ(f2->pick_calls, 0);
  }

  // ---- submenu: a screen that is not registered is left out (never drawn, never focused); a registered one opens
  {
    Ctx cx;
    make_ctx(cx);
    MenuSpec s;
    s.title = "SUB";
    s.build = [](MenuBuilder& m, Ctx&) {
      m.submenu("MISSING", "test.menu.missing");
      m.submenu("PRESENT", "test.menu.exists", "arg");
      m.submenu("ALSO MISSING", "test.menu.missing2");
    };
    Menu m(s);
    press(m, cx, Action::Left);
    CHECK_EQ(m.size(), size_t(1));
    CHECK_EQ(m.entry(0).label, std::string("PRESENT"));
    CHECK(m.entry(0).enabled);
    CHECK_EQ(m.cursor(), 0);
    Transition t = press(m, cx, Action::A);
    CHECK(t.kind == Transition::Open && t.name == "test.menu.exists" && t.arg == "arg");
    CHECK(Registry::instance().has("test.menu.exists"));
    // when nothing is registered the menu is empty and has nothing to focus
    MenuSpec e;
    e.build = [](MenuBuilder& m, Ctx&) { m.submenu("MISSING", "test.menu.missing"); };
    Menu me(e);
    press(me, cx, Action::Left);
    CHECK_EQ(me.size(), size_t(0));
    CHECK_EQ(me.cursor(), -1);
    CHECK(press(me, cx, Action::A).kind == Transition::None);
  }

  // ---- action with confirm: YES/NO dialog, focus starts on NO, B cancels, YES runs it
  {
    Ctx cx;
    make_ctx(cx);
    auto f = std::make_shared<Fixture>();
    auto m = make(f);
    for (int i = 0; i < 5; ++i) press(*m, cx, Action::Down);  // DANGER (cursor 9)
    CHECK_EQ(m->cursor(), 9);
    Transition t = press(*m, cx, Action::A);
    CHECK(t.kind == Transition::Push && t.screen);
    CHECK_EQ(f->action_calls, 0);
    auto* d = static_cast<ui::ConfirmDialog*>(t.screen.get());
    CHECK_EQ(d->focus(), 1);  // NO
    CHECK(d->handle(Action::A, cx).kind == Transition::Pop);
    CHECK_EQ(f->action_calls, 0);
    CHECK(d->handle(Action::B, cx).kind == Transition::Pop);
    d->handle(Action::Left, cx);
    CHECK_EQ(d->focus(), 0);  // YES
    CHECK(d->handle(Action::A, cx).kind == Transition::Pop);
    CHECK_EQ(f->action_calls, 1);
    // a callback that returns a Transition replaces the dialog (B afterwards goes back to the menu)
    Transition t3 = ui::confirm("Q", [](Ctx&) { return Transition::open("test.menu.exists"); });
    t3.screen->handle(Action::Left, cx);
    Transition r = t3.screen->handle(Action::A, cx);
    CHECK(r.kind == Transition::Replace && r.screen);
    Transition t4 = ui::confirm("Q", [](Ctx&) { return Transition::quit(); });
    t4.screen->handle(Action::Left, cx);
    CHECK(t4.screen->handle(Action::A, cx).kind == Transition::Quit);
    // message: a single OK button, A and B close it
    Transition t5 = ui::message("hello");
    CHECK(t5.screen->handle(Action::A, cx).kind == Transition::Pop);
  }

  // ---- on-screen keyboard
  {
    Ctx cx;
    make_ctx(cx);
    std::string got;
    int done = 0;
    ui::KeyboardOpts o;
    o.title = "NAME";
    o.max_len = 5;
    Transition t = ui::keyboard(o, "ab", [&](Ctx&, const std::string& s) { got = s, ++done; });
    auto* k = static_cast<ui::KeyboardScreen*>(t.screen.get());
    CHECK_EQ(k->text(), std::string("ab"));
    k->handle(Action::A, cx);  // cursor starts on 'q'
    CHECK_EQ(k->text(), std::string("abq"));
    k->handle(Action::Right, cx);
    k->handle(Action::A, cx);
    CHECK_EQ(k->text(), std::string("abqw"));
    k->handle(Action::B, cx);  // delete
    CHECK_EQ(k->text(), std::string("abq"));
    k->handle(Action::X, cx);  // space
    CHECK_EQ(k->text(), std::string("abq "));
    k->handle(Action::Y, cx);  // shift: upper case
    CHECK_EQ(k->layer(), 1);
    k->handle(Action::A, cx);  // 'w' under the cursor, now 'W'
    CHECK_EQ(k->text(), std::string("abq W"));
    k->handle(Action::A, cx);  // max length 5: ignored
    CHECK_EQ(k->text(), std::string("abq W"));
    k->handle(Action::Y, cx);
    CHECK_EQ(k->layer(), 0);
    // move to the bottom row with Down (4 times from row 1 wraps through 2, 3, 4)
    for (int i = 0; i < 3; ++i) k->handle(Action::Down, cx);
    k->handle(Action::Right, cx);  // SHIFT -> 123
    k->handle(Action::A, cx);      // symbols layer
    CHECK_EQ(k->layer(), 2);
    k->handle(Action::A, cx);  // back to letters
    CHECK_EQ(k->layer(), 0);
    CHECK_EQ(done, 0);
    CHECK(k->handle(Action::Start, cx).kind == Transition::Pop);
    CHECK_EQ(done, 1);
    CHECK_EQ(got, std::string("abq W"));
    // B deletes one letter at a time and, with the text empty, cancels and closes without calling `on_done`
    {
      int d2 = 0;
      Transition tb = ui::keyboard({}, "ab", [&](Ctx&, const std::string&) { ++d2; });
      auto* kb = static_cast<ui::KeyboardScreen*>(tb.screen.get());
      CHECK(kb->handle(Action::B, cx).kind == Transition::None);
      CHECK_EQ(kb->text(), std::string("a"));
      CHECK(kb->handle(Action::B, cx).kind == Transition::None);
      CHECK_EQ(kb->text(), std::string());
      CHECK(kb->handle(Action::B, cx).kind == Transition::Pop);
      CHECK_EQ(d2, 0);
      Transition te = ui::keyboard({}, "", [&](Ctx&, const std::string&) { ++d2; });
      CHECK(te.screen->handle(Action::B, cx).kind == Transition::Pop);  // an empty field closes at the first B
      CHECK_EQ(d2, 0);
      Transition ts = ui::keyboard({}, "xy", [&](Ctx&, const std::string&) { ++d2; });
      CHECK(ts.screen->handle(Action::Start, cx).kind == Transition::Pop);  // Start is OK inside the keyboard
      CHECK_EQ(d2, 1);
    }
    // B on an empty field cancels; UTF-8 characters are removed whole; Select cancels
    Transition t2 = ui::keyboard({}, "\xC3\xB1", nullptr);
    auto* k2 = static_cast<ui::KeyboardScreen*>(t2.screen.get());
    CHECK(k2->handle(Action::B, cx).kind == Transition::None);
    CHECK_EQ(k2->text(), std::string());
    CHECK(k2->handle(Action::B, cx).kind == Transition::Pop);
    CHECK(k2->handle(Action::Select, cx).kind == Transition::Pop);
    // password: L reveals
    ui::KeyboardOpts po;
    po.password = true;
    Transition t3 = ui::keyboard(po, "", nullptr);
    auto* k3 = static_cast<ui::KeyboardScreen*>(t3.screen.get());
    CHECK(!k3->revealed());
    k3->handle(Action::L, cx);
    CHECK(k3->revealed());
  }

  // ---- keyboard caret: blinks on the wall clock, never animates (the loop must not spin), solid while typing
  {
    Ctx cx;
    make_ctx(cx);
    Transition t = ui::keyboard({}, "x", nullptr);
    auto* k = static_cast<ui::KeyboardScreen*>(t.screen.get());
    CHECK(k->caret_visible());
    CHECK(!k->take_redraw());
    CHECK(k->wake_in().has_value());
    CHECK(*k->wake_in() > 0.45f && *k->wake_in() < 0.60f);  // >= 0.5 s per phase
    CHECK(!k->tick(0.016f, cx));
    CHECK(k->caret_visible());  // too early to flip
    CHECK(!k->take_redraw());
    std::this_thread::sleep_for(std::chrono::milliseconds(580));
    CHECK(!k->tick(0.58f, cx));  // not an animation
    CHECK(!k->caret_visible());
    CHECK(k->take_redraw());
    CHECK(!k->take_redraw());  // once
    CHECK(*k->wake_in() > 0.45f);
    std::this_thread::sleep_for(std::chrono::milliseconds(580));
    k->tick(0.58f, cx);
    CHECK(k->caret_visible());
    std::this_thread::sleep_for(std::chrono::milliseconds(580));
    k->tick(0.58f, cx);
    CHECK(!k->caret_visible());
    k->handle(Action::Right, cx);  // any key: solid again, phase restarts
    CHECK(k->caret_visible());
    CHECK(*k->wake_in() > 0.45f);
    // a menu has no timer at rest, so once the keyboard is gone the loop sleeps until the next event
    auto f = std::make_shared<Fixture>();
    auto m = make(f);
    press(*m, cx, Action::Left);
    CHECK(!m->wake_in().has_value());
    CHECK(!m->tick(1.0f, cx));
  }

  // ---- Start inside a menu closes the whole menu stack (every level runs its on_close), down to the first non-menu
  {
    struct Probe : Screen {
      int handled = 0;
      std::vector<std::function<Transition()>> queue;  // what the next actions return
      Transition handle(Action, Ctx&) override {
        ++handled;
        if (queue.empty()) return Transition::none();
        Transition t = queue.front()();
        queue.erase(queue.begin());
        return t;
      }
      void draw(Ctx&, std::vector<Op>&) override {}
    };
    auto closes = std::make_shared<int>(0);
    std::function<std::unique_ptr<Screen>(int)> level = [closes, &level](int n) {
      MenuSpec s;
      s.title = "L" + std::to_string(n);
      s.build = [n, &level](MenuBuilder& m, Ctx&) {
        m.action("NEXT", [n, &level]() -> Transition { return Transition::push(level(n + 1)); });
        m.action("ASK", []() -> Transition { return ui::confirm("Q", [](Ctx&) { return Transition::close_menus(); }); });
      };
      s.on_close = [closes](Ctx&) { ++*closes; };
      return make_menu(std::move(s));
    };
    Ctx cx;
    make_ctx(cx);
    auto probe = std::make_unique<Probe>();
    Probe* pp = probe.get();
    App app(std::move(cx), std::move(probe), make_status_source("/nonexistent"));
    // stack: probe, L1, L2
    pp->queue.push_back([&] { return Transition::push(level(1)); });
    app.handle(Action::A);  // the probe opens L1
    CHECK_EQ(pp->handled, 1);
    app.handle(Action::A);  // L1: NEXT -> L2
    CHECK_EQ(*closes, 0);
    app.handle(Action::Start);
    CHECK_EQ(*closes, 2);  // both levels ran their on_close
    app.handle(Action::Right);
    CHECK_EQ(pp->handled, 2);  // the probe, a non-menu, is on top again
    // B only goes one level back
    pp->queue.push_back([&] { return Transition::push(level(1)); });
    app.handle(Action::A);  // handled 3
    app.handle(Action::A);  // L1 -> L2
    app.handle(Action::B);  // L2 -> L1
    CHECK_EQ(*closes, 3);
    app.handle(Action::Right);
    CHECK_EQ(pp->handled, 3);  // still inside L1: the probe did not get it
    app.handle(Action::Start);
    CHECK_EQ(*closes, 4);
    app.handle(Action::Right);
    CHECK_EQ(pp->handled, 4);
    // a dialog callback may close everything too (the dialog counts as a menu screen)
    pp->queue.push_back([&] { return Transition::push(level(1)); });
    app.handle(Action::A);  // handled 5, L1 on top
    app.handle(Action::Down);
    app.handle(Action::A);  // ASK: confirm dialog
    app.handle(Action::Left);  // YES
    app.handle(Action::A);
    CHECK_EQ(*closes, 5);
    app.handle(Action::Right);
    CHECK_EQ(pp->handled, 6);
    // at rest nothing wakes the loop
    CHECK(!app.screen_deadline().has_value());
  }

  // ---- text entry opens the keyboard with the current value and stores the result
  {
    Ctx cx;
    make_ctx(cx);
    auto f = std::make_shared<Fixture>();
    f->typed = "old";
    auto m = make(f);
    for (int i = 0; i < 4; ++i) press(*m, cx, Action::Down);  // TXT (cursor 8)
    CHECK_EQ(m->cursor(), 8);
    Transition t = press(*m, cx, Action::A);
    CHECK(t.kind == Transition::Push);
    auto* k = static_cast<ui::KeyboardScreen*>(t.screen.get());
    CHECK_EQ(k->text(), std::string("old"));
    k->handle(Action::A, cx);
    k->handle(Action::Start, cx);
    CHECK_EQ(f->typed, std::string("oldq"));
    m->tick(0.0f, cx);
    CHECK_EQ(m->entry(8).value_text, std::string("oldq"));
  }

  // ---- asynchronous menus: refresh() rebuilds, keeps the cursor on the same entry, and is harmless after the menu is gone
  {
    Ctx cx;
    make_ctx(cx);
    auto nets = std::make_shared<std::vector<std::string>>();
    Refresh keep;
    int opened = 0;
    {
      MenuSpec s;
      s.title = "WIFI";
      s.build = [nets](MenuBuilder& m, Ctx&) {
        m.toggle("WIFI", [] { return true; }, nullptr).with_id("wifi");
        if (nets->empty()) m.info("Scanning...", "");
        for (const std::string& n : *nets) m.action(n, [] {}).with_id(n);
      };
      s.on_open = [&](Ctx&, const Refresh& r) { ++opened, keep = r; };
      Menu m(s);
      press(m, cx, Action::Left);
      CHECK_EQ(opened, 1);
      CHECK_EQ(m.size(), size_t(2));
      CHECK_EQ(m.cursor(), 0);
      nets->push_back("Casa");
      nets->push_back("Bar");
      keep();  // from the job's `done`
      m.tick(0.0f, cx);
      CHECK_EQ(m.size(), size_t(3));  // wifi, Casa, Bar
      CHECK_EQ(m.cursor(), 0);
      press(m, cx, Action::Down);
      press(m, cx, Action::Down);
      CHECK_EQ(m.cursor(), 2);  // Bar
      nets->erase(nets->begin());  // Casa disappears: Bar moves up, the cursor follows it by id
      keep();
      m.tick(0.0f, cx);
      CHECK_EQ(m.size(), size_t(2));
      CHECK_EQ(m.cursor(), 1);
      CHECK_EQ(m.entry(1).id, std::string("Bar"));
      CHECK_EQ(opened, 1);  // on_open runs once
    }
    keep();  // the menu is gone: must not crash
  }

  // ---- a menu with nothing selectable scrolls instead of moving a cursor
  {
    Ctx cx;
    make_ctx(cx);
    MenuSpec s;
    s.title = "INFO";
    s.build = [](MenuBuilder& m, Ctx&) {
      for (int i = 0; i < 30; ++i) m.info("ROW " + std::to_string(i), "v");
    };
    Menu m(s);
    CHECK(press(m, cx, Action::Down).kind == Transition::None);
    CHECK_EQ(m.cursor(), -1);
    CHECK(press(m, cx, Action::A).kind == Transition::None);
    CHECK(press(m, cx, Action::B).kind == Transition::Pop);
  }

  // ---- an action may return any transition
  {
    Ctx cx;
    make_ctx(cx);
    MenuSpec s;
    s.build = [](MenuBuilder& m, Ctx&) {
      m.action("QUIT", [] { return Transition::quit(); });
      m.action("CTX", [](Ctx& c) { c.sv->toasts.push("hi"); });
    };
    Menu m(s);
    CHECK(press(m, cx, Action::A).kind == Transition::Quit);
    press(m, cx, Action::Down);
    press(m, cx, Action::A);
    CHECK_EQ(cx.sv->toasts.items().size(), size_t(1));
  }

  // ---- OSD: shows, rides on one marker toast, hides
  {
    Ctx cx;
    make_ctx(cx);
    osd_clear();
    CHECK(!osd_visible());
    osd_show_level(cx, "volume", 0.5f, 5000);
    CHECK(osd_visible());
    CHECK_EQ(cx.sv->toasts.items().size(), size_t(1));
    osd_show_level(cx, "volume", 0.6f, 5000);  // the marker already covers it
    CHECK_EQ(cx.sv->toasts.items().size(), size_t(1));
    CHECK(cx.sv->toasts.next_deadline().has_value());
    osd_clear();
    CHECK(!osd_visible());
  }

  // ---- drawing (needs the real theme and font): every state at the four sizes, stable op counts
  {
    const std::string dir = "/home/epsil/zenith-frontend/theme-dark";
    if (is_dir(dir)) {
      const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
      for (const auto& wh : sizes) {
        Ctx cx;
        std::string err;
        cx.w = float(wh[0]);
        cx.h = float(wh[1]);
        CHECK(Theme::load(dir, ratio_name(wh[0], wh[1]), cx.th, err));
        CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
        auto f = std::make_shared<Fixture>();
        auto m = make(f);
        std::vector<Op> a1, a2;
        m->draw(cx, a1);
        CHECK(a1.size() > 20);
        m->draw(cx, a2);
        CHECK_EQ(a1.size(), a2.size());  // nothing is rebuilt on a second frame
        {
          // steady state: warm frames allocate nothing and are cheap (the Op vector keeps its capacity)
          a2.clear();
          m->draw(cx, a2);
          const int N = 200;
          size_t before = g_allocs;
          auto t0 = std::chrono::steady_clock::now();
          for (int i = 0; i < N; ++i) {
            a2.clear();
            m->draw(cx, a2);
          }
          double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / N;
          size_t allocs = g_allocs - before;
          std::printf("[menu perf] %ux%u: %.1f us/frame, %zu allocations in %d frames, %zu ops\n", wh[0], wh[1], us, allocs, N, a2.size());
          CHECK_EQ(allocs, size_t(0));
          // at rest the menu asks for no more frames and sets no timer
          for (int i = 0; i < 4; ++i) m->tick(1.0f, cx);
          CHECK(!m->tick(0.016f, cx));
          CHECK(!m->wake_in().has_value());
        }
        {
          // overlays keep clear of the header: the OSD pill never touches the title or lies across the hairline, and the
          // first toast starts below both (a long title is the hard case at 480x320)
          const char* titles[2] = {"TEST", "USER INTERFACE SETTINGS AND OTHER LONG THINGS"};
          for (const char* title : titles) {
            MenuSpec ls;
            ls.title = title;
            ls.build = [](MenuBuilder& mb, Ctx&) { mb.action("ONE", [] {}); };
            Menu lm(ls);
            std::vector<Op> tmp;
            lm.draw(cx, tmp);
            const ui::HeaderBox hb = ui::header_box();
            CHECK(hb.valid);
            OsdBox ob = osd_box(cx);
            bool hits_title = ob.x < hb.title_x1 && ob.x + ob.w > hb.title_x0 && ob.y < hb.title_y1 && ob.y + ob.h > hb.title_y0;
            bool straddles = ob.y < hb.line_y + 1.0f && ob.y + ob.h > hb.line_y;
            if (hits_title || straddles) std::printf("[menu overlay] %ux%u '%s': pill (%g,%g,%g,%g) title (%g..%g, %g..%g) line %g\n", wh[0], wh[1], title, ob.x, ob.y, ob.w, ob.h, hb.title_x0, hb.title_x1, hb.title_y0, hb.title_y1, hb.line_y);
            CHECK(!hits_title);
            CHECK(!straddles);
            // a short title leaves room in the header band: there the pill sits (the 3:2 theme slot would cut the hairline)
            if (title == titles[0] && wh[0] == 480 && wh[1] == 320) CHECK(ob.y + ob.h <= hb.line_y);
            float top = toast_slot_top(cx, 4.0f);
            CHECK(top >= ob.y + ob.h + 4.0f - 1.0f);
            CHECK(top >= hb.line_y + 4.0f - 1.0f);
            tmp.clear();
            draw_toasts(cx, tmp);  // reads and clears the header box
            CHECK(!ui::header_box().valid);
          }
        }
        for (int i = 0; i < 9; ++i) {
          press(*m, cx, Action::Down);
          m->tick(1.0f, cx);
          std::vector<Op> a3;
          m->draw(cx, a3);
          CHECK(a3.size() > 20);
        }
        // dialogs
        Transition t = press(*m, cx, Action::A);  // the cursor is on the CHO row: its popup
        std::vector<Op> d;
        CHECK(t.screen != nullptr);
        if (t.screen) t.screen->draw(cx, d);
        CHECK(d.size() > 10);
        Transition cf = ui::confirm("Sure?", nullptr);
        d.clear();
        cf.screen->draw(cx, d);
        CHECK(d.size() > 10);
        Transition kb = ui::keyboard({}, "wifi", nullptr);
        d.clear();
        kb.screen->draw(cx, d);
        CHECK(d.size() > 100);
        Transition ch = ui::choose("T", {{"a", "A"}, {"b", "B"}}, "a", nullptr);
        d.clear();
        ch.screen->draw(cx, d);
        CHECK(d.size() > 10);
        osd_show_level(cx, "brightness", 0.3f, 3000);
        cx.sv->toasts.push("Wi-Fi connected", 3000);
        d.clear();
        draw_toasts(cx, d);
        CHECK(d.size() > 10);
        // steady state of the overlays and of every dialog: no allocations
        Transition ck = ui::keyboard({}, "wifi", nullptr), cc = ui::confirm("Sure?", nullptr), cp = ui::choose("T", {{"a", "A"}, {"b", "B"}}, "a", nullptr);
        // keyboards whose field text is longer than the small-string buffer (a password shows 3 bytes per bullet)
        ui::KeyboardOpts pw;
        pw.password = true;
        Transition ckp = ui::keyboard(pw, "hunter22", nullptr), ckl = ui::keyboard({}, "a long network name over fifteen bytes", nullptr);
        for (int pass = 0; pass < 2; ++pass) {
          size_t before = g_allocs;
          for (int i = 0; i < 20; ++i) {
            d.clear();
            draw_toasts(cx, d);
            ck.screen->draw(cx, d);
            ckp.screen->draw(cx, d);
            ckl.screen->draw(cx, d);
            cc.screen->draw(cx, d);
            cp.screen->draw(cx, d);
          }
          if (pass == 1) {
            std::printf("[menu perf] %ux%u: toast+osd+3 dialogs: %zu allocations in 20 frames\n", wh[0], wh[1], g_allocs - before);
            CHECK_EQ(g_allocs - before, size_t(0));
          }
        }
        osd_clear();
      }
    }
  }
  return zt::done();
}
