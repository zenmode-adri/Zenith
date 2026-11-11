// The neighbours of the list cursor are made ready while it rests: their covers are decoded on the job thread one at a time after the
// cursor's own, so the frame that brings a neighbour to the cursor finds its cover there and queues nothing (that frame was the stall:
// 129-160 ms the first time each cover appeared). Needs the Zenith theme folder (ZF_THEME_DIR); skipped without it.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app.hpp"
#include "check.hpp"
#include "covers.hpp"
#include "cpu.hpp"
#include "detailed.hpp"
#include "screens.hpp"
#include "util.hpp"

using namespace zf;

namespace {

std::string theme_dir() {
  const char* e = std::getenv("ZF_THEME_DIR");
  return e ? e : "/usr/share/zfront/theme-dark";
}

bool make_png(const std::string& path, uint32_t w, uint32_t h, uint8_t r) {
  Canvas c;
  c.w = w;
  c.h = h;
  c.px.assign(size_t(w) * h * 4, 255);
  for (size_t i = 0; i < c.px.size(); i += 4) c.px[i] = r;
  std::string err;
  return save_png(c, path, err);
}

void settle(Ctx& cx) {
  while (!cx.sv->jobs.idle()) {
    cx.sv->jobs.drain();
    usleep(1000);
  }
  cx.sv->jobs.drain();
}

size_t count_images(const std::vector<Op>& ops) {
  size_t n = 0;
  for (const Op& o : ops) n += o.kind == Op::Image;
  return n;
}

}  // namespace

int main() {
  Theme th;
  Assets a;
  std::string err;
  if (!Theme::load(theme_dir(), "1/1", th, err) || !a.load_font(th.path(th.str("font")), err)) {
    std::fprintf(stderr, "test_list_prefetch: theme not found (%s): skipped\n", err.c_str());
    return 0;
  }
  char tmpl[] = "/tmp/zf-prefetch-XXXXXX";
  const std::string dir = mkdtemp(tmpl);
  for (int i = 0; i < 24; ++i) CHECK(make_png(dir + "/" + std::to_string(i) + ".png", 40 + uint32_t(i), 30, uint8_t(i * 10)));  // one distinct file per game
  ::setenv("ZF_DEMO_COVER_DIR", dir.c_str(), 1);

  Ctx cx;
  cx.th = th;
  cx.assets = std::move(a);
  cx.w = cx.h = 720;
  CHECK(load_common(cx.th, cx.assets, cx.common, err));
  System sys;
  sys.name = sys.theme = "gbc";
  sys.fullname = "Game Boy Color";
  for (int i = 1; i <= 6; ++i) {
    Game g;
    g.name = "Game " + std::to_string(i);
    g.path = "/roms/gbc/" + g.name + ".gbc";
    sys.games.push_back(g);
  }
  sys.count = sys.games.size();
  sys.loaded = true;
  cx.systems.push_back(sys);
  cx.items.push_back(SystemItem{"gbc", "Game Boy Color", 6});
  detailed_set_demo(true);
  cx.async_covers = true;  // the device loop: nothing is decoded by the frame that draws

  ListScreen s(0, cx);  // row 0: "Game 1", settled from the start
  std::vector<Op> ops;
  s.tick(0.01f, cx);
  s.draw(cx, ops);
  CHECK_EQ(covers::in_flight(), size_t(1));  // the cursor's own cover first
  CHECK(s.wake_in() == std::nullopt || *s.wake_in() > 0.0f);  // while it is missing nothing else is asked
  settle(cx);
  CHECK_EQ(covers::in_flight(), size_t(0));
  ops.clear();
  s.draw(cx, ops);  // its cover is there: nothing is missing, the neighbours may start
  CHECK_EQ(covers::in_flight(), size_t(0));
  CHECK(s.wake_in().has_value());  // the loop is told to wake for them
  s.tick(0.5f, cx);                // the rest delay is over
  CHECK(s.take_redraw());
  // The neighbours one by one: each is drawn into a scrap, its cover requested, and the next one waits until it landed.
  size_t asked = 0;
  for (int step = 0; step < 16; ++step) {
    ops.clear();
    s.draw(cx, ops);
    if (covers::in_flight() > 0) {
      CHECK_EQ(covers::in_flight(), size_t(1));  // never two at once: the newest request would cancel the other
      ++asked;
      settle(cx);
    }
    s.tick(0.1f, cx);
  }
  CHECK(asked >= 2);  // Game 2 and Game 3 (two neighbours below; none above the first row)
  CHECK(cx.sv->jobs.idle());
  CHECK(!s.wake_in().has_value() || *s.wake_in() > 0.0f);  // done: the loop sleeps again

  // The key that brings Game 2 to the cursor: its cover is already there, the settled frame decodes and queues nothing.
  s.handle(Action::Down, cx);
  s.tick(1.0f, cx);
  ops.clear();
  s.draw(cx, ops);
  CHECK_EQ(covers::in_flight(), size_t(0));
  CHECK(cx.sv->jobs.idle());
  CHECK_EQ(cx.assets.inline_fits(), size_t(0));
  CHECK(count_images(ops) > 0);

  // Without the prefetch (a game far from the ones made ready) the cover is asked for as before.
  s.handle(Action::Down, cx);
  s.handle(Action::Down, cx);
  s.handle(Action::Down, cx);  // Game 5: not a neighbour of Game 1 or 2
  s.tick(1.0f, cx);
  ops.clear();
  s.draw(cx, ops);
  CHECK_EQ(covers::in_flight(), size_t(1));
  settle(cx);

  detailed_set_demo(false);
  ::unsetenv("ZF_DEMO_COVER_DIR");
  std::string cmd = "rm -rf " + dir;
  if (std::system(cmd.c_str()) != 0) {
  }
  return zt::done();
}
