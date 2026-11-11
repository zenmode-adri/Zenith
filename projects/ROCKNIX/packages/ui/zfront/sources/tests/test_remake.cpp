// The four remade themes on every screen of the consolas: the automatic grid keeps the text of the game list inside the screen and out of the help bar, with long
// names, with and without data; and only those four ES themes are remade. Needs the merged Zenith folder (as test_carousel_lift); skipped when there is none.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
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
  CHECK_EQ(estheme::remade_style("/x/es-theme-xmb-fcamod"), std::string("xmb"));
  CHECK_EQ(estheme::remade_style("/x/es-theme-art-book-next"), std::string("artbook"));
  CHECK_EQ(estheme::remade_style("/x/es-theme-iconic"), std::string("iconic"));
  CHECK_EQ(estheme::remade_style("/x/es-theme-techdweeb"), std::string("techdweeb"));
  CHECK_EQ(estheme::remade_style("/x/es-theme-other"), std::string());

  {  // the grid: a size of the design scales with the short side, text never under its floor
    const remake::Grid small(480, 320), base(640, 480), big(1280, 720);
    CHECK_EQ(base.P(24), 24);
    CHECK(small.P(24) < 24);
    CHECK(big.P(24) > 24);
    CHECK_EQ(small.F(14, 12), 12);
    CHECK_EQ(base.F(14, 12), 14);
    CHECK(small.help_top() < small.help_y());
  }
  // Next's count under the chosen tile: tools and pictures show the bare number, a collection holds games like a system
  CHECK_EQ(remake::zen_count("gbc", 1), std::string("1 game"));
  CHECK_EQ(remake::zen_count("tools", 3), std::string("3"));
  CHECK_EQ(remake::zen_count("imageviewer", 12), std::string("12"));
  CHECK_EQ(remake::zen_count("auto-allgames", 7), std::string("7 games"));
  CHECK_EQ(remake::zen_count("auto-lastplayed", 4), std::string("4 games"));
  CHECK_EQ(remake::zen_count("custom-collections", 2), std::string("2 games"));

  const std::string light = find_base("theme-light"), dark = find_base("theme-dark");
  if (light.empty() || dark.empty()) {
    std::printf("sin carpetas del tema: se omite la parte de dibujo\n");
    return zt::done();
  }
  char tmpl[] = "/tmp/zf-test-remake-XXXXXX";
  const std::string root = mkdtemp(tmpl);
  const std::vector<std::string> names = {"A game", "Another game", "The Legend of a very long name that does not fit in any row of any screen at all, not even a big one", "Short", "Game five", "Game six", "Game seven", "Game eight", "Game nine",
                                          "Game ten", "Game eleven", "Game twelve", "Game thirteen", "Game fourteen", "Game fifteen", "Game sixteen", "Game seventeen", "Game eighteen", "Game nineteen", "Game twenty"};
  const int sizes[][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 540}, {720, 720}, {800, 480}, {854, 480}, {1024, 768}, {1280, 720}};
  for (const char* folder : {"es-theme-xmb-fcamod", "es-theme-art-book-next", "es-theme-iconic", "es-theme-techdweeb"}) {
    const std::string es = root + "/" + folder;
    fs::create_directories(es);
    std::ofstream(es + "/theme.xml") << "<theme><view name=\"system\"><carousel name=\"systemcarousel\"><type>horizontal</type></carousel></view></theme>";
    estheme::Options opt;
    estheme::Generated g;
    std::string err;
    CHECK(estheme::generate(es, estheme::Bases{light, dark}, root + "/gen", opt, g, err));
    for (const auto& sz : sizes) {
      Theme th;
      CHECK(Theme::load(g.dir, ratio_name(unsigned(sz[0]), unsigned(sz[1])), th, err));
      CHECK(remake::active(th));
      Assets a;
      CHECK(a.load_font(th.path(th.str("font")), err));
      const float w = float(sz[0]), h = float(sz[1]);
      const remake::Grid grid(w, h);
      Status st = Status::demo("12:04");
      DetailedData d;
      d.name = names[2];
      d.year = 1994;
      d.genre = "A role playing game with a long genre";
      d.players = "1-2";
      d.rating = 0.7f;
      d.desc.assign(4000, 'x');
      for (size_t i = 8; i < d.desc.size(); i += 9) d.desc[i] = ' ';
      d.desc_time = -1;
      const DetailedData* const details[] = {&d, nullptr};
      for (const DetailedData* detail : details) {
        for (size_t cur : {size_t(0), size_t(2), size_t(19)}) {
          std::vector<Op> ops;
          remake::state().system_theme = "snes";
          remake::list_view(th, a, w, h, "Super Nintendo", names, cur, st, detail, ops);
          CHECK(!ops.empty());
          bool clipped = false;
          for (const Op& op : ops) {
            if (op.kind == Op::Clip) clipped = true;
            if (op.kind == Op::Unclip) clipped = false;
            if (op.kind != Op::Image || clipped) continue;
            // every text and picture of the list stays on the screen (the soft shadows may spread out of it, they carry a tint with little alpha)
            if (op.color[3] < 255) continue;
            CHECK(op.x >= -1.0f);
            CHECK(op.x + op.w <= w + 1.0f);
            CHECK(op.y >= -1.0f);
            CHECK(op.y + op.h <= h + 1.0f);
          }
        }
      }
      if (remake::kind_of(th) != remake::Kind::Xmb) {  // the description never reaches the help bar
        std::vector<Op> ops;
        remake::list_view(th, a, w, h, "Super Nintendo", names, 2, st, &d, ops);
        for (size_t i = 0; i + 1 < ops.size(); ++i)
          if (ops[i].kind == Op::Clip) CHECK(ops[i].y + ops[i].h <= float(grid.help_top()) + 1.0f);
      }
    }
  }
  std::error_code ec;
  fs::remove_all(root, ec);
  return zt::done();
}
