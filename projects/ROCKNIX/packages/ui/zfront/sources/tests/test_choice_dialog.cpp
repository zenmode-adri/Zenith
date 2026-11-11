// A choice dialog with hundreds of options (249 countries, ~400 time zones) makes pictures only for the rows in view, not one per option on
// its first frame. Needs the Zenith theme folder (ZF_THEME_DIR); skipped without it.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "theme.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

using namespace zf;

namespace {

std::string theme_dir() {
  const char* e = std::getenv("ZF_THEME_DIR");
  return e ? e : "/usr/share/zfront/theme-dark";
}

// Pictures in the assets after one frame of a dialog with `n` options (the cursor on option `at`); 0 when there is no theme.
size_t images_after_draw(size_t n, size_t at) {
  Theme th;
  std::string err;
  Ctx cx;
  if (!Theme::load(theme_dir(), "1/1", th, err) || !cx.assets.load_font(th.path(th.str("font")), err)) return 0;
  cx.th = th;
  cx.w = cx.h = 720;
  std::vector<Option> opts;
  for (size_t i = 0; i < n; ++i) opts.push_back({"v" + std::to_string(i), "Option number " + std::to_string(i) + (i == 7 ? " with a much longer name than the others" : "")});
  ui::ChoiceDialog d("PICK ONE", opts, "v" + std::to_string(at), [](Ctx&, const std::string&) {});
  std::vector<Op> ops;
  d.draw(cx, ops);
  return cx.assets.count();
}

}  // namespace

int main() {
  Theme probe;
  Assets pa;
  std::string err;
  if (!Theme::load(theme_dir(), "1/1", probe, err) || !pa.load_font(probe.path(probe.str("font")), err)) {
    std::fprintf(stderr, "test_choice_dialog: theme not found (%s): skipped\n", err.c_str());
    return 0;
  }
  const size_t few = images_after_draw(8, 0);
  const size_t many = images_after_draw(400, 0);
  const size_t middle = images_after_draw(400, 200);
  CHECK(few > 0);
  // a screenful of rows more than the small one, not 400 pictures
  CHECK(many <= few + 40);
  CHECK(middle <= few + 40);
  return zt::done();
}
