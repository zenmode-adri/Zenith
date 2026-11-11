// The XMB game list shows the WHOLE picture of a game in the thumbnail of each row (it was cut to 4:3: a portrait cover lost its top and its
// bottom). A portrait picture must come out portrait. Needs the Zenith bases (ZF_THEME_DIR: theme-light, theme-dark); skipped without them.
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "cpu.hpp"
#include "es_theme.hpp"
#include "gfx.hpp"
#include "remake.hpp"
#include "scene.hpp"
#include "status.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

std::string find_base(const char* name) {
  if (const char* e = std::getenv("ZF_THEME_DIR")) {
    const std::string p = std::string(e) + "/" + name;
    if (fs::exists(p + "/theme.xml")) return p;
  }
  for (const char* d : {"themes/", "../themes/", "./"}) {
    const std::string p = std::string(d) + name;
    if (fs::exists(p + "/theme.xml")) return p;
  }
  return {};
}

}  // namespace

int main() {
  const std::string light = find_base("theme-light"), dark = find_base("theme-dark");
  if (light.empty() || dark.empty()) {
    std::printf("test_cover_fit: no theme folders: skipped\n");
    return 0;
  }
  char tmpl[] = "/tmp/zf-coverfit-XXXXXX";
  const std::string root = mkdtemp(tmpl);
  const std::string es = root + "/es-theme-xmb-fcamod";
  fs::create_directories(es);
  std::ofstream(es + "/theme.xml") << "<theme><view name=\"system\"><carousel name=\"systemcarousel\"><type>horizontal</type></carousel></view></theme>";
  estheme::Options opt;
  estheme::Generated gen;
  std::string err;
  CHECK(estheme::generate(es, estheme::Bases{light, dark}, root + "/gen", opt, gen, err));
  Theme th;
  CHECK(Theme::load(gen.dir, ratio_name(720, 720), th, err));
  Assets a;
  CHECK(a.load_font(th.path(th.str("font")), err));
  Common common;
  CHECK(load_common(th, a, common, err));

  // a portrait cover, 300 x 400 (3:4), with noise so that it is a real picture
  const std::string cover = root + "/cover.png";
  {
    Canvas c;
    c.w = 300;
    c.h = 400;
    c.px.assign(size_t(c.w) * c.h * 4, 255);
    for (uint32_t y = 0; y < c.h; ++y)
      for (uint32_t x = 0; x < c.w; ++x) c.px[(size_t(y) * c.w + x) * 4 + 1] = uint8_t((x * 7 + y * 3) % 256);
    CHECK(save_png(c, cover, err));
  }
  // the key the themes bake a cover with has no crop flag, and decoding it keeps the shape
  {
    const std::string key = remake::cover_key(cover, 6);
    CHECK(key.find(std::string(kBakeMark) + "r6") != std::string::npos);
    CHECK(key.find(std::string(kBakeMark) + "c") == std::string::npos);
    Img im;
    CHECK(Assets::decode_fit(key, 100, 75, im));
    CHECK(im.w <= 100 && im.h <= 75);
    CHECK(std::fabs(float(im.w) / float(im.h) - 0.75f) < 0.05f);  // 3:4 stays 3:4 in a 4:3 box
  }

  std::vector<std::string> names;
  for (int i = 0; i < 20; ++i) names.push_back("Game number " + std::to_string(i));
  remake::state().inline_decode = true;
  remake::state().system_theme = "snes";
  remake::state().row_image = [cover](size_t) { return cover; };  // every row has this picture
  std::vector<Op> ops;
  list_view(th, a, common, 720, 720, "Super Nintendo", names, 5, Status::demo("12:04"), nullptr, nullptr, ops);
  remake::state().row_image = nullptr;
  int portrait = 0;
  for (const Op& o : ops) {
    if (o.kind != Op::Image || o.id >= a.count()) continue;
    const Img& im = a.img(o.id);
    if (im.h < im.w * 6 / 5) continue;  // text, discs, shadows, icons: not a cover
    ++portrait;
    CHECK(std::fabs(o.w / o.h - 0.75f) < 0.06f);  // drawn as it is, not cut to the shape of the tile
  }
  CHECK(portrait >= 3);  // the thumbnails of the rows above and below the chosen one
  CHECK(remake::state().rows_shown >= 3);  // the view tells how many rows it drew (L / R page by it)

  std::error_code ec;
  fs::remove_all(root, ec);
  return zt::done();
}
