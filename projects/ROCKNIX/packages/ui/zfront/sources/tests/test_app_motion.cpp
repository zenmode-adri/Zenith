// Entry / exit motion of screens (App with `cx.ui_motion`): fade + slide in, fade out, nothing at all when it is off.
#include <cmath>
#include <memory>

#include "app.hpp"
#include "check.hpp"
#include "motion.hpp"

using namespace zf;

namespace {

// bg = full-screen rect, content = a rectangle at y = 100
struct Box : Screen {
  bool menu;
  explicit Box(bool m) : menu(m) {}
  Transition handle(Action a, Ctx&) override {
    if (a == Action::A) return Transition::push(std::make_unique<Box>(true));
    if (a == Action::B) return Transition::pop();
    return Transition::none();
  }
  void draw(Ctx& cx, std::vector<Op>& out) override {
    out.push_back(Op::rect(0, 0, cx.w, cx.h, {10, 10, 10, 255}));
    out.push_back(Op::rect(20, 100, 50, 20, {255, 255, 255, 255}));
  }
  bool is_menu() const override { return menu; }
};

// ticks of 1/60 s for `secs` seconds (a single tick never counts for more than 50 ms)
void run(App& app, float secs) {
  for (float t = 0; t < secs; t += 1.0f / 60.0f) app.tick(1.0f / 60.0f);
}

App make_app(bool motion) {
  Ctx cx;
  cx.w = cx.h = 720;
  cx.ui_motion = motion;
  return App(std::move(cx), std::make_unique<Box>(false), make_status_source("/nonexistent"));
}

}  // namespace

int main() {
  {  // motion on: a pushed window starts invisible and a little lower, ends in place and opaque
    App app = make_app(true);
    app.render();
    app.handle(Action::A);
    CHECK(app.tick(0.0f));  // moving
    const std::vector<Op>& a = app.render();
    const float y0 = a[3].y;  // `render` returns the same buffer every time: keep the values
    // the screen below stays drawn (it shows through the fading background): root bg + content, then the window's bg + content
    CHECK_EQ(a.size(), 4ul);
    CHECK_EQ(int(a[2].color[3]), 0);
    CHECK_EQ(int(a[3].color[3]), 0);
    CHECK(std::fabs(a[3].y - (100.0f + 0.017f * 720.0f)) < 0.01f);  // starts 12 px low
    CHECK_EQ(a[2].y, 0.0f);                                          // the full-screen background never slides
    app.tick(0.09f);
    const std::vector<Op>& b = app.render();
    CHECK(b[3].color[3] > 128 && b[3].color[3] < 255);  // eased out: past half way at half the time
    CHECK(b[3].y > 100.0f && b[3].y < y0);
    run(app, 0.3f);
    const std::vector<Op>& c = app.render();
    CHECK_EQ(c.size(), 2ul);  // once it is over only the window is drawn: no screen under an opaque one
    CHECK_EQ(int(c[1].color[3]), 255);
    CHECK_EQ(c[1].y, 100.0f);
    CHECK(!app.tick(0.016f));  // at rest: nothing left to animate

    // popping: the window stays up for a moment, fading, above the screen that comes back
    app.handle(Action::B);
    const std::vector<Op>& d = app.render();
    CHECK_EQ(d.size(), 4ul);
    CHECK_EQ(int(d[3].color[3]), 255);  // first frame of the exit: still fully there
    app.tick(0.055f);
    const std::vector<Op>& e = app.render();
    CHECK(e[3].color[3] > 100 && e[3].color[3] < 160);  // half way
    CHECK(e[3].y > 100.0f);                              // sinking
    run(app, 0.3f);
    CHECK_EQ(app.render().size(), 2ul);
    CHECK(!app.tick(0.016f));
  }
  {  // the status bar of an entering / leaving screen stays put and opaque (it is the bar of the screen below): no ghost
    struct WithBar : Screen {
      Transition handle(Action a, Ctx&) override { return a == Action::B ? Transition::pop() : Transition::none(); }
      void draw(Ctx& cx, std::vector<Op>& out) override {
        out.push_back(Op::rect(0, 0, cx.w, cx.h, {10, 10, 10, 255}));
        const size_t first = out.size();
        out.push_back(Op::rect(600, 20, 50, 20, {255, 255, 255, 255}));  // the "clock"
        motion::status_range() = {first, out.size()};
        out.push_back(Op::rect(20, 100, 50, 20, {255, 255, 255, 255}));  // content
      }
      bool is_menu() const override { return true; }
    };
    struct Root : Screen {
      Transition handle(Action a, Ctx&) override { return a == Action::A ? Transition::push(std::make_unique<WithBar>()) : Transition::none(); }
      void draw(Ctx& cx, std::vector<Op>& out) override { out.push_back(Op::rect(0, 0, cx.w, cx.h, {10, 10, 10, 255})); }
    };
    Ctx cx;
    cx.w = cx.h = 720;
    cx.ui_motion = true;
    App app(std::move(cx), std::make_unique<Root>(), make_status_source("/nonexistent"));
    app.render();
    app.handle(Action::A);
    app.tick(0.03f);
    const std::vector<Op>& a = app.render();
    // root bg, window bg, clock, content
    CHECK_EQ(a.size(), 4ul);
    CHECK(a[1].color[3] < 255);                     // the window fades in
    CHECK_EQ(int(a[2].color[3]), 255);              // the clock does not
    CHECK_EQ(a[2].y, 20.0f);                        // and does not slide
    CHECK(a[3].color[3] < 255 && a[3].y > 100.0f);  // the content does
    run(app, 0.3f);
    app.handle(Action::B);
    app.tick(0.03f);
    const std::vector<Op>& b = app.render();
    CHECK_EQ(b.size(), 4ul);                        // root + the leaving window
    CHECK_EQ(int(b[2].color[3]), 255);
    CHECK_EQ(b[2].y, 20.0f);
    CHECK(b[3].color[3] < 255);
  }
  {  // motion off (harness, tests): the frame right after the key press is final
    App app = make_app(false);
    app.handle(Action::A);
    const std::vector<Op>& a = app.render();
    CHECK_EQ(a.size(), 2ul);
    CHECK_EQ(int(a[1].color[3]), 255);
    CHECK_EQ(a[1].y, 100.0f);
    CHECK(!app.tick(0.0f));
    app.handle(Action::B);
    CHECK_EQ(app.render().size(), 2ul);
  }
  {  // a long first tick (the loop slept) does not swallow the entry
    App app = make_app(true);
    app.handle(Action::A);
    app.tick(5.0f);
    const std::vector<Op>& a = app.render();
    CHECK(a.size() == 4ul && a[3].color[3] > 0 && a[3].color[3] < 255);
  }
  return zt::done();
}
