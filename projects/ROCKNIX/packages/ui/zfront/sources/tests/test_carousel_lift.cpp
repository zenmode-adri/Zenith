// 4:3 carousel lift: at 640x480 / 800x600 the carousel and the text under it sit higher so the "continue playing" row fits; every
// other ratio keeps the theme's positions and its old behaviour (the row on 720x720, no row on 480x320 and 720x405).
// Needs the merged Zenith theme folder (built next to the tests, or the checkout's); skipped when there is none.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "gfx.hpp"
#include "scene.hpp"
#include "status.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;

namespace {

struct Run {
  float frame_y = 0;       // top of the selected tile's frame image (the last frame op, drawn on top)
  float frame_h = 0;
  uint32_t box_w = 0, box_h = 0;
  float hair_y = -1;       // top of the one-pixel-ish hairline under the name: the widest thin rect
  float bottom = 0;        // lowest pixel any op of the carousel scene draws, help pill excluded (it is above the row anyway)
};

Run run(const std::string& theme_dir, unsigned w, unsigned h, bool with_row) {
  Run r;
  Theme th;
  Assets as;
  std::string err;
  CHECK(Theme::load(theme_dir, ratio_name(w, h), th, err));
  CHECK(as.load_font(th.path(th.str("font")), err));
  Common c;
  CHECK(load_common(th, as, c, err));
  std::vector<SystemItem> items = {{"tools", "Tools", 3}, {"gbc", "Game Boy Color", 14}, {"psp", "PlayStation Portable", 2}};
  RecentRow row;
  row.decode_inline = true;
  if (with_row)
    for (const char* n : {"One", "Two", "Three"}) row.items.push_back({std::string(ZF_TEST_DATA) + "/cover-box.png", n, "5 min ago"});
  std::vector<Op> ops;
  system_view(th, as, c, float(w), float(h), items, 1, 1.0f, Status::demo("12:04"), with_row ? &row : nullptr, InfoFade{}, ops);
  r.box_w = row.box_w;
  r.box_h = row.box_h;
  for (const Op& op : ops) {
    if (op.kind == Op::Image && op.id == c.frame) r.frame_y = op.y, r.frame_h = op.h;
    if (op.kind == Op::Rect && op.w > float(w) * 0.8f && op.h <= std::max(1.0f, std::round(float(h) / 720.0f))) r.hair_y = op.y;
  }
  return r;
}

}  // namespace

int main() {
  std::string dir = std::string(ZF_LOCALE_BUILD) + "/../themes/theme-dark";
  if (!is_dir(dir)) dir = "/home/epsil/zenith-frontend/theme-dark";
  if (!is_dir(dir)) {
    std::printf("[carousel_lift] no theme folder: skipped\n");
    return zt::done();
  }
  struct S {
    unsigned w, h;
    bool lifted, row;
  } sizes[] = {{640, 480, true, true}, {800, 600, true, true}, {720, 720, false, true}, {480, 320, false, false}, {720, 405, false, false}};
  for (const S& s : sizes) {
    Theme th;
    std::string err;
    CHECK(Theme::load(dir, ratio_name(s.w, s.h), th, err));
    const float h = float(s.h), w = float(s.w);
    const float lift = s.lifted ? std::round(0.09f * h) : 0.0f;
    Run with = run(dir, s.w, s.h, true), without = run(dir, s.w, s.h, false);
    // the selected tile (1.5x the container, centred on carY) moved up by `lift` and not by anything else
    const float cy = with.frame_y + with.frame_h / 2.0f;
    std::printf("[carousel_lift] %ux%u: centre %.1f (theme %.1f), row box %ux%u\n", s.w, s.h, cy, th.f("carY") * h, with.box_w, with.box_h);
    CHECK(std::fabs(cy - (th.f("carY") * h - lift)) < 0.01f);
    // the lift never depends on the row: same place with and without savestates, so the carousel does not jump between systems
    CHECK(with.frame_y == without.frame_y);
    CHECK(with.hair_y == without.hair_y);
    // the row fits exactly where it is meant to
    CHECK_EQ(with.box_w > 0, s.row);
    CHECK_EQ(without.box_w, uint32_t(0));
    if (s.row) CHECK(float(with.box_h) >= std::round(0.085f * h) - 2.0f);  // never squeezed below the row's own minimum
    // the card (container = cellW * 1.23) clears the status bar
    const float card_top = cy - th.f("cellW") * w * 1.23f / 2.0f;
    CHECK(card_top > (th.f("stackY") + th.f("stackH")) * h + 0.04f * h);
  }
  // 720x720 keeps its old row exactly (its tile is limited by the room under the hairline, not by 0.125 of the height)
  Run sq = run(dir, 720, 720, true);
  CHECK_EQ(sq.box_w, uint32_t(117));
  CHECK_EQ(sq.box_h, uint32_t(65));
  return zt::done();
}
