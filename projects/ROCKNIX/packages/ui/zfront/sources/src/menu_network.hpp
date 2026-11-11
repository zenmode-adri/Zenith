// `menu.network` (ES "NETWORK SETTINGS") and its two sub-screens `menu.network.wifi` (scan, connect, forget) and
// `menu.network.manual` (hidden network: SSID + key). Registered in menu_network.cpp.
//
// Everything that touches the system goes through `cx.sv->cmd` (the same `wifictl` / `systemctl` commands ES runs),
// `cx.conf.persist` (system.cfg keys ES uses) and `cx.sv->jobs` (anything slow).
//
// Preview / test hook: when the environment variable `ZF_NET_ROOT` is set, every probe of the machine (`/sys/class/net`,
// `/usr/sbin/sshd`, the UDC...) is read below that directory and, in dry-run, the output of the commands comes from
// `<root>/fixture/*.txt` (see `apply_fixture` in menu_network.cpp). Without the variable (the console) nothing changes.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "menu.hpp"

namespace zf {
namespace netmenu {

/// The addresses ES shows for this console, the first one is the one to give out (below `$ZF_NET_ROOT/fixture/ips.txt` in tests).
std::vector<std::string> local_ip_list();
/// `http://<ip>:<port>/files`, the address of the web file transfer (an IPv6 address in brackets).
std::string web_address(const std::string& ip);

/// A menu that looks every two seconds at what changes by itself (the addresses of the console, whether the web listener runs, the paired
/// phones, the Wi-Fi / web transfer / Samba switches) and rebuilds when it changed, so what it shows is real without leaving it. `on_change`
/// (optional) runs first, on the main thread, when it did (the Wi-Fi list uses it to read the networks again).
std::unique_ptr<Screen> make_live_menu(MenuSpec spec, std::function<void(Ctx&, const Refresh&)> on_change = nullptr);

/// Forgets what the menu learned during the session (`wifictl has_ap_mode`); for tests.
void reset_session_cache();

}  // namespace netmenu
}  // namespace zf
