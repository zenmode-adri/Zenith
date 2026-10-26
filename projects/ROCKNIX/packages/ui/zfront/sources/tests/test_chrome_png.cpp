// Review pictures of the menu chrome: title, header hairline, the level OSD and a toast over menus with long titles, and a
// list scrolled so that a section header sits on the edge of the viewport. Runs only when $ZF_CHROME_PNG names an output
// directory (it does nothing under ctest):
//   ZF_CHROME_PNG=/home/epsil/zf-out/debts2 ZF_THEME_DIR=/home/epsil/zenith-frontend ./test_chrome_png
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "cpu.hpp"
#include "menu.hpp"
#include "ui_osd.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

std::unique_ptr<Menu> make(const char* title, bool many) {
  MenuSpec s;
  s.title = title;
  s.build = [many](MenuBuilder& m, Ctx&) {
    for (int sec = 0; sec < (many ? 4 : 1); ++sec) {
      m.header("Section " + std::to_string(sec + 1));
      for (int i = 0; i < (many ? 5 : 3); ++i) m.action("Entry " + std::to_string(sec * 10 + i + 1), [] {});
    }
  };
  return std::make_unique<Menu>(std::move(s));
}

void shot(const std::string& dir, const std::string& theme_dir, const char* theme, unsigned w, unsigned h, const char* tag, const char* title, int downs, bool osd, bool toast) {
  Ctx cx;
  std::string err;
  cx.w = float(w);
  cx.h = float(h);
  cx.sv->cmd.dry_run = true;
  CHECK(Theme::load(theme_dir + "/" + theme, ratio_name(w, h), cx.th, err));
  CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
  cx.status = Status::demo("12:04");
  auto m = make(title, downs > 0);
  m->handle(Action::Left, cx);
  for (int i = 0; i < downs; ++i) {
    m->handle(Action::Down, cx);
    for (int k = 0; k < 12; ++k) m->tick(1.0f, cx);
  }
  osd_clear();
  if (osd) osd_show_level(cx, "volume", 0.62f, 60000);
  if (toast) cx.sv->toasts.push("Pad connected", 60000);
  std::vector<Op> ops;
  m->draw(cx, ops);
  draw_toasts(cx, ops);
  Canvas cv = cpu_render(ops, cx.assets, w, h);
  char name[256];
  std::snprintf(name, sizeof name, "%s/%s-%s-%ux%u.png", dir.c_str(), tag, theme, w, h);
  CHECK(save_png(cv, name, err));
}

}  // namespace

int main() {
  const char* out = std::getenv("ZF_CHROME_PNG");
  const char* root = std::getenv("ZF_THEME_DIR");
  if (!out || !root) return zt::done();
  fs::create_directories(out);
  const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
  for (const char* theme : {"theme-dark", "theme-light"})
    for (const auto& wh : sizes) {
      shot(out, root, theme, wh[0], wh[1], "osd-long", "Controller & Bluetooth settings", 0, true, false);
      shot(out, root, theme, wh[0], wh[1], "osd-toast", "User interface settings", 0, true, true);
      shot(out, root, theme, wh[0], wh[1], "osd-short", "Sound", 0, true, false);
      for (int d : {5, 6, 7, 11, 12}) {
        char tag[32];
        std::snprintf(tag, sizeof tag, "scroll%02d", d);
        shot(out, root, theme, wh[0], wh[1], tag, "Widget demo", d, false, false);
      }
    }
  return zt::done();
}
