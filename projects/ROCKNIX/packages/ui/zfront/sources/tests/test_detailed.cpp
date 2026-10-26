// Tests of the detailed column: scroll timing (pure functions), layout of the description window, stars, the settle
// logic and timers of ListScreen. The layout tests need the Zenith theme folder (ZF_THEME_DIR, default: the one
// `make-themes.sh` builds next to the repo); without it they are skipped.
#include <cmath>
#include <cstdlib>
#include <string>

#include "app.hpp"
#include "check.hpp"
#include "detailed.hpp"
#include "screens.hpp"
#include "util.hpp"

using namespace zf;

static void test_scroll_timing() {
  const int max_off = 100;
  CHECK_EQ(desc_scroll_offset(0.0, max_off), 0);
  CHECK_EQ(desc_scroll_offset(5.99, max_off), 0);  // ES waits 6 s before the first pixel
  CHECK_EQ(desc_scroll_offset(6.0, max_off), 1);
  CHECK_EQ(desc_scroll_offset(6.149, max_off), 1);
  CHECK_EQ(desc_scroll_offset(6.15, max_off), 2);  // then 1 px every 150 ms
  double t_end = kDescDelay + (max_off - 1) * kDescStep;
  CHECK_EQ(desc_scroll_offset(t_end + 0.001, max_off), max_off);
  CHECK_EQ(desc_scroll_offset(t_end + 5.9, max_off), max_off);  // holds at the end
  double period = t_end + kDescHold;
  CHECK_EQ(desc_scroll_offset(period + 0.01, max_off), 0);  // jumps back, waits 6 s again
  CHECK_EQ(desc_scroll_offset(period + 5.9, max_off), 0);
  CHECK_EQ(desc_scroll_offset(period + 6.0, max_off), 1);
  CHECK_EQ(desc_scroll_offset(1000.0, 0), 0);  // fits: never scrolls
  CHECK(std::isinf(desc_scroll_next(3.0, 0)));  // and never wakes the loop

  // the next change is where it says: just before, same offset; just after, a different one
  for (double t : {0.0, 2.5, 5.99, 6.0, 6.07, 6.15, 9.3, t_end - 0.01, t_end, t_end + 3.0, period - 0.01, period, period + 6.05, 5 * period + 7.3}) {
    int o = desc_scroll_offset(t, max_off);
    double n = desc_scroll_next(t, max_off);
    CHECK(n > 0 && std::isfinite(n));
    CHECK_EQ(desc_scroll_offset(t + n * 0.5, max_off), o);
    CHECK(desc_scroll_offset(t + n + 0.002, max_off) != o);
  }
  // few pixels: still the same shape
  CHECK_EQ(desc_scroll_offset(6.0, 1), 1);
  CHECK_EQ(desc_scroll_offset(12.0, 1), 0);  // period = 12 s
}

static std::string theme_dir() {
  const char* e = std::getenv("ZF_THEME_DIR");
  return e ? e : "/home/epsil/zenith-frontend/theme-dark";
}

static size_t count_images(const std::vector<Op>& ops) {
  size_t n = 0;
  for (const Op& o : ops) n += o.kind == Op::Image;
  return n;
}

static const std::string kLong =
    "Link's latest challenge is set on the mysterious Koholint, a place where dreams and reality collide. As a castaway, Link must find a way to escape "
    "from the island and return to his beloved homeland of Hyrule. This will not be an easy task: he must wake the legendary Wind Fish by collecting "
    "eight instruments scattered through the dungeons of the island.";

static void test_column(Theme& th, Assets& a, float w, float h) {
  DetailedData d;
  d.name = "Zelda";
  d.year = 1998;
  d.genre = "Adventure";
  d.players = "1";
  d.rating = 1.0f;
  d.desc = kLong;

  d.desc_time = 0;
  std::vector<Op> ops;
  detailed_column(th, a, w, h, d, ops);
  CHECK(d.desc_max_scroll > 0);  // a long text overflows: it autoscrolls
  // the description is a clipped window: Clip ... Image ... Unclip at the end of the column
  CHECK(ops.size() > 4 && ops.back().kind == Op::Unclip);
  const Op& clip = ops[ops.size() - 3];
  const Op& img = ops[ops.size() - 2];
  CHECK(clip.kind == Op::Clip && img.kind == Op::Image);
  CHECK(img.h > clip.h);                           // text taller than the window: it is cut (half a line at the bottom)
  float y_first = img.y;

  // after the delay the image moves up pixel by pixel, never past the end
  d.desc_time = kDescDelay + 0.151;
  std::vector<Op> o2;
  detailed_column(th, a, w, h, d, o2);
  CHECK_EQ(o2[o2.size() - 2].y, y_first - 2.0f);
  d.desc_time = 100000.0;
  std::vector<Op> o3;
  detailed_column(th, a, w, h, d, o3);
  CHECK(o3[o3.size() - 2].y >= y_first - float(d.desc_max_scroll));
  CHECK(o3[o3.size() - 2].y + o3[o3.size() - 2].h >= clip.y + clip.h - 1.0f);  // the last line stays inside the window

  // short description: fits, no animation
  d.desc = "A short one.";
  std::vector<Op> o4;
  detailed_column(th, a, w, h, d, o4);
  CHECK_EQ(d.desc_max_scroll, 0);

  // no description: no window at all
  d.desc.clear();
  std::vector<Op> o5;
  detailed_column(th, a, w, h, d, o5);
  bool any_clip = false;
  for (const Op& o : o5) any_clip |= o.kind == Op::Clip;
  CHECK(!any_clip);

  // no rating: still five stars (empty ones), same number of operations as a rated game; and the rows go away when empty
  d.desc = "x";
  d.rating = 0;
  std::vector<Op> unrated;
  detailed_column(th, a, w, h, d, unrated);
  d.rating = 1.0f;
  std::vector<Op> rated;
  detailed_column(th, a, w, h, d, rated);
  CHECK_EQ(unrated.size(), rated.size());
  CHECK(unrated[unrated.size() - 8].id != rated[rated.size() - 8].id);  // 5 star images before Clip/Image/Unclip
  CHECK_EQ(unrated[unrated.size() - 8].id, unrated[unrated.size() - 4].id);      // all five the same (empty) star
  d.genre.clear();
  d.players.clear();
  d.year = -1;
  std::vector<Op> bare;
  detailed_column(th, a, w, h, d, bare);
  CHECK_EQ(count_images(rated) - count_images(bare), size_t(6));  // 3 rows x (label + value) are gone, stars stay
}

static void test_for() {
  System sys;
  sys.name = "gbc";
  for (const char* n : {"Alpha", "Zelda: Link's Awakening"}) {
    Game g;
    g.name = n;
    g.path = std::string("/nonexistent/") + n + ".gb";
    g.meta.release_year = 1998;
    g.meta.genre = "Adventure";
    g.meta.players = "1";
    g.meta.rating_pct = 80;
    sys.games.push_back(g);
  }
  DetailedData d;
  CHECK(!detailed_for(sys, 5, true, d));
  detailed_set_demo(false);
  CHECK(detailed_for(sys, 1, false, d));
  CHECK_EQ(d.name, std::string("Zelda: Link's Awakening"));
  CHECK_EQ(d.name_up, std::string("ZELDA: LINK'S AWAKENING"));
  CHECK_EQ(d.year, 1998);
  CHECK(std::fabs(d.rating - 0.8f) < 1e-6f);
  CHECK(d.image.empty() && d.desc.empty());
  detailed_set_demo(true);  // demo: heavy parts only once settled
  CHECK(detailed_for(sys, 1, false, d));
  CHECK(d.image.empty() && d.desc.empty());
  CHECK(detailed_for(sys, 1, true, d));
  CHECK(!d.desc.empty());
  detailed_set_demo(false);
}

// ListScreen: settle delay, one redraw when it ends, timers only while there is something to wait for.
static void test_screen(Theme& th, Assets& a, float w, float h) {
  Ctx cx;
  cx.th = th;
  cx.assets = std::move(a);
  cx.w = w;
  cx.h = h;
  std::string err;
  CHECK(load_common(cx.th, cx.assets, cx.common, err));
  System sys;
  sys.name = sys.theme = "gbc";
  sys.fullname = "Game Boy Color";
  for (const char* n : {"Alpha", "Zelda: Link's Awakening", "Mario"}) {
    Game g;
    g.name = n;
    sys.games.push_back(g);
  }
  sys.count = sys.games.size();
  cx.systems.push_back(sys);
  cx.items.push_back(SystemItem{"gbc", "Game Boy Color", 3});
  detailed_set_demo(true);
  ListScreen s(0, cx);
  std::vector<Op> ops;
  s.draw(cx, ops);  // entering the list: settled at once
  auto wake = [&]() { return s.wake_in(); };
  CHECK(!wake());  // "Alpha": short description, nothing to wait for
  (void)s.handle(Action::Down, cx);
  CHECK(wake() && *wake() > 0.11f && *wake() < 0.13f);  // settle timer
  ops.clear();
  s.draw(cx, ops);
  CHECK(!s.take_redraw());
  CHECK(!s.tick(0.05f, cx) && !s.take_redraw());  // not yet
  CHECK(!s.tick(0.1f, cx));
  CHECK(s.take_redraw() && !s.take_redraw());  // settled: exactly one redraw, not an animation
  ops.clear();
  s.draw(cx, ops);
  auto w1 = wake();
  CHECK(w1 && *w1 > 5.0f && *w1 < 6.1f);  // long description: sleeps until the 6 s delay ends
  CHECK(!s.tick(1.0f, cx) && !s.take_redraw());
  s.tick(5.1f, cx);
  CHECK(s.take_redraw());  // first pixel of scroll
  ops.clear();
  s.draw(cx, ops);
  auto w2 = wake();
  CHECK(w2 && *w2 < 0.17f);
  // moving again resets everything
  (void)s.handle(Action::Down, cx);
  ops.clear();
  s.draw(cx, ops);
  CHECK(wake() && *wake() < 0.13f);
  detailed_set_demo(false);
  a = std::move(cx.assets);
}

int main() {
  test_scroll_timing();
  test_for();
  std::string dir = theme_dir();
  Theme th;
  Assets a;
  std::string err;
  if (!Theme::load(dir, "1/1", th, err) || !a.load_font(th.path(th.str("font")), err)) {
    std::fprintf(stderr, "test_detailed: theme not found at %s (%s): layout tests skipped\n", dir.c_str(), err.c_str());
    return zt::done();
  }
  test_column(th, a, 720, 720);
  test_screen(th, a, 720, 720);
  return zt::done();
}
