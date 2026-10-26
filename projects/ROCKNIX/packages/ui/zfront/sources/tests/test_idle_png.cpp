// Review pictures of the screensaver: runs only when $ZF_IDLE_PNG names an output directory (it does nothing under ctest).
// Both themes at the four screen sizes: the carousel and an open menu, normal / dim / black, and the "screensaver type" row.
//   ZF_IDLE_PNG=/home/epsil/zf-out/idle-firstrun ZF_THEME_DIR=/home/epsil/zenith-frontend ./test_idle_png
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "cpu.hpp"
#include "menu.hpp"
#include "registry.hpp"
#include "screens.hpp"
#include "screensaver.hpp"
#include "status.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

struct Shooter {
  std::string dir, theme_name;
  unsigned w = 0, h = 0;
  int n = 0;
  void shot(Ctx& cx, const char* name, Screen* base, screensaver::Mode mode) {
    std::vector<Op> ops;
    screensaver::set_mode(mode);
    if (mode != screensaver::Mode::Black) base->draw(cx, ops);  // black: App::render skips the screens
    screensaver::draw(cx, ops);
    screensaver::set_mode(screensaver::Mode::Off);
    Canvas cv = cpu_render(ops, cx.assets, w, h);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s/%02d-%s-%s-%ux%u.png", dir.c_str(), n++, name, theme_name.c_str(), w, h);
    std::string err;
    CHECK(save_png(cv, buf, err));
  }
};

void run_all(const std::string& theme_dir, const std::string& theme_name, unsigned w, unsigned h, const std::string& outdir, const std::string& fixture) {
  Ctx& cx = *new Ctx;  // leaked on purpose: framework caches key on the Assets address, which must never repeat
  std::string err;
  cx.w = float(w);
  cx.h = float(h);
  CHECK(Theme::load(theme_dir, ratio_name(w, h), cx.th, err));
  CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
  CHECK(load_common(cx.th, cx.assets, cx.common, err));
  CHECK(load_systems_index(fixture + "/es_systems.cfg", fixture + "/roms", cx.systems, err));
  for (const System& s : cx.systems) cx.items.push_back(SystemItem{s.theme, s.fullname, uint32_t(s.count)});
  cx.status = Status::demo("12:04");
  cx.sv->cmd.dry_run = true;
  cx.conf = SystemConf::load(fixture + "/system.cfg");
  cx.es = EsSettings::from_text("<?xml version=\"1.0\"?>\n<config>\n<string name=\"ScreenSaverBehavior\" value=\"dim\" />\n</config>\n", fixture + "/es_settings.cfg");

  Shooter sh;
  sh.dir = outdir;
  sh.theme_name = theme_name;
  sh.w = w;
  sh.h = h;
  {  // the carousel under the screensaver
    CarouselScreen car(2);
    for (int i = 0; i < 6; ++i) car.tick(1.0f, cx);
    sh.shot(cx, "carousel-normal", &car, screensaver::Mode::Off);
    sh.shot(cx, "carousel-dim", &car, screensaver::Mode::Dim);
    sh.shot(cx, "carousel-black", &car, screensaver::Mode::Black);
  }
  {  // a menu under dim: what the screen looks like when the screensaver starts with the menu open
    auto s = Registry::instance().make("menu.main", cx, "");
    if (s) {
      s->tick(0.0f, cx);
      for (int i = 0; i < 6; ++i) s->tick(1.0f, cx);
      sh.shot(cx, "menu-normal", s.get(), screensaver::Mode::Off);
      sh.shot(cx, "menu-dim", s.get(), screensaver::Mode::Dim);
    }
  }
  {  // the setting
    auto s = Registry::instance().make("menu.ui", cx, "");
    if (s) {
      Menu& m = *static_cast<Menu*>(s.get());
      m.tick(0.0f, cx);
      int idx = -1;
      for (size_t i = 0; i < m.size(); ++i)
        if (m.entry(i).label == std::string(tr("SCREENSAVER TYPE"))) idx = int(i);
      for (size_t k = 0; idx >= 0 && k <= m.size() && m.cursor() != idx; ++k) m.handle(Action::Down, cx);
      for (int i = 0; i < 6; ++i) m.tick(1.0f, cx);
      sh.shot(cx, "setting-type", s.get(), screensaver::Mode::Off);
    }
  }
}

}  // namespace

int main() {
  const char* out = std::getenv("ZF_IDLE_PNG");
  if (!out) return 0;
  const char* root = std::getenv("ZF_THEME_DIR");
  std::string base = root ? root : "/home/epsil/zenith-frontend";
  fs::create_directories(out);
  char tpl[] = "/tmp/zf-idlepng-XXXXXX";
  std::string fx = ::mkdtemp(tpl);
  if (std::system(("sh " ZF_TEST_DATA "/../make_fixture.sh " + fx + " >/dev/null").c_str()) != 0) return 1;
  const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
  for (const char* theme : {"theme-dark", "theme-light"})
    for (const auto& wh : sizes) run_all(base + "/" + theme, theme + 6, wh[0], wh[1], out, fx);
  std::error_code ec;
  fs::remove_all(fx, ec);
  return zt::done();
}
