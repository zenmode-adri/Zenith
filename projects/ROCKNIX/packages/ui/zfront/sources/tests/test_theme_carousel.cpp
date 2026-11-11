// Optional theme variables that change the structure of the home carousel (carScale, carSpacing, carCount, carTileScale, typeScale,
// sysBar) and the case of the list texts (listCase): without them the scene is exactly Zenith's, with them the geometry moves, and
// absurd values are clamped. Needs the merged Zenith theme folder (as test_carousel_lift); skipped when there is none.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "gfx.hpp"
#include "scene.hpp"
#include "status.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

/// A copy of the Zenith folder with `extra` (variables) added to its theme.xml.
std::string make_theme(const std::string& base, const std::string& tag, const std::string& extra) {
  const fs::path dst = fs::temp_directory_path() / ("zf-test-carousel-" + tag);
  std::error_code ec;
  fs::remove_all(dst, ec);
  fs::copy(base, dst, fs::copy_options::recursive, ec);
  std::string xml = slurp(dst / "theme.xml");
  const size_t at = xml.find("<variables>");
  CHECK(at != std::string::npos);
  xml.insert(at + 11, extra);
  std::ofstream(dst / "theme.xml", std::ios::binary) << xml;
  return dst.string();
}

struct Scene {
  std::vector<Op> ops;
  Common c;
  std::vector<float> cx;  // centre x of the tile images, in drawing order (the selected one is last)
  std::vector<float> tw;  // their widths
  float hair_y = -1;
  size_t thin_rects = 0;  // small filled rects (the bar before the name)
  float name_x = 1e9f;    // left edge of the system's name
};

bool same(const std::vector<Op>& a, const std::vector<Op>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].kind != b[i].kind || a[i].id != b[i].id || a[i].x != b[i].x || a[i].y != b[i].y || a[i].w != b[i].w || a[i].h != b[i].h ||
        a[i].color != b[i].color)
      return false;
  return true;
}

Scene home(const std::string& dir, unsigned w, unsigned h) {
  Scene s;
  Theme th;
  Assets as;
  std::string err;
  CHECK(Theme::load(dir, ratio_name(w, h), th, err));
  CHECK(as.load_font(th.path(th.str("font")), err));
  CHECK(load_common(th, as, s.c, err));
  std::vector<SystemItem> items;
  for (const char* n : {"gb", "gbc", "gba", "nes", "snes", "psx", "psp", "mame"}) items.push_back({n, n, 7});
  system_view(th, as, s.c, float(w), float(h), items, 3, 3.0f, Status::demo("12:04"), nullptr, InfoFade{}, s.ops);
  for (const Op& op : s.ops) {
    if (op.kind == Op::Image && op.id == s.c.tile) {
      s.cx.push_back(op.x + op.w / 2.0f);
      s.tw.push_back(op.w);
    }
    if (op.kind == Op::Rect && op.w > float(w) * 0.8f && op.h <= std::max(1.0f, std::round(float(h) / 720.0f))) s.hair_y = op.y;
    if (op.kind == Op::Rect && op.w < float(w) * 0.05f) ++s.thin_rects;
  }
  // the system's name: the left-most image drawn above the hairline and below the carousel
  for (const Op& op : s.ops)
    if (op.kind == Op::Image && op.y > s.hair_y - 0.12f * float(h) && op.y < s.hair_y && op.x < float(w) * 0.5f) s.name_x = std::min(s.name_x, op.x);
  return s;
}

/// distance between the selected card and its nearest right neighbour
float gap_right(const Scene& s) {
  const float sel = s.cx.back();
  float best = 1e9f;
  for (size_t i = 0; i + 1 < s.cx.size(); ++i)
    if (s.cx[i] > sel + 1.0f) best = std::min(best, s.cx[i] - sel);
  return best;
}

std::vector<Op> list(const std::string& dir) {
  Theme th;
  Assets as;
  Common c;
  std::string err;
  CHECK(Theme::load(dir, "1/1", th, err));
  CHECK(as.load_font(th.path(th.str("font")), err));
  CHECK(load_common(th, as, c, err));
  std::vector<Op> ops;
  list_view(th, as, c, 720, 720, "Game Boy", {"Zelda Link", "Metroid", "Tetris"}, 0, Status::demo("12:04"), nullptr, nullptr, ops);
  return ops;
}

}  // namespace

int main() {
  CHECK(utf8_lower("ZELDA \xC3\x91" "AND\xC3\x9A") == "zelda \xC3\xB1" "and\xC3\xBA");
  CHECK(utf8_lower(utf8_upper("abc xyz \xC3\xB1 \xC3\xA9")) == "abc xyz \xC3\xB1 \xC3\xA9");
  std::string dir = std::string(ZF_LOCALE_BUILD) + "/../themes/theme-dark";
  if (!is_dir(dir)) dir = "/usr/share/zfront/theme-dark";
  if (!is_dir(dir)) {
    std::printf("[theme_carousel] no theme folder: skipped\n");
    return zt::done();
  }
  struct S {
    unsigned w, h;
  } sizes[] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
  for (const S& sz : sizes) {
    const unsigned w = sz.w, h = sz.h;
    // the explicit Zenith values are exactly what a theme without them gets
    const std::string defs = make_theme(dir, "defs",
                                        "<carScale>1.23</carScale><carSpacing>1.21</carSpacing><carCount>5</carCount><carTileScale>1.5</carTileScale>"
                                        "<typeScale>1</typeScale><sysBar>1</sysBar><listCase></listCase>");
    const Scene base = home(dir, w, h);
    CHECK(same(base.ops, home(defs, w, h).ops));
    CHECK(base.cx.size() >= 5);
    CHECK(base.hair_y > 0);
    const float g0 = gap_right(base);

    // carScale 1 = the selected card is no bigger than the others
    const Scene flat = home(make_theme(dir, "scale", "<carScale>1</carScale>"), w, h);
    CHECK(std::fabs(flat.tw.back() - flat.tw.front()) < 0.01f);
    CHECK(base.tw.back() > base.tw.front() * 1.2f);
    CHECK(gap_right(flat) < g0);
    // out of range values are clamped (1.5 is the top)
    CHECK(same(home(make_theme(dir, "big", "<carScale>99</carScale>"), w, h).ops, home(make_theme(dir, "big2", "<carScale>1.5</carScale>"), w, h).ops));
    CHECK(same(home(make_theme(dir, "ti99", "<carTileScale>99</carTileScale>"), w, h).ops, home(make_theme(dir, "ti18", "<carTileScale>1.8</carTileScale>"), w, h).ops));

    // carSpacing: the gap around the selected card follows it
    CHECK(gap_right(home(make_theme(dir, "sp", "<carSpacing>1.0</carSpacing>"), w, h)) < g0);
    CHECK(gap_right(home(make_theme(dir, "sp2", "<carSpacing>1.6</carSpacing>"), w, h)) > g0);

    // carCount: fewer cards at once, further apart; more cards, closer
    const Scene three = home(make_theme(dir, "cnt", "<carCount>3</carCount>"), w, h);
    CHECK(three.cx.size() < base.cx.size());
    CHECK(three.cx.size() >= 3);
    CHECK(gap_right(three) > g0);
    const Scene seven = home(make_theme(dir, "cnt7", "<carCount>7</carCount>"), w, h);
    CHECK(seven.cx.size() >= base.cx.size());
    CHECK(gap_right(seven) < g0);
    CHECK(same(home(make_theme(dir, "c0", "<carCount>0</carCount>"), w, h).ops, home(make_theme(dir, "c1", "<carCount>1</carCount>"), w, h).ops));  // clamped to 1

    // carTileScale: only the card image changes size, in proportion
    const Scene small = home(make_theme(dir, "tile", "<carTileScale>1.2</carTileScale>"), w, h);
    CHECK(std::fabs(small.tw.back() / base.tw.back() - 1.2f / 1.5f) < 0.001f);
    CHECK(small.cx.size() == base.cx.size());
    CHECK(std::fabs(gap_right(small) - g0) < 0.01f);  // the pitch belongs to the container, not to the card

    // typeScale: bigger labels under the carousel push the hairline down
    CHECK(home(make_theme(dir, "type", "<typeScale>1.4</typeScale>"), w, h).hair_y > base.hair_y);
    CHECK(home(make_theme(dir, "ty", "<typeScale>0.7</typeScale>"), w, h).hair_y < base.hair_y);
    CHECK(same(home(make_theme(dir, "ty99", "<typeScale>99</typeScale>"), w, h).ops, home(make_theme(dir, "ty14", "<typeScale>1.4</typeScale>"), w, h).ops));

    // sysBar 0: no bar and the name starts at the margin
    const Scene nobar = home(make_theme(dir, "bar", "<sysBar>0</sysBar>"), w, h);
    CHECK_EQ(base.thin_rects, nobar.thin_rects + 1);
    CHECK(nobar.name_x < base.name_x);
  }
  // listCase: "" keeps each text as it is (rows in capitals), "upper" / "lower" force the case on all; a wrong value is ignored
  const std::vector<Op> l0 = list(dir), lu = list(make_theme(dir, "lu", "<listCase>upper</listCase>")), ll = list(make_theme(dir, "ll", "<listCase>lower</listCase>"));
  CHECK_EQ(l0.size(), lu.size());
  CHECK(!same(l0, lu));  // the header title ("tools") is forced to capitals by `upper`, not by ""
  CHECK(!same(lu, ll));
  CHECK(!same(l0, ll));
  CHECK(same(l0, list(make_theme(dir, "lx", "<listCase>rubbish</listCase>"))));
  return zt::done();
}
