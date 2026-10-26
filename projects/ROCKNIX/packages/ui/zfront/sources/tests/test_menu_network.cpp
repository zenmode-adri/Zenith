// menu.network: parsers (fed with the real formats of wifictl / iwd / tailscale / zerotier) and the three screens, driven
// through the registry on a fake machine (`ZF_NET_ROOT`) with the command runner in dry-run: the tests check the
// commands that would run and the values written to system.cfg / es_settings.cfg.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "menu_network.hpp"
#include "menu_network_countries.hpp"
#include "menu_network_parse.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"

using namespace zf;
using namespace zf::netmenu;
namespace fs = std::filesystem;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
void put(const std::string& path, const std::string& text) {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream f(path);
  f << text;
}

// The fake machine: a root directory with the files the menu probes and the fixture outputs of the commands.
struct Machine {
  std::string root;
  Machine() {
    char dir[] = "/tmp/zf-net-XXXXXX";
    root = ::mkdtemp(dir);
    ::setenv("ZF_NET_ROOT", root.c_str(), 1);
    SystemConf::lock_path() = root + "/lock";
    reset_session_cache();
  }
  ~Machine() {
    ::unsetenv("ZF_NET_ROOT");
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  void touch(const std::string& p) { put(root + p, ""); }
  void wifi() { fs::create_directories(root + "/sys/class/net/wlan0"); }
  void full_image() {  // everything the real image has
    wifi();
    for (const char* p : {"/usr/sbin/sshd", "/usr/sbin/smbd", "/usr/lib/systemd/system/simple-http-server.service", "/usr/bin/usbgadget", "/usr/bin/syncthing",
                          "/usr/bin/rclonectl", "/usr/sbin/tailscale", "/usr/sbin/zerotier-cli"})
      touch(p);
    put(root + "/sys/class/udc/ff300000.usb/function", "mtp\n");
  }
  void fixture(const std::string& name, const std::string& text) { put(root + "/fixture/" + name, text); }
  std::string cfg() const { return root + "/system.cfg"; }
};

void make_ctx(Ctx& cx, const Machine& mc, const std::string& conf_text, float w = 640, float h = 480) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = w;
  cx.h = h;
  cx.sv->cmd.dry_run = true;
  cx.sv->cmd.keep_log = true;
  put(mc.cfg(), conf_text);
  cx.conf = SystemConf::load(mc.cfg());
  cx.es = EsSettings::from_text("<?xml version=\"1.0\"?>\n<config>\n</config>\n", mc.root + "/es_settings.cfg");
}

Menu* menu_of(std::unique_ptr<Screen>& s) { return static_cast<Menu*>(s.get()); }

std::unique_ptr<Screen> open(Ctx& cx, const char* name) {
  CHECK(Registry::instance().has(name));
  std::unique_ptr<Screen> s = Registry::instance().make(name, cx, "");
  CHECK(s != nullptr);
  if (s) s->tick(0.0f, cx);  // builds and runs on_open
  return s;
}

int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}
bool has(const Menu& m, const std::string& label) { return find(m, label) >= 0; }

bool focus(Menu& m, Ctx& cx, const std::string& label) {
  int idx = find(m, label);
  if (idx < 0) return false;
  for (size_t k = 0; k <= m.size() && m.cursor() != idx; ++k) m.handle(Action::Down, cx);
  return m.cursor() == idx;
}

/// Wait for every job and run the `done` callbacks, then let the menu rebuild.
void pump(Ctx& cx, Menu* m = nullptr) {
  for (int i = 0; i < 600; ++i) {
    cx.sv->jobs.drain();
    if (cx.sv->jobs.idle()) break;
    ::usleep(5000);
  }
  cx.sv->jobs.drain();
  if (m) m->tick(0.0f, cx);
}

bool toast_has(Ctx& cx, const std::string& what) {
  for (const Toast& t : cx.sv->toasts.items())
    if (t.text.find(what) != std::string::npos) return true;
  return false;
}
bool log_has(Ctx& cx, const std::string& cmd) {
  for (const std::string& c : cx.sv->cmd.log)
    if (c == cmd) return true;
  return false;
}
int log_at(Ctx& cx, const std::string& cmd) {
  for (size_t i = 0; i < cx.sv->cmd.log.size(); ++i)
    if (cx.sv->cmd.log[i] == cmd) return int(i);
  return -1;
}
bool log_prefix(Ctx& cx, const std::string& p) {
  for (const std::string& c : cx.sv->cmd.log)
    if (c.compare(0, p.size(), p) == 0) return true;
  return false;
}

// ---- samples: the exact shapes of the OS ---------------------------------------------------------------------------------

// busctl call net.connman.iwd /net/connman/iwd/0/3 net.connman.iwd.Station GetOrderedNetworks, between our markers
const char* kDetail =
    "@ORDERED\n"
    "a(on) 5 \"/net/connman/iwd/0/3/43617361203547_psk\" -4200 \"/net/connman/iwd/0/3/4d4f5649535441525f32464131_psk\" -6100 "
    "\"/net/connman/iwd/0/3/43616665204c69627265_open\" -7300 \"/net/connman/iwd/0/3/4f666963696e61_8021x\" -8200 "
    "\"/net/connman/iwd/0/3/43617361c3b1_psk\" -9100\n"
    "@CONNECTED\n"
    "o \"/net/connman/iwd/0/3/43617361203547_psk\"\n";

const char* kScanlist = "Casa 5G\nMOVISTAR_2FA1\nCafe Libre\nOficina\nCasa\xc3\xb1\n";

// ---- tests --------------------------------------------------------------------------------------------------------------

void test_parsers() {
  // wifictl scanlist: one SSID per line, CR and blank lines tolerated, repeated names once
  auto names = parse_ssid_lines("Casa 5G\r\n\nMOVISTAR_2FA1\nCasa 5G\n");
  CHECK_EQ(names.size(), size_t(2));
  CHECK_EQ(names[0], std::string("Casa 5G"));
  CHECK(parse_ssid_lines("").empty());
  CHECK(parse_ssid_lines("\n\n").empty());

  std::string ssid, sec;
  CHECK(split_net_path("/net/connman/iwd/0/3/43617361203547_psk", ssid, sec));
  CHECK_EQ(ssid, std::string("Casa 5G"));
  CHECK_EQ(sec, std::string("psk"));
  CHECK(split_net_path("/net/connman/iwd/0/3/43617361c3b1_open", ssid, sec));
  CHECK_EQ(ssid, std::string("Casa\xc3\xb1"));
  CHECK(!split_net_path("/net/connman/iwd/0/3", ssid, sec));       // the station, not a network
  CHECK(!split_net_path("/net/connman/iwd/0/3/zz_psk", ssid, sec));  // not hex
  CHECK(!split_net_path("/org/other/4142_psk", ssid, sec));

  IwdDetail d = parse_iwd_details(kDetail);
  CHECK_EQ(d.nets.size(), size_t(5));
  CHECK_EQ(d.nets[0].ssid, std::string("Casa 5G"));
  CHECK_EQ(d.nets[0].signal, -4200);
  CHECK_EQ(d.nets[2].security, std::string("open"));
  CHECK_EQ(d.nets[3].security, std::string("8021x"));
  CHECK(d.has_connected);
  CHECK_EQ(d.connected.ssid, std::string("Casa 5G"));
  // no station / empty array / garbage
  CHECK(parse_iwd_details("").nets.empty());
  CHECK(parse_iwd_details("@ORDERED\na(on) 0\n@CONNECTED\n").nets.empty());
  CHECK(!parse_iwd_details("@ORDERED\na(on) 0\n@CONNECTED\n").has_connected);
  CHECK(parse_iwd_details("a(on) 1 \"/net/connman/iwd/0/3/4142_psk\" -5000").nets.size() == 1);  // bare busctl output

  CHECK_EQ(signal_percent(-4200), 100);
  CHECK_EQ(signal_percent(-6100), 78);
  CHECK_EQ(signal_percent(-7300), 54);
  CHECK_EQ(signal_percent(-10000), 0);
  CHECK_EQ(signal_percent(-12000), 0);

  // merge: ES order, detail attached by name, saved-out-of-range appended, connected from iwd
  std::vector<WifiNet> nets = merge_networks(parse_ssid_lines(kScanlist), d, {"MOVISTAR_2FA1", "Vecino"}, "");
  CHECK_EQ(nets.size(), size_t(6));
  CHECK_EQ(nets[0].ssid, std::string("Casa 5G"));
  CHECK(nets[0].connected && !nets[0].saved);
  CHECK_EQ(nets[0].signal_pct, 100);
  CHECK(nets[1].saved && !nets[1].connected && nets[1].security == "psk");
  CHECK(nets[2].open());
  CHECK(nets[3].enterprise());
  CHECK_EQ(nets[5].ssid, std::string("Vecino"));
  CHECK(nets[5].saved && nets[5].signal_pct < 0);
  // iwd silent: the stored wifi.ssid decides; no detail at all keeps the names
  nets = merge_networks({"A", "B"}, IwdDetail(), {}, "B");
  CHECK(!nets[0].connected && nets[1].connected && nets[0].security.empty() && nets[0].signal_pct < 0);
  nets = merge_networks({}, d, {}, "");  // scanlist empty but iwd has them
  CHECK_EQ(nets.size(), size_t(5));

  // tailscale status
  TsStatus t = parse_tailscale_status("Logged out.\n\nLog in at: https://login.tailscale.com/a/1234abcd5678\n");
  CHECK(t.logged_out);
  CHECK_EQ(t.url, std::string("https://login.tailscale.com/a/1234abcd5678"));
  t = parse_tailscale_status("100.64.0.7      zenith     adri@  linux   -\n100.64.0.9      pc         adri@  windows idle\n");
  CHECK(!t.logged_out && t.url.empty());
  CHECK(!parse_tailscale_status("").logged_out);  // ES: silence counts as up
  // zerotier info
  CHECK(zerotier_running("200 info 8a93de7f12 1.14.0 ONLINE\n"));
  CHECK(!zerotier_running("zerotier-cli: Error connecting to the ZeroTier service: Connection refused\n\nPlease check that the service is running and that TCP port 9993 is not blocked.\n"));
  CHECK(!zerotier_running(""));

  CHECK(valid_hostname("zenith"));
  CHECK(valid_hostname("zenith-2"));
  CHECK(!valid_hostname(""));
  CHECK(!valid_hostname("-x"));
  CHECK(!valid_hostname("my console"));
  CHECK(!valid_hostname("ñandú"));
  CHECK(!valid_hostname(std::string(64, 'a')));
  CHECK_EQ(usb_mode_from_function("cdc\n"), std::string("network"));
  CHECK_EQ(usb_mode_from_function("mtp"), std::string("file_transfer"));
  CHECK_EQ(usb_mode_from_function(""), std::string("disabled"));
  CHECK_EQ(first_line("192.168.7.2\n"), std::string("192.168.7.2"));
  CHECK(kCountryCount == 249);
}

void test_main_screen() {
  // ---- a console without Wi-Fi hardware, with nothing installed: only what always applies
  {
    Machine mc;
    Ctx cx;
    make_ctx(cx, mc, "wifi.enabled=1\n");
    auto s = open(cx, "menu.network");
    Menu& m = *menu_of(s);
    CHECK(has(m, "IP ADDRESS") && has(m, "INTERNET STATUS") && has(m, "HOSTNAME") && has(m, "SHOW NETWORK INDICATOR"));
    CHECK(!has(m, "ENABLE WI-FI"));
    CHECK(!has(m, "WI-FI NETWORKS"));
    CHECK(!has(m, "ENABLE SSH"));
    CHECK(!has(m, "NETWORK SERVICES") && !has(m, "VPN SERVICES") && !has(m, "CLOUD SERVICES"));  // no empty headers
    CHECK(!log_prefix(cx, "wifictl"));                                                           // nothing to prescan
    CHECK_EQ(m.entry(size_t(find(m, "IP ADDRESS"))).value_text, std::string("NOT CONNECTED"));
  }

  // ---- the full image, Wi-Fi off: no networks list, services as in ES, the order of ES
  {
    Machine mc;
    mc.full_image();
    mc.fixture("ips.txt", "192.168.1.42\n");
    Ctx cx;
    make_ctx(cx, mc, "wifi.enabled=0\nsystem.hostname=zenith\nssh.enabled=1\n");
    auto s = open(cx, "menu.network");
    Menu& m = *menu_of(s);
    CHECK(has(m, "ENABLE WI-FI"));
    CHECK(!has(m, "WI-FI NETWORKS") && !has(m, "WI-FI COUNTRY"));
    for (const char* l : {"ENABLE SSH", "ENABLE SAMBA", "ENABLE SIMPLE HTTP SERVER", "USB GADGET FUNCTION", "ENABLE SYNCTHING", "MOUNT CLOUD DRIVE", "TAILSCALE VPN", "ZeroTier One"})
      CHECK(has(m, l));
    CHECK(!has(m, "WIREGUARD VPN"));  // no wg0.conf
    CHECK(find(m, "ENABLE SSH") < find(m, "ENABLE SYNCTHING") && find(m, "ENABLE SYNCTHING") < find(m, "TAILSCALE VPN"));
    CHECK_EQ(m.entry(size_t(find(m, "IP ADDRESS"))).value_text, std::string("192.168.1.42"));
    CHECK_EQ(m.entry(size_t(find(m, "INTERNET STATUS"))).value_text, std::string("CONNECTED"));
    CHECK_EQ(m.entry(size_t(find(m, "HOSTNAME"))).kind, EntryKind::Text);
    CHECK_EQ(m.entry(size_t(find(m, "HOSTNAME"))).get_s(), std::string("zenith"));
    CHECK(m.entry(size_t(find(m, "ENABLE SSH"))).get_b());  // ssh.enabled=1 in system.cfg
    CHECK(!m.entry(size_t(find(m, "ENABLE SAMBA"))).get_b());
    CHECK_EQ(m.entry(size_t(find(m, "USB GADGET FUNCTION"))).get_s(), std::string("file_transfer"));  // UDC function "mtp"
    CHECK(!log_prefix(cx, "wifictl"));  // Wi-Fi off: no prescan
    // wireguard appears once its config exists
    mc.touch("/storage/.config/wireguard/wg0.conf");
    auto s2 = open(cx, "menu.network");
    CHECK(has(*menu_of(s2), "WIREGUARD VPN"));
  }

  // ---- several addresses: a row that lists them all
  {
    Machine mc;
    mc.fixture("ips.txt", "192.168.1.42\n192.168.7.2\n");
    Ctx cx;
    make_ctx(cx, mc, "");
    auto s = open(cx, "menu.network");
    Menu& m = *menu_of(s);
    int i = find(m, "IP ADDRESS");
    CHECK(i >= 0);
    CHECK_EQ(m.entry(size_t(i)).kind, EntryKind::Action);
    CHECK_EQ(m.entry(size_t(i)).value_text, std::string("192.168.1.42 (+)"));
    CHECK(focus(m, cx, "IP ADDRESS"));
    Transition t = m.handle(Action::A, cx);
    CHECK(t.kind == Transition::Push && t.screen);
  }

  // ---- SSH and Samba: ES's exact sequences, the setting stored first
  {
    Machine mc;
    mc.full_image();
    Ctx cx;
    make_ctx(cx, mc, "ssh.enabled=0\nsamba.enabled=1\n");
    auto s = open(cx, "menu.network");
    Menu& m = *menu_of(s);
    cx.sv->cmd.log.clear();
    CHECK(focus(m, cx, "ENABLE SSH"));
    m.handle(Action::A, cx);
    CHECK(slurp(mc.cfg()).find("ssh.enabled=1") != std::string::npos);  // before the job ends
    pump(cx, &m);
    std::vector<std::string> want = {"mkdir -p /storage/.cache/services/", "touch /storage/.cache/services/sshd.conf", "systemctl enable sshd", "systemctl start sshd"};
    CHECK_EQ(cx.sv->cmd.log, want);
    CHECK(toast_has(cx, "SSH is on"));
    CHECK(m.entry(size_t(find(m, "ENABLE SSH"))).get_b());
    cx.sv->cmd.log.clear();
    m.handle(Action::A, cx);  // off
    pump(cx, &m);
    want = {"systemctl stop sshd", "systemctl disable sshd", "rm /storage/.cache/services/sshd.conf"};
    CHECK_EQ(cx.sv->cmd.log, want);
    CHECK(slurp(mc.cfg()).find("ssh.enabled=0") != std::string::npos);
    CHECK(toast_has(cx, "SSH is off"));

    cx.sv->cmd.log.clear();
    CHECK(focus(m, cx, "ENABLE SAMBA"));
    m.handle(Action::A, cx);  // was on -> off
    pump(cx, &m);
    want = {"systemctl stop nmbd", "systemctl stop smbd", "rm /storage/.cache/services/smb.conf"};
    CHECK_EQ(cx.sv->cmd.log, want);
    CHECK(slurp(mc.cfg()).find("samba.enabled=0") != std::string::npos);
    cx.sv->cmd.log.clear();
    m.handle(Action::A, cx);
    pump(cx, &m);
    want = {"mkdir -p /storage/.cache/services/", "touch /storage/.cache/services/smb.conf", "systemctl start nmbd", "systemctl start smbd"};
    CHECK_EQ(cx.sv->cmd.log, want);

    cx.sv->cmd.log.clear();
    CHECK(focus(m, cx, "ENABLE SIMPLE HTTP SERVER"));
    m.handle(Action::A, cx);
    pump(cx, &m);
    CHECK_EQ(cx.sv->cmd.log, std::vector<std::string>{"systemctl enable --now simple-http-server"});
    CHECK(slurp(mc.cfg()).find("simplehttp.enabled=1") != std::string::npos);
    cx.sv->cmd.log.clear();
    CHECK(focus(m, cx, "ENABLE SYNCTHING"));
    m.handle(Action::A, cx);
    pump(cx, &m);
    CHECK_EQ(cx.sv->cmd.log, std::vector<std::string>{"systemctl start syncthing"});
    m.handle(Action::A, cx);
    pump(cx, &m);
    CHECK(log_has(cx, "systemctl stop syncthing"));
    CHECK(slurp(mc.cfg()).find("syncthing.enabled=0") != std::string::npos);
  }

  // ---- hostname: stored for the next boot; a name the boot script cannot use is refused
  {
    Machine mc;
    Ctx cx;
    make_ctx(cx, mc, "system.hostname=zenith\n");
    auto s = open(cx, "menu.network");
    Menu& m = *menu_of(s);
    const Entry& h = m.entry(size_t(find(m, "HOSTNAME")));
    h.set_s("my console");
    CHECK(slurp(mc.cfg()).find("system.hostname=zenith\n") != std::string::npos);
    CHECK(toast_has(cx, "letters"));
    h.set_s("  zenith-2 ");
    CHECK(slurp(mc.cfg()).find("system.hostname=zenith-2\n") != std::string::npos);
    CHECK(!log_prefix(cx, ""));  // no command at all: ES also only stores it
  }

  // ---- ES setting
  {
    Machine mc;
    Ctx cx;
    make_ctx(cx, mc, "");
    auto s = open(cx, "menu.network");
    Menu& m = *menu_of(s);
    CHECK(m.entry(size_t(find(m, "SHOW NETWORK INDICATOR"))).get_b());  // ES default true
    CHECK(focus(m, cx, "SHOW NETWORK INDICATOR"));
    m.handle(Action::A, cx);
    CHECK(slurp(mc.root + "/es_settings.cfg").find("ShowNetworkIndicator") != std::string::npos);
    CHECK(!cx.es.get_bool("ShowNetworkIndicator", true));
  }

  // ---- USB gadget: network mode asks for the address and says it
  {
    Machine mc;
    mc.full_image();
    mc.fixture("usb_address.txt", "192.168.7.2\n");
    Ctx cx;
    make_ctx(cx, mc, "");
    auto s = open(cx, "menu.network");
    Menu& m = *menu_of(s);
    cx.sv->cmd.log.clear();
    m.entry(size_t(find(m, "USB GADGET FUNCTION"))).set_s("network");
    pump(cx, &m);
    CHECK_EQ(cx.sv->cmd.log, (std::vector<std::string>{"/usr/bin/usbgadget network", "/usr/bin/usbgadget address"}));
    CHECK(toast_has(cx, "192.168.7.2"));
    cx.sv->cmd.log.clear();
    m.entry(size_t(find(m, "USB GADGET FUNCTION"))).set_s("disabled");
    pump(cx, &m);
    CHECK_EQ(cx.sv->cmd.log, std::vector<std::string>{"/usr/bin/usbgadget disabled"});
    // without a UDC the entry is not shown
    fs::remove_all(mc.root + "/sys/class/udc");
    auto s2 = open(cx, "menu.network");
    CHECK(!has(*menu_of(s2), "USB GADGET FUNCTION"));
  }

  // ---- VPN and cloud: the setting follows the real outcome
  {
    Machine mc;
    mc.full_image();
    mc.touch("/storage/.config/wireguard/wg0.conf");
    mc.fixture("tailscale.txt", "Logged out.\n\nLog in at: https://login.tailscale.com/a/1234abcd5678\n");
    mc.fixture("zerotier.txt", "200 info 8a93de7f12 1.14.0 ONLINE\n");
    Ctx cx;
    make_ctx(cx, mc, "");
    auto s = open(cx, "menu.network");
    Menu& m = *menu_of(s);
    cx.sv->cmd.log.clear();
    CHECK(focus(m, cx, "TAILSCALE VPN"));
    m.handle(Action::A, cx);
    pump(cx, &m);
    CHECK_EQ(cx.sv->cmd.log, (std::vector<std::string>{"systemctl start tailscaled", "tailscale up --timeout=7s", "tailscale status"}));
    CHECK(slurp(mc.cfg()).find("tailscale.up=0") != std::string::npos);  // logged out: not up
    CHECK(!m.entry(size_t(find(m, "TAILSCALE VPN"))).get_b());
    CHECK(has(m, "TAILSCALE LOGIN"));                                    // the link to log in
    CHECK(toast_has(cx, "log in"));
    // logged in
    mc.fixture("tailscale.txt", "100.64.0.7      zenith     adri@  linux   -\n");
    cx.sv->cmd.log.clear();
    m.handle(Action::A, cx);
    pump(cx, &m);
    CHECK(slurp(mc.cfg()).find("tailscale.up=1") != std::string::npos);
    CHECK(!has(m, "TAILSCALE LOGIN"));
    cx.sv->cmd.log.clear();
    m.handle(Action::A, cx);
    pump(cx, &m);
    CHECK_EQ(cx.sv->cmd.log, (std::vector<std::string>{"tailscale down", "systemctl stop tailscaled"}));
    CHECK(slurp(mc.cfg()).find("tailscale.up=0") != std::string::npos);

    cx.sv->cmd.log.clear();
    CHECK(focus(m, cx, "ZeroTier One"));
    m.handle(Action::A, cx);
    pump(cx, &m);
    CHECK_EQ(cx.sv->cmd.log, (std::vector<std::string>{"systemctl start zerotier-one", "zerotier-cli -D/storage/.config/zerotier/ info"}));
    CHECK(slurp(mc.cfg()).find("zerotier.up=1") != std::string::npos);
    mc.fixture("zerotier.txt", "zerotier-cli: Error connecting to the ZeroTier service: Connection refused\n");
    m.handle(Action::A, cx);  // off
    pump(cx, &m);
    m.handle(Action::A, cx);  // on again, the daemon does not answer
    pump(cx, &m);
    CHECK(slurp(mc.cfg()).find("zerotier.up=0") != std::string::npos);

    cx.sv->cmd.log.clear();
    CHECK(focus(m, cx, "WIREGUARD VPN"));
    m.handle(Action::A, cx);
    pump(cx, &m);
    CHECK_EQ(cx.sv->cmd.log, (std::vector<std::string>{"systemctl start connman-vpn", "wg-quick up /storage/.config/wireguard/wg0.conf"}));
    cx.sv->cmd.log.clear();
    m.handle(Action::A, cx);
    pump(cx, &m);
    CHECK_EQ(cx.sv->cmd.log, (std::vector<std::string>{"wg-quick down /storage/.config/wireguard/wg0.conf", "systemctl stop connman-vpn"}));

    cx.sv->cmd.log.clear();
    CHECK(focus(m, cx, "MOUNT CLOUD DRIVE"));
    m.handle(Action::A, cx);
    pump(cx, &m);
    CHECK_EQ(cx.sv->cmd.log, std::vector<std::string>{"rclonectl mount"});
    CHECK(slurp(mc.cfg()).find("clouddrive.mounted=1") != std::string::npos);
    m.handle(Action::A, cx);
    pump(cx, &m);
    CHECK(log_has(cx, "rclonectl unmount"));
    CHECK(slurp(mc.cfg()).find("clouddrive.mounted=0") != std::string::npos);
  }
}

void test_wifi_toggle_and_country() {
  Machine mc;
  mc.full_image();
  mc.fixture("ap_mode", "");
  Ctx cx;
  make_ctx(cx, mc, "wifi.enabled=0\nwifi.ssid=Casa 5G\nwifi.key=secreta\nwifi.country=\n");
  auto s = open(cx, "menu.network");
  Menu& m = *menu_of(s);
  CHECK(!has(m, "WI-FI NETWORKS") && !has(m, "LOCAL PLAY MODE"));
  cx.sv->cmd.log.clear();
  CHECK(focus(m, cx, "ENABLE WI-FI"));
  m.handle(Action::A, cx);
  CHECK(slurp(mc.cfg()).find("wifi.enabled=1") != std::string::npos);
  pump(cx, &m);
  // ApiSystem::enableWifi, byte for byte (bootlog-save hides the key by this shape)
  CHECK(log_at(cx, "wifictl enable") >= 0);
  CHECK(log_at(cx, "wifictl connect 'Casa 5G' 'secreta' ''") > log_at(cx, "wifictl enable"));
  CHECK(log_has(cx, "wifictl has_ap_mode"));
  CHECK(toast_has(cx, "Wi-Fi is on"));
  for (const Toast& t : cx.sv->toasts.items()) CHECK(t.text.find("secreta") == std::string::npos);  // the key never shows
  // the rows that depend on Wi-Fi appear
  CHECK(has(m, "WI-FI NETWORKS") && has(m, "WI-FI COUNTRY"));
  CHECK(has(m, "LOCAL PLAY MODE"));  // the adapter supports AP mode
  CHECK_EQ(m.entry(size_t(find(m, "WI-FI COUNTRY"))).value_text, std::string("N/A"));

  // country: a list popup, pick Spain -> stored, radio reconfigured with it
  cx.sv->cmd.log.clear();
  CHECK(focus(m, cx, "WI-FI COUNTRY"));
  Transition t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  size_t spain = 0;
  for (unsigned i = 0; i < kCountryCount; ++i)
    if (std::string(kCountries[i].code) == "ES") spain = i + 1;  // +1: N/A first
  for (size_t i = 0; i < spain; ++i) t.screen->handle(Action::Down, cx);
  Transition done = t.screen->handle(Action::A, cx);
  (void)done;
  pump(cx, &m);
  CHECK(slurp(mc.cfg()).find("wifi.country=ES") != std::string::npos);
  CHECK(log_has(cx, "wifictl enable"));
  CHECK(log_has(cx, "wifictl connect 'Casa 5G' 'secreta' 'ES'"));
  CHECK_EQ(m.entry(size_t(find(m, "WI-FI COUNTRY"))).value_text, std::string("Spain"));

  // local play: ES's keys, radio restarted in the new mode, networks list replaced by the ID
  cx.sv->cmd.log.clear();
  CHECK(focus(m, cx, "LOCAL PLAY MODE"));
  m.handle(Action::A, cx);
  pump(cx, &m);
  std::string c = slurp(mc.cfg());
  CHECK(c.find("wifi.adhoc.enabled=1") != std::string::npos);
  CHECK(c.find("global.netplay.host=192.168.80.1") != std::string::npos);
  CHECK(c.find("global.netplay.port=55435") != std::string::npos);
  CHECK(c.find("global.netplay.relay=none") != std::string::npos);
  CHECK(c.find("wifi.adhoc.id=1") != std::string::npos);
  CHECK(log_at(cx, "wifictl disable") >= 0 && log_at(cx, "wifictl enable") > log_at(cx, "wifictl disable"));
  CHECK(!has(m, "WI-FI NETWORKS") && !has(m, "WI-FI COUNTRY") && has(m, "LOCAL PLAY ID"));
  m.entry(size_t(find(m, "LOCAL PLAY ID"))).set_s("3");
  CHECK(slurp(mc.cfg()).find("wifi.adhoc.id=3") != std::string::npos);

  // off
  cx.sv->cmd.log.clear();
  CHECK(focus(m, cx, "ENABLE WI-FI"));
  m.handle(Action::A, cx);
  pump(cx, &m);
  CHECK_EQ(cx.sv->cmd.log, std::vector<std::string>{"wifictl disable"});
  CHECK(slurp(mc.cfg()).find("wifi.enabled=0") != std::string::npos);
  CHECK(toast_has(cx, "Wi-Fi is off"));

  // no stored network: enabling must not report an error (the connect step has nothing to join)
  {
    Ctx c2;
    make_ctx(c2, mc, "wifi.enabled=0\n");
    auto s2 = open(c2, "menu.network");
    Menu& m2 = *menu_of(s2);
    m2.entry(size_t(find(m2, "ENABLE WI-FI"))).set_b(true);
    pump(c2, &m2);
    CHECK(toast_has(c2, "Wi-Fi is on"));
    CHECK(log_has(c2, "wifictl connect '' '' ''"));
  }
  // enabling fails
  {
    Ctx c3;
    make_ctx(c3, mc, "wifi.enabled=0\nwifi.ssid=X\n");
    mc.fixture("connect_fail", "");
    auto s3 = open(c3, "menu.network");
    Menu& m3 = *menu_of(s3);
    m3.entry(size_t(find(m3, "ENABLE WI-FI"))).set_b(true);
    pump(c3, &m3);
    CHECK(toast_has(c3, "configuration error"));
  }
}

void test_prescan_and_ap_cache() {
  Machine mc;
  mc.wifi();
  Ctx cx;
  make_ctx(cx, mc, "wifi.enabled=1\n");
  auto s = open(cx, "menu.network");
  pump(cx, menu_of(s));
  CHECK(log_has(cx, "wifictl prescan"));  // patch 0016: scan while the user is still here
  CHECK(log_has(cx, "wifictl has_ap_mode"));
  CHECK(!has(*menu_of(s), "LOCAL PLAY MODE"));  // no ap_mode fixture: not supported
  // asked once per session
  cx.sv->cmd.log.clear();
  auto s2 = open(cx, "menu.network");
  pump(cx, menu_of(s2));
  CHECK(!log_has(cx, "wifictl has_ap_mode"));
  // ad hoc mode on: no prescan (ES: only without adhoc)
  Ctx c2;
  make_ctx(c2, mc, "wifi.enabled=1\nwifi.adhoc.enabled=1\n");
  auto s3 = open(c2, "menu.network");
  pump(c2, menu_of(s3));
  CHECK(!log_has(c2, "wifictl prescan"));
  CHECK(has(*menu_of(s3), "LOCAL PLAY ID"));
}

void test_wifi_list() {
  Machine mc;
  mc.wifi();
  mc.fixture("scanlist.txt", kScanlist);
  mc.fixture("detail.txt", kDetail);
  mc.fixture("ips.txt", "192.168.1.42\n");
  Ctx cx;
  make_ctx(cx, mc, "wifi.enabled=1\nwifi.ssid=Casa 5G\nwifi.country=ES\n");
  auto s = open(cx, "menu.network.wifi");
  Menu& m = *menu_of(s);
  // while scanning there is nothing to choose
  CHECK(has(m, "SEARCHING WI-FI NETWORKS"));
  CHECK_EQ(m.cursor(), -1);
  pump(cx, &m);
  CHECK(log_has(cx, "wifictl scanlist"));
  CHECK(!has(m, "SEARCHING WI-FI NETWORKS"));
  // order of ES, then the actions
  const char* want[] = {"Casa 5G", "MOVISTAR_2FA1", "Cafe Libre", "Oficina", "Casa\xc3\xb1"};
  for (size_t i = 0; i < 5; ++i) CHECK_EQ(m.entry(i).label, std::string(want[i]));
  CHECK(has(m, "REFRESH") && has(m, "INPUT MANUALLY"));
  CHECK_EQ(m.cursor(), 0);  // the cursor lands on the first network
  CHECK_EQ(m.entry(0).value_text, std::string("CONNECTED  100%"));
  CHECK_EQ(m.entry(1).value_text, std::string("SECURED  78%"));
  CHECK_EQ(m.entry(2).value_text, std::string("OPEN  54%"));
  CHECK_EQ(m.entry(3).value_text, std::string("ENTERPRISE  36%"));

  // enterprise: not supported, no command
  cx.sv->cmd.log.clear();
  CHECK(focus(m, cx, "Oficina"));
  m.handle(Action::A, cx);
  pump(cx, &m);
  CHECK(toast_has(cx, "not supported"));
  CHECK(cx.sv->cmd.log.empty());

  // open network: ES asks for a key anyway (any network that is not saved), so the keyboard comes up; accepted empty it sends
  // an empty key, with the ssid stored before the commands
  CHECK(focus(m, cx, "Cafe Libre"));
  Transition t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  t.screen->handle(Action::Start, cx);
  std::string c = slurp(mc.cfg());
  CHECK(c.find("wifi.ssid=Cafe Libre") != std::string::npos);
  pump(cx, &m);
  CHECK(log_at(cx, "wifictl enable") >= 0);
  CHECK(log_has(cx, "wifictl connect 'Cafe Libre' '' 'ES'"));
  CHECK(toast_has(cx, "Connected successfully"));
  CHECK(log_has(cx, "wifictl list"));  // afterwards the list is read again, without scanning

  // secured network: the key keyboard (password), A types the first key, Start accepts
  cx.sv->cmd.log.clear();
  CHECK(focus(m, cx, "MOVISTAR_2FA1"));
  t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  t.screen->handle(Action::A, cx);
  Transition ok = t.screen->handle(Action::Start, cx);
  (void)ok;
  c = slurp(mc.cfg());
  CHECK(c.find("wifi.ssid=MOVISTAR_2FA1") != std::string::npos);
  CHECK(c.find("wifi.key=q") != std::string::npos);
  pump(cx, &m);
  CHECK(log_has(cx, "wifictl connect 'MOVISTAR_2FA1' 'q' 'ES'"));

  // failure: a toast, the pair stays stored (as ES)
  mc.fixture("connect_fail", "");
  cx.sv->cmd.log.clear();
  CHECK(focus(m, cx, "Cafe Libre"));
  t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  t.screen->handle(Action::Start, cx);
  pump(cx, &m);
  CHECK(toast_has(cx, "Failed to connect"));

  // SSID with quotes and spaces reaches the shell quoted
  fs::remove(mc.root + "/fixture/connect_fail");
  mc.fixture("scanlist.txt", "Bob's \"WiFi\"\n");
  mc.fixture("detail.txt", "@ORDERED\na(on) 1 \"/net/connman/iwd/0/3/426f62277320225769466922_open\" -5000\n@CONNECTED\n");
  CHECK(focus(m, cx, "REFRESH"));
  m.handle(Action::A, cx);
  m.tick(0.0f, cx);
  CHECK(has(m, "SEARCHING WI-FI NETWORKS"));
  pump(cx, &m);
  CHECK(has(m, "Bob's \"WiFi\""));
  cx.sv->cmd.log.clear();
  CHECK(focus(m, cx, "Bob's \"WiFi\""));
  t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  t.screen->handle(Action::Start, cx);
  pump(cx, &m);
  CHECK(log_has(cx, "wifictl connect 'Bob'\\''s \"WiFi\"' '' 'ES'"));

  // nothing around
  mc.fixture("scanlist.txt", "");
  mc.fixture("detail.txt", "");
  CHECK(focus(m, cx, "REFRESH"));
  m.handle(Action::A, cx);
  pump(cx, &m);
  CHECK(has(m, "NO WI-FI NETWORKS FOUND"));
  CHECK(m.cursor() >= 0);  // REFRESH is still reachable
}

void test_known_names() {
  std::string ssid, sec;
  CHECK(known_network_name("Casa 5G.psk", ssid, sec) && ssid == "Casa 5G" && sec == "psk");
  CHECK(known_network_name("Cafe_Libre-2.open", ssid, sec) && ssid == "Cafe_Libre-2" && sec == "open");
  CHECK(known_network_name("=43617361c3b1.psk", ssid, sec) && ssid == "Casa\xc3\xb1" && sec == "psk");
  CHECK(known_network_name("Oficina.8021x", ssid, sec) && sec == "8021x");
  CHECK(known_network_name("a.b.psk", ssid, sec) && ssid == "a.b");  // only the last dot separates the type
  CHECK(!known_network_name("Hotspot.ap", ssid, sec));
  CHECK(!known_network_name("ap", ssid, sec));
  CHECK(!known_network_name(".psk", ssid, sec));
  CHECK(!known_network_name("=4361736.psk", ssid, sec));  // odd number of digits
  CHECK(!known_network_name("=zz.psk", ssid, sec));
  CHECK(!known_network_name("=.psk", ssid, sec));
}

void test_saved_networks() {
  // the stock `wifictl list_saved` prints nothing and has no connect_saved / forget_saved: the saved networks are the profiles in
  // iwd's state folder, joined and forgotten with iwctl
  Machine mc;
  mc.wifi();
  mc.fixture("scanlist.txt", "Casa 5G\nMOVISTAR_2FA1\n");
  mc.fixture("detail.txt", "");
  fs::create_directories(mc.root + "/storage/.cache/iwd/ap");
  put(mc.root + "/storage/.cache/iwd/Casa 5G.psk", "[Security]\nPassphrase=x\n");
  put(mc.root + "/storage/.cache/iwd/=566563696e6f.psk", "[Security]\nPassphrase=y\n");  // "Vecino", written in hex
  put(mc.root + "/storage/.cache/iwd/ap/Hotspot.ap", "");
  Ctx cx;
  make_ctx(cx, mc, "wifi.enabled=1\nwifi.ssid=Otra\nwifi.key=zzz\n");
  auto s = open(cx, "menu.network.wifi");
  Menu& m = *menu_of(s);
  pump(cx, &m);
  CHECK(has(m, "Vecino"));  // saved but out of range
  CHECK_EQ(m.entry(size_t(find(m, "Casa 5G"))).value_text, std::string("SAVED"));
  CHECK(find(m, "Vecino") > find(m, "MOVISTAR_2FA1"));

  // connect: no key asked; on success the pair in system.cfg is replaced (ES)
  cx.sv->cmd.log.clear();
  CHECK(focus(m, cx, "Casa 5G"));
  Transition t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  Transition r = t.screen->handle(Action::A, cx);  // first option: CONNECT
  (void)r;
  pump(cx, &m);
  CHECK(log_has(cx, "iwctl station 'wlan0' connect 'Casa 5G'"));
  CHECK(!log_has(cx, "wifictl connect_saved"));
  std::string c = slurp(mc.cfg());
  CHECK(c.find("wifi.ssid=Casa 5G") != std::string::npos);
  CHECK(c.find("wifi.key=\n") != std::string::npos);

  // forget
  cx.sv->cmd.log.clear();
  CHECK(focus(m, cx, "Vecino"));
  t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  t.screen->handle(Action::Down, cx);  // FORGET
  r = t.screen->handle(Action::A, cx);
  (void)r;
  pump(cx, &m);
  CHECK(log_has(cx, "iwctl known-networks 'Vecino' forget"));
  CHECK(toast_has(cx, "forgotten"));
  CHECK(!has(m, "Hotspot"));  // AP profiles are not networks to join

  // forgetting the network of system.cfg also clears the pair that wifictl connect would use at the next wake-up
  put(mc.root + "/storage/.cache/iwd/Otra.psk", "");
  m.handle(Action::A, cx);  // (back on the list: refresh to see it)
  CHECK(focus(m, cx, "REFRESH"));
  m.handle(Action::A, cx);
  pump(cx, &m);
  CHECK(focus(m, cx, "Otra"));
  t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  t.screen->handle(Action::Down, cx);
  t.screen->handle(Action::A, cx);
  pump(cx, &m);
  c = slurp(mc.cfg());
  CHECK(c.find("wifi.key=zzz") == std::string::npos);
  CHECK(c.find("wifi.ssid=Otra") == std::string::npos);

  // iwd answers and says it is not on that network although iwctl exited 0: failure; on it: success
  mc.fixture("detail.txt", "@ORDERED\na(on) 1 \"/net/connman/iwd/0/3/43617361203547_psk\" -5000\n@CONNECTED\n");
  CHECK(focus(m, cx, "Casa 5G"));
  t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  t.screen->handle(Action::A, cx);
  pump(cx, &m);
  CHECK(cx.sv->toasts.items().back().text.find("Failed to connect") != std::string::npos);
  mc.fixture("detail.txt", "@ORDERED\n@CONNECTED\no \"/net/connman/iwd/0/3/43617361203547_psk\"\n");
  CHECK(focus(m, cx, "Casa 5G"));
  t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  t.screen->handle(Action::A, cx);
  pump(cx, &m);
  CHECK(cx.sv->toasts.items().back().text.find("Connected successfully") != std::string::npos);
  mc.fixture("detail.txt", "");

  // a failing iwctl is reported
  mc.fixture("connect_fail", "");
  CHECK(focus(m, cx, "Casa 5G"));
  t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  t.screen->handle(Action::A, cx);
  pump(cx, &m);
  CHECK(toast_has(cx, "Failed to connect"));
}

void test_manual() {
  Machine mc;
  mc.wifi();
  Ctx cx;
  make_ctx(cx, mc, "wifi.enabled=1\nwifi.country=ES\n");
  auto s = open(cx, "menu.network.manual");
  Menu& m = *menu_of(s);
  CHECK(has(m, "WI-FI SSID") && has(m, "WI-FI KEY"));
  CHECK(!has(m, "CONNECT"));  // nothing to join yet
  CHECK(m.entry(size_t(find(m, "WI-FI KEY"))).password);
  m.entry(size_t(find(m, "WI-FI SSID"))).set_s("Red Oculta");
  m.entry(size_t(find(m, "WI-FI KEY"))).set_s("clave 123");
  m.tick(0.0f, cx);
  CHECK(has(m, "CONNECT"));
  CHECK(focus(m, cx, "CONNECT"));
  m.handle(Action::A, cx);
  pump(cx, &m);
  CHECK(log_has(cx, "wifictl connect 'Red Oculta' 'clave 123' 'ES'"));
  std::string c = slurp(mc.cfg());
  CHECK(c.find("wifi.ssid=Red Oculta") != std::string::npos);
  CHECK(c.find("wifi.key=clave 123") != std::string::npos);
  CHECK(toast_has(cx, "Connected successfully"));
}

}  // namespace

int main() {
  test_parsers();
  test_main_screen();
  test_known_names();
  test_wifi_toggle_and_country();
  test_prescan_and_ap_cache();
  test_wifi_list();
  test_saved_networks();
  test_manual();
  return zt::done();
}
