// A long system name must not run over its neighbours: under the carousel it ends before the game count, in the list header before the
// wifi / battery / clock stack; a name that fits is drawn at its natural width. Needs the merged Zenith theme folder (skipped without).
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

const std::string kLong =
    "Sistema con un nombre completo extremadamente largo que de ninguna manera cabe en la pantalla de una consola de bolsillo";

void check_theme(const std::string& dir, const char* tag) {
  const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
  for (const auto& wh : sizes) {
    Theme th;
    Assets as;
    std::string err;
    const float w = float(wh[0]), h = float(wh[1]);
    CHECK(Theme::load(dir, ratio_name(wh[0], wh[1]), th, err));
    CHECK(as.load_font(th.path(th.str("font")), err));
    Common c;
    CHECK(load_common(th, as, c, err));
    Status st = Status::demo("12:04");
    char what[64];
    std::snprintf(what, sizeof what, "%s %ux%u", tag, wh[0], wh[1]);

    // ---- under the carousel: [bar rect][name][count][hairline]
    for (bool long_name : {true, false}) {
      std::vector<SystemItem> items = {{"gbc", long_name ? kLong : std::string("Game Boy Color"), 14}};
      std::vector<Op> ops;
      system_view(th, as, c, w, h, items, 0, 0.0f, st, nullptr, InfoFade{}, ops);
      const Op *name = nullptr, *info = nullptr;
      for (size_t i = 0; i + 3 < ops.size(); ++i)
        if (ops[i].kind == Op::Rect && ops[i + 1].kind == Op::Image && ops[i + 2].kind == Op::Image && ops[i + 3].kind == Op::Rect &&
            ops[i + 3].w > 0.8f * w) {
          name = &ops[i + 1];
          info = &ops[i + 2];
          break;
        }
      CHECK(name && info);
      if (!name || !info) {
        std::fprintf(stderr, "  (%s, long=%d): no name/count ops found\n", what, int(long_name));
        continue;
      }
      if (long_name) {
        CHECK(name->x + name->w <= info->x - 1.0f);  // ends before the count
      } else {
        const float px = std::round(th.f("fSys") * h * 1.75f);
        CHECK(std::abs(name->w - as.measure("Game Boy Color", px)) <= 2.0f);  // fits: natural width, no ellipsis
      }
    }

    // ---- list header: avatar, title, status stack
    std::vector<Op> stack;
    status_bar(th, as, w, h, st, stack);
    float stack_left = w;
    for (const Op& o : stack)
      if (o.kind == Op::Image && o.w > 0) stack_left = std::min(stack_left, o.x);
    CHECK(stack_left < w);
    for (bool long_name : {true, false}) {
      std::vector<Op> ops;
      std::vector<std::string> names = {"A game"};
      list_view(th, as, c, w, h, long_name ? kLong : std::string("Game Boy Color"), names, 0, st, nullptr, nullptr, ops);
      const Op* title = nullptr;
      for (const Op& o : ops)
        if (o.kind == Op::Image && o.x == th.f("nameX") * w) {
          title = &o;
          break;
        }
      CHECK(title != nullptr);
      if (!title) {
        std::fprintf(stderr, "  (%s, long=%d): no title op found\n", what, int(long_name));
        continue;
      }
      if (long_name) {
        CHECK(title->x + title->w <= stack_left - 1.0f);
      } else {
        CHECK(std::abs(title->w - as.measure("Game Boy Color", std::floor(th.f("fTitle") * h))) <= 2.0f);
      }
    }
  }
}

}  // namespace

int main() {
  bool any = false;
  for (const char* t : {"theme-dark", "theme-light"}) {
    std::string dir = std::string(ZF_LOCALE_BUILD) + "/../themes/" + t;
    if (!is_dir(dir)) dir = std::string("/home/epsil/zenith-frontend/") + t;
    if (!is_dir(dir)) continue;
    any = true;
    check_theme(dir, t);
  }
  if (!any) std::printf("[titles] no theme folder: skipped\n");
  return zt::done();
}
