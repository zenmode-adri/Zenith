#include <cstdio>
#include <string>
#include <unistd.h>

#include "check.hpp"
#include "cpu.hpp"
#include "gfx.hpp"

using namespace zf;

static bool make_png(const std::string& path, uint32_t w, uint32_t h, uint8_t r) {
  Canvas c;
  c.w = w;
  c.h = h;
  c.px.assign(size_t(w) * h * 4, 255);
  for (size_t i = 0; i < c.px.size(); i += 4) c.px[i] = r;
  std::string err;
  return save_png(c, path, err);
}

int main() {
  std::string base = "/tmp/zf_test_gfx_" + std::to_string(getpid());
  std::string pa = base + "_a.png", pb = base + "_b.png", pc = base + "_c.png";
  CHECK(make_png(pa, 100, 100, 10));
  CHECK(make_png(pb, 100, 100, 20));
  CHECK(make_png(pc, 100, 100, 30));

  Assets a;
  ImgId ia = 0, ib = 0, ic = 0;
  CHECK(a.file(pa, ia) && a.file(pb, ib) && a.file(pc, ic));
  CHECK(ia != ib && ib != ic);
  CHECK_EQ(a.img(ia).w, 100u);
  CHECK_EQ(a.img(ia).rgba[0], uint8_t(10));
  ImgId again = 0;
  CHECK(a.file(pa, again) && again == ia);  // same file: same id, decoded once

  // decode at a smaller size keeps the aspect ratio
  ImgId fit = 0;
  CHECK(a.file_fit(pa, 40, 20, fit));
  CHECK(a.img(fit).w <= 40 && a.img(fit).h <= 20);

  CHECK(!a.file(base + "_missing.png", again));

  // LRU: budget smaller than all images; pinned and in-frame images survive, the other is evicted
  a.pin(ia);
  std::vector<Op> frame = {Op::image(ic, 0, 0, 10, 10)};
  a.end_frame(frame, 50000);
  CHECK(!a.img(ia).rgba.empty());
  CHECK(!a.img(ic).rgba.empty());
  CHECK(a.img(ib).rgba.empty());
  CHECK_EQ(a.img(ib).w, 100u);  // size is kept for layout
  CHECK(a.bytes() <= 50000 + 2 * 100 * 100 * 4);

  // cpu raster: opaque rect, then a tinted image on top
  std::vector<Op> ops = {Op::rect(0, 0, 20, 20, {0, 0, 255, 255}), Op::image(ic, 5, 5, 10, 10, {255, 255, 255, 255})};
  Canvas cv = cpu_render(ops, a, 20, 20);
  CHECK_EQ(cv.px[(0 * 20 + 0) * 4 + 2], uint8_t(255));       // rect colour (blue)
  CHECK_EQ(cv.px[(10 * 20 + 10) * 4 + 0], uint8_t(30));      // image pixel (red channel 30)
  // clip cuts drawing
  std::vector<Op> ops2 = {Op::rect(0, 0, 20, 20, {0, 0, 0, 255}), Op::clip(0, 0, 5, 5), Op::rect(0, 0, 20, 20, {255, 255, 255, 255}), Op::unclip()};
  Canvas cv2 = cpu_render(ops2, a, 20, 20);
  CHECK_EQ(cv2.px[(2 * 20 + 2) * 4], uint8_t(255));
  CHECK_EQ(cv2.px[(10 * 20 + 10) * 4], uint8_t(0));

  std::remove(pa.c_str());
  std::remove(pb.c_str());
  std::remove(pc.c_str());
  return zt::done();
}
