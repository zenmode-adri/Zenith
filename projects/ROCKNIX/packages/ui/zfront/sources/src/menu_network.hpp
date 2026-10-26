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

namespace zf {
namespace netmenu {

/// Forgets what the menu learned during the session (`wifictl has_ap_mode`); for tests.
void reset_session_cache();

}  // namespace netmenu
}  // namespace zf
