// `menu.home`: the in-game HOME panel (homemenu.hpp). It only exists inside the `zfront --home-panel` child that the frontend starts
// while a game is paused; its answer to the parent is the exit code (`homemenu::set_panel_result` + `exit_code_of`).
//
//   BATTERY          (read only)
//   SCREEN BRIGHTNESS  slider, same sysfs write and `system.brightness` setting as the SCREEN menu
//   SYSTEM VOLUME      slider, the `volume` script through `sound::set_volume`
//   SAVE STATE / LOAD STATE   only with argument "states" (RetroArch with network_cmd_enable on; the parent decides)
//   RESUME             B does the same
//   QUIT GAME          asks first; the parent resumes the game and then runs the existing emukill path
//
// Every way out ends the child (`Transition::quit()`): the parent resumes the paused game whatever happens, so a crash here is harmless.
#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>

#include "homemenu.hpp"
#include "menu.hpp"
#include "menu_lang.hpp"
#include "menu_sound.hpp"
#include "menu_system.hpp"
#include "registry.hpp"
#include "status.hpp"
#include "util.hpp"

namespace zf {

namespace {

struct Backlight {
  std::string path;
  int max = 0, pct = 100;
};

Backlight first_backlight() {
  Backlight b;
  if (DIR* d = ::opendir("/sys/class/backlight")) {
    std::vector<std::string> names;
    while (const dirent* e = ::readdir(d))
      if (e->d_name[0] != '.') names.emplace_back(e->d_name);
    ::closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
      const std::string dir = "/sys/class/backlight/" + n;
      std::string mx, cur;
      long max = 0, c = 0;
      if (!read_file(dir + "/max_brightness", mx) || !parse_int(trim(mx), max) || max <= 0) continue;
      if (read_file(dir + "/brightness", cur)) parse_int(trim(cur), c);
      b.path = dir + "/brightness";
      b.max = int(max);
      b.pct = std::min(100, std::max(5, int(float(c) / float(max) * 100.0f + 0.5f)));
      break;
    }
  }
  return b;
}

class HomeMenu : public Menu {
 public:
  explicit HomeMenu(MenuSpec spec) : Menu(std::move(spec)) {}
  Transition handle(Action a, Ctx& cx) override {
    Transition t = Transition::none();
    if (a == Action::B) {
      homemenu::set_panel_result(homemenu::PanelResult::Resume);
      t = Transition::quit();
    } else {
      t = Menu::handle(a, cx);
    }
    if (t.kind == Transition::Quit) {
      on_dismiss(cx);  // flushes a slider commit (brightness setting) that has not been written yet
      // the volume script runs on the job thread: give it a moment to finish before this process ends
      for (int i = 0; i < 100 && !cx.sv->jobs.idle(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        cx.sv->jobs.drain();
      }
    }
    return t;
  }
};

Transition answer(homemenu::PanelResult r) {
  homemenu::set_panel_result(r);
  return Transition::quit();
}

std::unique_ptr<Screen> make_home(bool states) {
  MenuSpec s;
  s.title = tr("HOME MENU");
  s.title_key = N_("HOME MENU");
  s.build = [states](MenuBuilder& m, Ctx& cx) {
    if (std::optional<Battery> b = read_battery("/sys"))
      m.info(tr("BATTERY"), std::to_string(int(b->percent)) + "%" + (b->charging ? std::string(" (") + tr("CHARGING") + ")" : std::string()));
    auto bl = std::make_shared<Backlight>(first_backlight());
    if (!bl->path.empty()) {
      m.slider(tr("SCREEN BRIGHTNESS"), 5, 100, 1, [bl] { return bl->pct; }, [&cx, bl](int v) {
        bl->pct = v;
        msys::sysfs_write(cx, bl->path, std::to_string(v * bl->max / 100));  // like the OS script `brightness set`
      }).unit("%").commit([&cx](int v) { msys::put(cx, "system.brightness", std::to_string(v), true); });
    }
    m.slider(tr("SYSTEM VOLUME"), 0, 100, 1, [&cx] { return sound::volume(cx); }, [&cx](int v) { sound::set_volume(cx, v); }).unit("%");
    m.separator();
    if (states) {
      m.action(tr("SAVE STATE"), [](Ctx&) { return answer(homemenu::PanelResult::SaveState); }).confirm(tr("OVERWRITE THE SAVE STATE?"));
      m.action(tr("LOAD STATE"), [](Ctx&) { return answer(homemenu::PanelResult::LoadState); }).confirm(tr("LOAD THE SAVE STATE? UNSAVED PROGRESS IS LOST."));
    }
    m.action(tr("RESUME"), [](Ctx&) { return answer(homemenu::PanelResult::Resume); });
    m.action(tr("QUIT GAME"), [](Ctx&) { return answer(homemenu::PanelResult::Quit); }).confirm(tr("REALLY QUIT THE GAME?"));
  };
  return std::make_unique<HomeMenu>(std::move(s));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.home", [](Ctx&, const std::string& arg) { return make_home(arg == "states"); });

}  // namespace zf
