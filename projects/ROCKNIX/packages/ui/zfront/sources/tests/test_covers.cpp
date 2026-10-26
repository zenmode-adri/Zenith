// Covers decoded on the job thread: last request wins, no duplicates, failures are remembered, the list screen shows
// the cover once it arrives (and never decodes in `draw` when async).
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
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

// Holds the single worker thread until `release()`, so the requests behind it queue up deterministically.
struct Gate {
  std::mutex m;
  std::condition_variable cv;
  bool open = false, entered = false;
  void post(Jobs& jobs) {
    jobs.post([this] {
      std::unique_lock<std::mutex> g(m);
      entered = true;
      cv.notify_all();
      cv.wait(g, [this] { return open; });
    });
    std::unique_lock<std::mutex> g(m);
    cv.wait(g, [this] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> g(m);
    open = true;
    cv.notify_all();
  }
};

void test_last_request_wins() {
  std::string base = "/tmp/zf_test_covers_" + std::to_string(getpid());
  std::string pa = base + "_a.png", pb = base + "_b.png", pc = base + "_c.png";
  CHECK(make_png(pa, 300, 400, 10) && make_png(pb, 300, 400, 20) && make_png(pc, 300, 400, 30));
  Ctx cx;
  ImgId id = 0;
  Gate gate;
  gate.post(cx.sv->jobs);  // the worker is busy: what follows only queues
  CHECK(!covers::request(cx.assets, cx.sv->jobs, pa, 150, 200));
  CHECK(!covers::request(cx.assets, cx.sv->jobs, pb, 150, 200));
  CHECK(!covers::request(cx.assets, cx.sv->jobs, pb, 150, 200));  // already requested: no second job
  CHECK_EQ(covers::in_flight(), size_t(2));
  gate.release();
  settle(cx);
  CHECK_EQ(covers::in_flight(), size_t(0));
  CHECK(!covers::ready(cx.assets, pa, 150, 200, id));  // superseded before it started: never decoded
  CHECK(covers::ready(cx.assets, pb, 150, 200, id));
  CHECK(cx.assets.img(id).w <= 150 && cx.assets.img(id).h <= 200);
  CHECK_EQ(cx.assets.img(id).rgba[0], uint8_t(20));
  CHECK(covers::request(cx.assets, cx.sv->jobs, pb, 150, 200));  // cached: answered at once, nothing posted
  CHECK(cx.sv->jobs.idle());
  // asked again later, the skipped one is decoded
  CHECK(!covers::request(cx.assets, cx.sv->jobs, pa, 150, 200));
  settle(cx);
  CHECK(covers::ready(cx.assets, pa, 150, 200, id));
  // a request for something already queued makes it the wanted one again
  Gate g2;
  g2.post(cx.sv->jobs);
  CHECK(!covers::request(cx.assets, cx.sv->jobs, pc, 150, 200));
  CHECK(!covers::request(cx.assets, cx.sv->jobs, pc, 100, 100));  // other box: another image
  CHECK(!covers::request(cx.assets, cx.sv->jobs, pc, 150, 200));  // back to the first: it is the wanted one
  g2.release();
  settle(cx);
  CHECK(covers::ready(cx.assets, pc, 150, 200, id));
  CHECK(!covers::ready(cx.assets, pc, 100, 100, id));
  std::remove(pa.c_str());
  std::remove(pb.c_str());
  std::remove(pc.c_str());
}

void test_failure() {
  Ctx cx;
  std::string bad = "/tmp/zf_test_covers_missing_" + std::to_string(getpid()) + ".png";
  CHECK(!covers::failed(bad));
  CHECK(!covers::request(cx.assets, cx.sv->jobs, bad, 100, 100));
  settle(cx);
  CHECK(covers::failed(bad));
  CHECK(!covers::request(cx.assets, cx.sv->jobs, bad, 100, 100));
  CHECK(cx.sv->jobs.idle());  // not retried
  CHECK_EQ(covers::in_flight(), size_t(0));
}

std::string theme_dir() {
  const char* e = std::getenv("ZF_THEME_DIR");
  return e ? e : "/home/epsil/zenith-frontend/theme-dark";
}

size_t count_images(const std::vector<Op>& ops) {
  size_t n = 0;
  for (const Op& o : ops) n += o.kind == Op::Image;
  return n;
}

// ListScreen with async covers: the first settled frame has no cover and queues one job; after it ran the cover is drawn.
void test_list_screen() {
  Theme th;
  Assets a;
  std::string err;
  if (!Theme::load(theme_dir(), "1/1", th, err) || !a.load_font(th.path(th.str("font")), err)) {
    std::fprintf(stderr, "test_covers: theme not found (%s): list screen test skipped\n", err.c_str());
    return;
  }
  Ctx cx;
  cx.th = th;
  cx.assets = std::move(a);
  cx.w = cx.h = 720;
  CHECK(load_common(cx.th, cx.assets, cx.common, err));
  System sys;
  sys.name = sys.theme = "gbc";
  sys.fullname = "Game Boy Color";
  for (const char* n : {"Alpha", "Zelda: Link's Awakening"}) {
    Game g;
    g.name = n;
    sys.games.push_back(g);
  }
  sys.count = sys.games.size();
  cx.systems.push_back(sys);
  cx.items.push_back(SystemItem{"gbc", "Game Boy Color", 2});
  detailed_set_demo(true);

  // inline (harness): the cover is in the very first settled frame
  {
    ListScreen s(0, cx);
    (void)s.handle(Action::Down, cx);
    s.tick(1.0f, cx);
    std::vector<Op> ops;
    s.draw(cx, ops);
    CHECK(cx.sv->jobs.idle() && covers::in_flight() == 0);
    size_t inline_images = count_images(ops);
    // async: same state, but nothing is decoded in draw
    cx.async_covers = true;
    ListScreen s2(0, cx);
    ImgId probe = 0;
    uint32_t bw = uint32_t(cx.th.f("coverW") * 720 + 0.5f), bh = uint32_t(cx.th.f("coverH") * 720 + 0.5f);
    std::string cover = path_dir(path_dir(__FILE__)) + "/tests/data/cover-box.png";
    // drop the inline-decoded copy so the async path really has to load it
    cx.assets.end_frame({}, 0);
    CHECK(!covers::ready(cx.assets, cover, bw, bh, probe));
    (void)s2.handle(Action::Down, cx);
    s2.tick(1.0f, cx);
    std::vector<Op> o2;
    s2.draw(cx, o2);
    CHECK_EQ(count_images(o2) + 1, inline_images);  // everything but the cover
    CHECK_EQ(covers::in_flight(), size_t(1));
    settle(cx);
    CHECK_EQ(covers::in_flight(), size_t(0));
    std::vector<Op> o3;
    s2.draw(cx, o3);
    CHECK_EQ(count_images(o3), inline_images);  // the cover arrived
    CHECK(covers::ready(cx.assets, cover, bw, bh, probe));
    CHECK(cx.sv->jobs.idle());
  }
  detailed_set_demo(false);
}

}  // namespace

int main() {
  test_last_request_wins();
  test_failure();
  test_list_screen();
  return zt::done();
}
