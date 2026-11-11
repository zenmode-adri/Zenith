// tests/test_zen_motion.cpp: moving between systems must not make new pictures on every frame.
// A logo decoded and scaled for each size of a growing card took the console from 60 to 20 frames per second.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "cpu.hpp"
#include "gfx.hpp"
#include "remake.hpp"
#include "scene.hpp"
#include "status.hpp"
#include "theme.hpp"

using namespace zf;
namespace fs = std::filesystem;

int main() {
  std::string base;
  if (const char* e = std::getenv("ZF_THEME_DIR")) base = std::string(e) + "/theme-dark";
  if (base.empty() || !fs::exists(base + "/theme.xml")) base = "../themes/theme-dark";
  if (!fs::exists(base + "/theme.xml")) {
    std::printf("sin carpeta del tema: se omite\n");
    return zt::done();
  }
  // Zenith Next is its folder with one more variable, as the package makes it
  char tmpl[] = "/tmp/zf-test-motion-XXXXXX";
  const std::string dir = std::string(mkdtemp(tmpl)) + "/next";
  fs::copy(fs::canonical(base), dir, fs::copy_options::recursive);
  {
    std::ifstream in(dir + "/theme.xml");
    std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t at = xml.find("<variables>");
    CHECK(at != std::string::npos);
    xml.insert(at + 11, "<zenStyle>next</zenStyle>");
    std::ofstream(dir + "/theme.xml") << xml;
  }
  const float w = 720, h = 720;
  Theme th;
  std::string err;
  CHECK(Theme::load(dir, ratio_name(720, 720), th, err));
  Assets a;
  CHECK(a.load_font(th.path(th.str("font")), err));
  Common common;
  CHECK(load_common(th, a, common, err));
  Status st = Status::demo("12:04");
  // real logos and a system without one, which draws its name instead
  std::vector<SystemItem> items(6);
  const char* sys[][2] = {{"snes", "Super Nintendo"}, {"megadrive", "Sega Mega Drive"}, {"psp", "PlayStation Portable"}, {"gba", "Game Boy Advance"}, {"nosuchlogo", "A system without a logo"}, {"nes", "Nintendo"}};
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].theme_name = sys[i][0];
    items[i].full_name = sys[i][1];
    items[i].games = 19;
  }
  auto frame = [&](int cursor, float cam) {
    std::vector<Op> ops;
    InfoFade fade;
    fade.item = cursor;
    RecentRow recent;
    recent.decode_inline = true;
    system_view(th, a, common, w, h, items, cursor, cam, st, &recent, fade, ops);
  };
  // the chosen card must not flash on its way: from the side card to the dark one, nothing brighter than both.
  // The band of the card above its logo, on the real pixels; its box is the biggest rectangle that is not the background.
  auto card_light = [&](int cursor, float cam) {
    std::vector<Op> ops;
    InfoFade fade;
    fade.item = cursor;
    RecentRow recent;
    recent.decode_inline = true;
    system_view(th, a, common, w, h, items, cursor, cam, st, &recent, fade, ops);
    const Op* box = nullptr;
    for (const Op& o : ops)
      if (o.kind == Op::Rect && o.w < w && (!box || o.w * o.h > box->w * box->h)) box = &o;
    if (!box) return 0.0f;
    const Canvas c = cpu_render(ops, a, uint32_t(w), uint32_t(h));
    const int y = int(box->y + box->h * 0.08f);
    float sum = 0;
    int n = 0;
    for (int x = int(box->x + box->w * 0.1f); x < int(box->x + box->w * 0.9f); ++x, ++n) {
      const uint8_t* p = &c.px[(size_t(y) * size_t(c.w) + size_t(x)) * 4];
      sum += float(std::max({p[0], p[1], p[2]}));
    }
    return n ? sum / float(n) : 0.0f;
  };
  const float rest_side = card_light(0, 0.0f), rest_sel = card_light(1, 1.0f);
  const float limit = std::max(rest_side, rest_sel) + 14.0f;
  float worst = 0;
  for (int f = 1; f < 30; ++f) worst = std::max(worst, card_light(1, float(f) / 30.0f));
  std::printf("luz de la ficha: quieta %.0f y %.0f, peor en movimiento %.0f\n", rest_side, rest_sel, worst);
  CHECK(worst <= limit);
  for (int from = 0; from < 4; ++from) {
    frame(from, float(from));
    const size_t before = a.count();
    for (int f = 1; f <= 30; ++f) frame(from + 1, float(from) + float(f) / 30.0f);
    const size_t made = a.count() - before;
    std::printf("paso %d: %zu imagenes nuevas en 30 fotogramas\n", from, made);
    CHECK(made <= 12);  // the two sizes of the logos that change, and the name of the new system; nothing per frame
  }
  // the list slides: at rest the rows of always, on the way the ones between
  {
    remake::DrawState& rs = remake::state();
    rs.list_cam = -1.0f;
    remake::RowWindow r = remake::row_window(40, 20, 8);
    CHECK(r.start == 16.0f && r.first == 16 && r.end == 24 && r.sel == 4.0f && r.lit == 20 && !r.moving);
    r = remake::row_window(40, 0, 8);
    CHECK(r.start == 0.0f && r.first == 0 && r.end == 8 && r.sel == 0.0f && r.lit == 0);
    r = remake::row_window(40, 39, 8);
    CHECK(r.start == 32.0f && r.end == 40 && r.sel == 7.0f && r.lit == 39);
    rs.list_cam = 19.5f;
    r = remake::row_window(40, 20, 8);
    CHECK(r.start == 15.5f && r.first == 15 && r.end == 24 && r.sel == 4.0f && r.moving);
    rs.list_cam = 1.4f;  // near the top the view stays and only the selector moves
    r = remake::row_window(40, 2, 8);
    CHECK(r.start == 0.0f && r.first == 0 && r.end == 8 && r.sel > 1.39f && r.sel < 1.41f && r.lit == 1);
    rs.list_cam = -1.0f;
  }
  std::error_code ec;
  fs::remove_all(fs::path(dir).parent_path(), ec);
  return zt::done();
}
