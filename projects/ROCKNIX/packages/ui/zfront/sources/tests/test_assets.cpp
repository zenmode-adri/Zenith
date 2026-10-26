// Assets memory rules: pixels released after upload keep counting in the budget, pinned images keep theirs, eviction and
// `forget_released`, and wrapped text does not cache its line images.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "check.hpp"
#include "cpu.hpp"
#include "gfx.hpp"
#include "theme.hpp"

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

void test_release() {
  std::string base = "/tmp/zf_test_assets_" + std::to_string(getpid());
  std::string pa = base + "_a.png", pb = base + "_b.png", pc = base + "_c.png";
  CHECK(make_png(pa, 100, 100, 10) && make_png(pb, 100, 100, 20) && make_png(pc, 100, 100, 30));
  const size_t kImg = 100 * 100 * 4;
  Assets a;
  ImgId ia = 0, ib = 0, ic = 0;
  CHECK(a.file(pa, ia) && a.file(pb, ib) && a.file(pc, ic));
  CHECK_EQ(a.bytes(), 3 * kImg);
  CHECK_EQ(a.cpu_bytes(), 3 * kImg);
  CHECK(a.has_pixels(ib) && !a.evicted(ib));

  a.pin(ia);
  a.release_pixels(ia);  // pinned: refused (the scenes keep the id and the textures are rebuilt after a game)
  CHECK(a.has_pixels(ia) && a.img(ia).rgba.size() == kImg);
  a.release_pixels(ib);
  CHECK(!a.has_pixels(ib) && !a.evicted(ib));
  CHECK(a.img(ib).rgba.empty());
  CHECK(a.img(ib).w == 100 && a.img(ib).h == 100);  // layout still works
  CHECK_EQ(a.bytes(), 3 * kImg);                     // the texture is as big: still in the budget
  CHECK_EQ(a.cpu_bytes(), 2 * kImg);
  a.release_pixels(ib);  // idempotent
  CHECK_EQ(a.cpu_bytes(), 2 * kImg);
  ImgId again = 0;
  CHECK(a.file(pb, again) && again == ib);  // the cache still answers for it (the texture is alive)

  // eviction of a released image gives its whole size back, and the next request makes a new one with pixels
  std::vector<Op> frame = {Op::image(ic, 0, 0, 10, 10)};
  a.end_frame(frame, kImg);  // budget for one image: ia is pinned, ic is in the frame, ib goes
  CHECK(a.evicted(ib) && !a.has_pixels(ib));
  CHECK_EQ(a.bytes(), 2 * kImg);
  CHECK_EQ(a.cpu_bytes(), 2 * kImg);
  CHECK(a.file(pb, again) && again != ib && a.has_pixels(again));
  CHECK_EQ(a.img(again).rgba[0], uint8_t(20));

  // the window was recreated: textures gone, released images are forgotten, the others stay
  ImgId id2 = again;
  a.release_pixels(id2);
  a.release_pixels(ic);
  a.forget_released();
  CHECK(a.evicted(id2) && a.evicted(ic));
  CHECK(a.has_pixels(ia) && !a.evicted(ia));
  CHECK_EQ(a.bytes(), kImg);
  CHECK_EQ(a.cpu_bytes(), kImg);
  CHECK(a.file(pc, again) && a.has_pixels(again) && again != ic);
  std::remove(pa.c_str());
  std::remove(pb.c_str());
  std::remove(pc.c_str());
}

std::string theme_dir() {
  const char* e = std::getenv("ZF_THEME_DIR");
  return e ? e : "/home/epsil/zenith-frontend/theme-dark";
}

void test_wrap() {
  Theme th;
  Assets a;
  std::string err;
  if (!Theme::load(theme_dir(), "1/1", th, err) || !a.load_font(th.path(th.str("font")), err)) {
    std::fprintf(stderr, "test_assets: theme not found (%s): wrap test skipped\n", err.c_str());
    return;
  }
  Color c = {200, 200, 200, 255};
  const std::string text = "The quick brown fox jumps over the lazy dog and keeps running through the dark forest until the morning comes.";
  size_t n0 = a.count();
  ImgId w = a.text_wrap(text, 20, c, 220, 50, 1.3f);
  CHECK_EQ(a.count(), n0 + 1);  // one image: the lines are not cached on the side
  CHECK(a.img(w).h > 40);
  ImgId w2 = a.text_wrap(text, 20, c, 220, 50, 1.3f);
  CHECK_EQ(w, w2);

  // a line that was drawn on its own, uploaded and released can still be part of a wrapped text
  ImgId line = a.text("Hello world", 20, c);
  ImgId ref = a.text_wrap("Hello world", 20, c, 400, 5, 1.0f);
  a.release_pixels(line);
  Assets b;
  CHECK(b.load_font(th.path(th.str("font")), err));
  ImgId line_b = b.text("Hello world", 20, c);
  b.release_pixels(line_b);
  ImgId ref_b = b.text_wrap("Hello world", 20, c, 400, 5, 1.0f);
  CHECK(a.img(ref).rgba == b.img(ref_b).rgba);
  size_t ink = 0;
  for (size_t i = 3; i < b.img(ref_b).rgba.size(); i += 4) ink += b.img(ref_b).rgba[i] != 0;
  CHECK(ink > 50);
}

}  // namespace

int main() {
  test_release();
  test_wrap();
  return zt::done();
}
