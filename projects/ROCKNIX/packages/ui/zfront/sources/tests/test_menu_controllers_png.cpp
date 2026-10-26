// Review images of the controller section (every screen/state at the four resolutions, dark and light). Runs only with
//   ZF_CTL_PNG=/home/epsil/zf-out/menu-controllers [ZF_THEME_DIR=/home/epsil/zenith-frontend] ./test_menu_controllers_png
// Nothing is executed (dry-run) and all files live in a temp directory.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "cpu.hpp"
#include "menu.hpp"
#include "menu_controllers.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

using namespace zf;
using namespace zf::ctl;
namespace fs = std::filesystem;

namespace {

void put(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
}

const char* kGuid = "19009b4d4b4800000111000000010000";
const char* kName = "retrogame_joypad";

struct FakePad : PadCore {
  FakePad() : PadCore(PadConfig(), true) {}
  std::vector<JoyInfo> list;
  std::vector<JoyInfo> joysticks() const override { return list; }
};

Menu* menu_of(std::unique_ptr<Screen>& s) { return static_cast<Menu*>(s.get()); }

void settle(Screen& s, Ctx& cx) {
  for (int i = 0; i < 6; ++i) s.tick(1.0f, cx);
}

struct Shooter {
  std::string dir, theme_name;
  unsigned w = 0, h = 0;
  int n = 0;
  void shot(Ctx& cx, const char* name, Screen* base, Screen* over = nullptr, bool toasts = false) {
    if (std::getenv("ZF_CTL_TRACE")) std::fprintf(stderr, "[ctl-png]   %s\n", name);
    std::vector<Op> ops;
    base->draw(cx, ops);
    if (over) over->draw(cx, ops);
    if (toasts) draw_toasts(cx, ops);
    Canvas cv = cpu_render(ops, cx.assets, w, h);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s/%02d-%s-%s-%ux%u.png", dir.c_str(), n++, name, theme_name.c_str(), w, h);
    std::string err;
    CHECK(save_png(cv, buf, err));
  }
};

void press_release(ConfigScreen& s, uint16_t id) {
  std::string g = kGuid, n = kName;
  s.on_capture({0, InKind::Button, id, 1, &g, &n});
  s.on_capture({0, InKind::Button, id, 0, &g, &n});
}

void run_all(const std::string& theme_dir, const std::string& theme_name, unsigned w, unsigned h, const std::string& outdir) {
  std::string tmp = "/tmp/zf-ctl-png-" + std::to_string(::getpid());
  fs::remove_all(tmp);
  fs::create_directories(tmp + "/platform/rocknix-joypad");
  put(tmp + "/platform/rocknix-joypad/rumble_enable", "1\n");
  put(tmp + "/rocknix-bluetooth", "#!/bin/sh\n");
  put(tmp + "/system.cfg", "controllers.bluetooth.enabled=1\nrumble.enabled=1\n");
  put(tmp + "/es_input.cfg",
      std::string("<inputList><inputConfig type=\"joystick\" deviceName=\"") + kName + "\" deviceGUID=\"" + kGuid + "\"><input name=\"a\" type=\"button\" id=\"1\" value=\"1\"/></inputConfig></inputList>");
  paths().user_input = tmp + "/es_input.cfg";
  paths().bt_tool = tmp + "/rocknix-bluetooth";
  paths().bt_live_log = tmp + "/bt-live.log";
  paths().sysfs_platform = tmp + "/platform";
  SystemConf::lock_path() = tmp + "/.lock";

  Ctx& cx = *new Ctx;  // never freed on purpose: toast.cpp keys its text cache by the address of Assets
  std::string err;
  cx.w = float(w);
  cx.h = float(h);
  CHECK(Theme::load(theme_dir, ratio_name(w, h), cx.th, err));
  CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
  cx.sv->cmd.dry_run = true;
  cx.conf = SystemConf::load(tmp + "/system.cfg");
  cx.es = EsSettings::from_text("<?xml version=\"1.0\"?><config/>", tmp + "/es_settings.cfg");
  FakePad pad;
  pad.list.push_back({0, kGuid, kName, 18, 1, 4});
  pad.list.push_back({1, "0300bbbb5e040000fd02000030110000", "8BitDo Pro 2", 15, 1, 6});
  cx.pad = &pad;

  Shooter sh;
  sh.dir = outdir;
  sh.theme_name = theme_name;
  sh.w = w;
  sh.h = h;
  std::fprintf(stderr, "[ctl-png] %s %ux%u\n", theme_name.c_str(), w, h);

  // ---- main page
  {
    std::unique_ptr<Screen> sc = Registry::instance().make("menu.controllers", cx, "");
    Menu& m = *menu_of(sc);
    m.handle(Action::Left, cx);
    settle(m, cx);
    sh.shot(cx, "page", &m);
    // second page of the list: the cursor goes down to the pads and further
    for (int i = 0; i < 4; ++i) m.handle(Action::Down, cx);
    settle(m, cx);
    sh.shot(cx, "page-pads", &m);
    for (int i = 0; i < 7; ++i) m.handle(Action::Down, cx);
    settle(m, cx);
    sh.shot(cx, "page-end", &m);
    // the intro dialog of the mapping
  }
  {
    ui::ConfirmOpts opts;
    opts.yes = tr("OK");
    opts.no = tr("CANCEL");
    opts.default_yes = true;
    Transition intro = ui::confirm(tr("YOU ARE GOING TO MAP A CONTROLLER. MAP BASED ON THE BUTTON'S POSITION, NOT ITS PHYSICAL LABEL. IF YOU DO NOT HAVE A SPECIAL "
                                      "BUTTON FOR HOTKEY, USE THE SELECT BUTTON. SKIP ALL BUTTONS/STICKS YOU DO NOT HAVE BY HOLDING ANY BUTTON. PRESS THE SOUTH BUTTON "
                                      "TO CONFIRM WHEN DONE."),
                                   nullptr, opts);
    std::unique_ptr<Screen> base = Registry::instance().make("menu.controllers", cx, "");
    base->handle(Action::Left, cx);
    settle(*base, cx);
    sh.shot(cx, "intro", base.get(), intro.screen.get());
  }

  // ---- detect
  {
    DetectScreen d(cx);
    d.tick(0.01f, cx);
    sh.shot(cx, "detect", &d);
    std::string g = kGuid, n = kName;
    d.on_capture({0, InKind::Button, 4, 1, &g, &n});
    d.tick(0.55f, cx);
    sh.shot(cx, "detect-holding", &d);
  }

  // ---- the wizard
  {
    ConfigScreen s(cx, kGuid, kName, 18, 1, 4);
    s.tick(0.01f, cx);
    sh.shot(cx, "wiz-first", &s);
    press_release(s, 11);  // up
    press_release(s, 12);  // down
    press_release(s, 13);  // left
    sh.shot(cx, "wiz-right", &s);
    std::string g = kGuid, n = kName;
    s.on_capture({0, InKind::Button, 13, 1, &g, &n});  // taken
    s.on_capture({0, InKind::Button, 13, 0, &g, &n});
    s.tick(0.01f, cx);
    sh.shot(cx, "wiz-taken", &s);
    press_release(s, 14);  // right
    press_release(s, 9);   // start
    s.tick(0.01f, cx);
    sh.shot(cx, "wiz-select", &s);
    s.on_capture({0, InKind::Button, 8, 1, &g, &n});
    s.tick(0.5f, cx);
    sh.shot(cx, "wiz-hold-skip", &s);
    s.tick(0.6f, cx);  // skipped
    for (int i = 0; i < 4; ++i) press_release(s, uint16_t(0 + i));  // a b x y
    s.tick(0.01f, cx);
    sh.shot(cx, "wiz-shoulder", &s);
    s.on_capture({0, InKind::Axis, 2, 1, &g, &n});  // a stick moved on a mandatory row
    s.tick(0.01f, cx);
    sh.shot(cx, "wiz-holding", &s);
    s.on_capture({0, InKind::Axis, 2, 0, &g, &n});
    while (s.wizard().phase() == InputWizard::Phase::Rows) {
      if (!s.wizard().skip_row()) press_release(s, uint16_t(40 + s.wizard().row()));
    }
    s.tick(0.01f, cx);
    sh.shot(cx, "wiz-review", &s);
  }
  {
    ConfigScreen s(cx, kGuid, kName, 18, 1, 4);
    s.tick(0.01f, cx);
    while (s.wizard().phase() == InputWizard::Phase::Rows) press_release(s, uint16_t(20 + s.wizard().row()));
    s.tick(0.01f, cx);
    sh.shot(cx, "wiz-review-hotkey", &s);
  }

  // ---- bluetooth
  {
    put(paths().bt_live_log, "");
    std::unique_ptr<Screen> sc = Registry::instance().make("menu.controllers.btpair", cx, "");
    Menu& m = *menu_of(sc);
    m.handle(Action::Left, cx);
    settle(m, cx);
    sh.shot(cx, "btpair-scanning", &m);
    put(paths().bt_live_log,
        "<device id=\"AA:BB:CC:DD:EE:01\" name=\"8BitDo Pro 2\" status=\"added\" type=\"joystick\" />\n"
        "<device id=\"AA:BB:CC:DD:EE:02\" name=\"Wireless Controller\" status=\"added\" type=\"joystick\" />\n"
        "<device id=\"AA:BB:CC:DD:EE:03\" name=\"WH-1000XM4\" status=\"added\" type=\"audio\" />\n"
        "<device id=\"AA:BB:CC:DD:EE:04\" name=\"Some device with quite a long name that will not fit\" status=\"added\" type=\"\" />\n");
    m.tick(1.2f, cx);
    settle(m, cx);
    sh.shot(cx, "btpair", &m);
    cx.sv->toasts.push("8BitDo Pro 2: PAIRED", 5000);
    sh.shot(cx, "btpair-toast", &m, nullptr, true);
    cx.sv->toasts.expire(Clock::now() + std::chrono::seconds(10));
  }
  {
    std::unique_ptr<Screen> sc = Registry::instance().make("menu.controllers.btdevice", cx, "AA:BB:CC:DD:EE:01 1 8BitDo Pro 2");
    Menu& m = *menu_of(sc);
    m.handle(Action::Left, cx);
    settle(m, cx);
    sh.shot(cx, "btdevice", &m);
    Transition t = m.handle(Action::A, cx);
    if (t.screen) sh.shot(cx, "btdevice-confirm", &m, t.screen.get());
  }
  {
    std::unique_ptr<Screen> sc = Registry::instance().make("menu.controllers.btdevices", cx, "");
    Menu& m = *menu_of(sc);
    m.handle(Action::Left, cx);
    for (int i = 0; i < 400 && !cx.sv->jobs.idle(); ++i) ::usleep(2000);
    cx.sv->jobs.drain();
    settle(m, cx);
    sh.shot(cx, "btdevices-empty", &m);
  }
  fs::remove_all(tmp);
}

}  // namespace

int main() {
  const char* out = std::getenv("ZF_CTL_PNG");
  if (!out) return 0;
  const char* root = std::getenv("ZF_THEME_DIR");
  std::string base = root ? root : "/home/epsil/zenith-frontend";
  fs::create_directories(out);
  const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
  for (const char* theme : {"theme-dark", "theme-light"})
    for (const auto& wh : sizes) run_all(base + "/" + theme, theme + 6, wh[0], wh[1], out);
  return zt::done();
}
