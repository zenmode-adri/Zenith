// Review pictures of the network menus: runs only when $ZF_NET_PNG names an output directory (it does nothing under
// ctest). Both themes at the four screen sizes, every relevant state, on the fake machine of test_menu_network.
//   ZF_NET_PNG=/home/epsil/zf-out/menu-network ZF_THEME_DIR=/home/epsil/zenith-frontend ./test_menu_network_png
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "cpu.hpp"
#include "menu.hpp"
#include "menu_network.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

void put(const std::string& path, const std::string& text) {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream f(path);
  f << text;
}

struct Machine {
  std::string root;
  Machine() {
    char dir[] = "/tmp/zf-netpng-XXXXXX";
    root = ::mkdtemp(dir);
    ::setenv("ZF_NET_ROOT", root.c_str(), 1);
    SystemConf::lock_path() = root + "/lock";
    netmenu::reset_session_cache();
  }
  ~Machine() {
    ::unsetenv("ZF_NET_ROOT");
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  void touch(const std::string& p) { put(root + p, ""); }
  void fixture(const std::string& name, const std::string& text) { put(root + "/fixture/" + name, text); }
};

std::unique_ptr<Screen> open(Ctx& cx, const char* name) {
  std::unique_ptr<Screen> s = Registry::instance().make(name, cx, "");
  if (s) s->tick(0.0f, cx);
  return s;
}
Menu* menu_of(std::unique_ptr<Screen>& s) { return static_cast<Menu*>(s.get()); }

void pump(Ctx& cx, Menu* m) {
  for (int i = 0; i < 600; ++i) {
    cx.sv->jobs.drain();
    if (cx.sv->jobs.idle()) break;
    ::usleep(5000);
  }
  cx.sv->jobs.drain();
  if (m) m->tick(0.0f, cx);
}

void settle(Menu& m, Ctx& cx) {
  for (int i = 0; i < 6; ++i) m.tick(1.0f, cx);
}

bool focus(Menu& m, Ctx& cx, const std::string& label) {
  int idx = -1;
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) idx = int(i);
  if (idx < 0) return false;
  for (size_t k = 0; k <= m.size() && m.cursor() != idx; ++k) m.handle(Action::Down, cx);
  return m.cursor() == idx;
}

struct Shooter {
  std::string dir, theme_name;
  unsigned w = 0, h = 0;
  int n = 0;
  void shot(Ctx& cx, const char* name, Screen* base, Screen* over = nullptr, bool toasts = false) {
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

void run_all(const std::string& theme_dir, const std::string& theme_name, unsigned w, unsigned h, const std::string& outdir) {
  Machine mc;
  for (const char* p : {"/usr/sbin/sshd", "/usr/sbin/smbd", "/usr/lib/systemd/system/simple-http-server.service", "/usr/bin/usbgadget", "/usr/bin/syncthing",
                        "/usr/bin/rclonectl", "/usr/sbin/tailscale", "/usr/sbin/zerotier-cli", "/storage/.config/wireguard/wg0.conf", "/fixture/ap_mode"})
    mc.touch(p);
  fs::create_directories(mc.root + "/sys/class/net/wlan0");
  put(mc.root + "/sys/class/udc/ff300000.usb/function", "mtp\n");
  mc.fixture("ips.txt", "192.168.1.42\n");
  mc.fixture("scanlist.txt", "Casa 5G\nMOVISTAR_2FA1\nCafe Libre\nOficina\nHotel_Gran_Via_Planta_3_Habit_12\nWIFI-Vecino\nDIRECT-roku-123\nTP-Link_9F2A\n");
  mc.fixture("detail.txt",
             "@ORDERED\na(on) 8 \"/net/connman/iwd/0/3/43617361203547_psk\" -4200 \"/net/connman/iwd/0/3/4d4f5649535441525f32464131_psk\" -6100 "
             "\"/net/connman/iwd/0/3/43616665204c69627265_open\" -7300 \"/net/connman/iwd/0/3/4f666963696e61_8021x\" -8200 "
             "\"/net/connman/iwd/0/3/486f74656c5f4772616e5f5669615f506c616e74615f335f48616269745f3132_psk\" -8400 "
             "\"/net/connman/iwd/0/3/574946492d566563696e6f_psk\" -8800 \"/net/connman/iwd/0/3/4449524543542d726f6b752d313233_psk\" -9000 "
             "\"/net/connman/iwd/0/3/54502d4c696e6b5f39463241_psk\" -9300\n@CONNECTED\no \"/net/connman/iwd/0/3/43617361203547_psk\"\n");
  mc.fixture("tailscale.txt", "100.64.0.7      zenith     adri@  linux   -\n");

  Ctx& cx = *new Ctx;  // leaked on purpose: framework caches (toast.cpp) key on the Assets address, which must never repeat
  std::string err;
  cx.w = float(w);
  cx.h = float(h);
  CHECK(Theme::load(theme_dir, ratio_name(w, h), cx.th, err));
  CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
  CHECK(load_common(cx.th, cx.assets, cx.common, err));
  cx.status = Status::demo();
  cx.sv->cmd.dry_run = true;
  cx.sv->cmd.keep_log = true;
  put(mc.root + "/system.cfg", "wifi.enabled=1\nwifi.ssid=Casa 5G\nwifi.country=ES\nssh.enabled=1\nsystem.hostname=zenith\ntailscale.up=1\nsyncthing.enabled=0\n");
  cx.conf = SystemConf::load(mc.root + "/system.cfg");
  cx.es = EsSettings::from_text("<?xml version=\"1.0\"?>\n<config>\n</config>\n", mc.root + "/es_settings.cfg");

  Shooter sh;
  sh.dir = outdir;
  sh.theme_name = theme_name;
  sh.w = w;
  sh.h = h;

  {  // main screen: top, then the end of the list, then Wi-Fi off, then the Spanish list
    auto s = open(cx, "menu.network");
    pump(cx, menu_of(s));
    settle(*menu_of(s), cx);
    sh.shot(cx, "main-top", s.get());
    focus(*menu_of(s), cx, "ZeroTier One");
    settle(*menu_of(s), cx);
    sh.shot(cx, "main-end", s.get());
    focus(*menu_of(s), cx, "WI-FI COUNTRY");
    settle(*menu_of(s), cx);
    Transition t = s->handle(Action::A, cx);
    if (t.screen) {
      for (int i = 0; i < 6; ++i) t.screen->tick(1.0f, cx);
      sh.shot(cx, "country-popup", s.get(), t.screen.get());
    }
  }
  {
    Ctx& c2 = *new Ctx;
    CHECK(Theme::load(theme_dir, ratio_name(w, h), c2.th, err));
    CHECK(c2.assets.load_font(c2.th.path(c2.th.str("font")), err));
    CHECK(load_common(c2.th, c2.assets, c2.common, err));
    c2.w = float(w);
    c2.h = float(h);
    c2.status = Status::demo();
    c2.sv->cmd.dry_run = true;
    put(mc.root + "/system.cfg2", "wifi.enabled=0\nsystem.hostname=zenith\n");
    c2.conf = SystemConf::load(mc.root + "/system.cfg2");
    c2.es = cx.es;
    auto s = open(c2, "menu.network");
    pump(c2, menu_of(s));
    settle(*menu_of(s), c2);
    Shooter s2 = sh;
    s2.n = 20;
    s2.shot(c2, "main-wifi-off", s.get());
  }

  {  // the networks list
    auto s = open(cx, "menu.network.wifi");
    Menu& m = *menu_of(s);
    sh.n = 30;
    sh.shot(cx, "wifi-scanning", s.get());
    pump(cx, &m);
    settle(m, cx);
    sh.shot(cx, "wifi-list", s.get());
    focus(m, cx, "MOVISTAR_2FA1");
    settle(m, cx);
    Transition t = m.handle(Action::A, cx);  // the key keyboard
    if (t.screen) {
      for (int i = 0; i < 3; ++i) t.screen->handle(Action::A, cx);
      sh.shot(cx, "wifi-key", t.screen.get());
      t.screen->handle(Action::B, cx);
      t.screen->handle(Action::B, cx);
      t.screen->handle(Action::B, cx);
      t.screen->handle(Action::B, cx);  // cancel on empty
    }
    focus(m, cx, "Cafe Libre");
    settle(m, cx);
    m.handle(Action::A, cx);  // open network: connects
    m.tick(0.0f, cx);
    settle(m, cx);
    sh.shot(cx, "wifi-connecting", s.get(), nullptr, true);
    pump(cx, &m);
    settle(m, cx);
    sh.shot(cx, "wifi-connected-toast", s.get(), nullptr, true);
    focus(m, cx, "Oficina");
    settle(m, cx);
    m.handle(Action::A, cx);
    sh.shot(cx, "wifi-enterprise-toast", s.get(), nullptr, true);
  }
  {  // saved network options
    mc.fixture("saved.txt", "Casa 5G\nVecino_lejos\n");
    auto s = open(cx, "menu.network.wifi");
    Menu& m = *menu_of(s);
    pump(cx, &m);
    settle(m, cx);
    focus(m, cx, "Casa 5G");
    Transition t = m.handle(Action::A, cx);
    if (t.screen) {
      for (int i = 0; i < 4; ++i) t.screen->tick(1.0f, cx);
      sh.shot(cx, "wifi-saved-options", s.get(), t.screen.get());
    }
    mc.fixture("saved.txt", "");
  }
  {  // nothing around
    mc.fixture("scanlist.txt", "");
    mc.fixture("detail.txt", "");
    auto s = open(cx, "menu.network.wifi");
    pump(cx, menu_of(s));
    settle(*menu_of(s), cx);
    sh.shot(cx, "wifi-empty", s.get());
  }
  {  // hidden network
    auto s = open(cx, "menu.network.manual");
    Menu& m = *menu_of(s);
    sh.shot(cx, "manual-empty", s.get());
    m.entry(0).set_s("Red Oculta");
    m.entry(1).set_s("clave 123");
    m.tick(0.0f, cx);
    settle(m, cx);
    sh.shot(cx, "manual-filled", s.get());
  }
}

}  // namespace

int main() {
  const char* out = std::getenv("ZF_NET_PNG");
  if (!out) return 0;
  const char* root = std::getenv("ZF_THEME_DIR");
  std::string base = root ? root : "/home/epsil/zenith-frontend";
  fs::create_directories(out);
  const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
  for (const char* theme : {"theme-dark", "theme-light"})
    for (const auto& wh : sizes) run_all(base + "/" + theme, theme + 6, wh[0], wh[1], out);
  return zt::done();
}
