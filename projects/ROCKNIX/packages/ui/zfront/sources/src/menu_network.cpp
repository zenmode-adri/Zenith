// ES "NETWORK SETTINGS" for zfront. Same commands, same system.cfg / es_settings.cfg keys as ES (GuiMenu::openNetworkSettings,
// GuiWifi, ApiSystem), another presentation. See menu_network.hpp.
#include "menu_network.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#include "menu.hpp"
#include "menu_network_countries.hpp"
#include "menu_network_parse.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "ui_prefs.hpp"
#include "util.hpp"

namespace zf {
namespace netmenu {

namespace {

// ---- machine probes (below $ZF_NET_ROOT in tests and previews) --------------------------------------------------------

std::string root() {
  const char* r = std::getenv("ZF_NET_ROOT");
  return r ? r : "";
}
std::string under(const std::string& p) { return root() + p; }
bool exists(const std::string& p) { return file_exists(under(p)); }

std::vector<std::string> list_dir(const std::string& dir) {
  std::vector<std::string> v;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d)) {
      if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) v.emplace_back(e->d_name);
    }
    ::closedir(d);
  }
  std::sort(v.begin(), v.end());
  return v;
}

/// A wireless interface exists (`/sys/class/net/wlan*`); without it every Wi-Fi row is hidden.
bool has_wifi() {
  for (const std::string& n : list_dir(under("/sys/class/net")))
    if (n.compare(0, 4, "wlan") == 0) return true;
  return false;
}

/// Addresses ES shows (ApiSystem::getIpAddresses): IPv4 of every non-loopback interface, IPv6 (not link-local) only when
/// there is no IPv4.
std::vector<std::string> local_ips() {
  std::vector<std::string> v;
  if (!root().empty()) {
    std::string t;
    if (read_file(under("/fixture/ips.txt"), t)) v = parse_ssid_lines(t);
    return v;
  }
  ifaddrs* all = nullptr;
  if (getifaddrs(&all) != 0) return v;
  char buf[INET6_ADDRSTRLEN];
  for (ifaddrs* p = all; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || (p->ifa_flags & IFF_LOOPBACK)) continue;
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, buf, sizeof buf);
    v.emplace_back(buf);
  }
  if (v.empty()) {
    for (ifaddrs* p = all; p; p = p->ifa_next) {
      if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET6 || (p->ifa_flags & IFF_LOOPBACK)) continue;
      inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(p->ifa_addr)->sin6_addr, buf, sizeof buf);
      if (std::strncmp(buf, "fe80:", 5) == 0) continue;
      v.emplace_back(buf);
    }
  }
  freeifaddrs(all);
  return v;
}

/// Networks iwd has a profile for (it saves one after every successful connection): the files of its state folder. This is
/// what `wifictl list_saved` should print, but the script of this OS does not implement it (it exits at once), so zfront reads
/// the folder itself: no process, no D-Bus, and it works while iwd is down.
std::vector<std::string> saved_networks() {
  std::vector<std::string> v;
  for (const std::string& f : list_dir(under("/storage/.cache/iwd"))) {
    std::string ssid, sec;
    if (known_network_name(f, ssid, sec) && std::find(v.begin(), v.end(), ssid) == v.end()) v.push_back(ssid);
  }
  return v;
}

/// The wireless interface (`wlan0`), empty when there is none.
std::string wifi_device() {
  for (const std::string& n : list_dir(under("/sys/class/net")))
    if (n.compare(0, 4, "wlan") == 0) return n;
  return {};
}

/// The USB device controller, if the console has one: its `function` file ("cdc", "mtp", ""...).
bool usb_function(std::string& out) {
  std::vector<std::string> udcs = list_dir(under("/sys/class/udc"));
  if (udcs.empty()) return false;
  out.clear();
  read_file(under("/sys/class/udc/" + udcs.front() + "/function"), out);
  return true;
}

// ---- commands -----------------------------------------------------------------------------------------------------------

/// In dry-run (tests, previews) with $ZF_NET_ROOT set, the output of a command comes from `<root>/fixture/`.
void apply_fixture(const std::string& cmd, CmdResult& r) {
  if (root().empty()) return;
  auto file = [](const char* n) {
    std::string t;
    read_file(under(std::string("/fixture/") + n), t);
    return t;
  };
  auto flag = [](const char* n) { return file_exists(under(std::string("/fixture/") + n)); };
  if (cmd == kDetailCommand) r.out = file("detail.txt");
  else if (starts_with(cmd, "iwctl station ")) r.exit_code = flag("connect_fail") ? 1 : 0;
  else if (starts_with(cmd, "iwctl known-networks ")) r.exit_code = flag("forget_fail") ? 1 : 0;
  else if (starts_with(cmd, "wifictl scanlist") || cmd == "wifictl list") r.out = file("scanlist.txt");
  else if (starts_with(cmd, "wifictl has_ap_mode")) r.exit_code = flag("ap_mode") ? 0 : 1;
  else if (starts_with(cmd, "wifictl connect")) r.exit_code = flag("connect_fail") ? 1 : 0;
  else if (starts_with(cmd, "/usr/bin/usbgadget address")) r.out = file("usb_address.txt");
  else if (starts_with(cmd, "tailscale status")) r.out = file("tailscale.txt");
  else if (starts_with(cmd, "zerotier-cli")) r.out = file("zerotier.txt");
}

CmdResult run_cmd(Services& sv, const std::string& cmd, int timeout_ms) {
  CmdResult r = sv.cmd.run(cmd, timeout_ms);
  if (r.dry) apply_fixture(cmd, r);
  return r;
}

/// ApiSystem::enableWifi: `wifictl enable`, then `wifictl connect <ssid> <key> <country>` (the country is applied by
/// connect, `iw reg set`). With no SSID stored the connect step fails by design (nothing to join): not an error.
bool enable_wifi(Services& sv, const std::string& ssid, const std::string& key, const std::string& country) {
  if (run_cmd(sv, "wifictl enable", 20000).exit_code != 0) return false;
  int rc = run_cmd(sv, "wifictl connect " + shell_quote(ssid) + " " + shell_quote(key) + " " + shell_quote(country), 45000).exit_code;
  return rc == 0 || ssid.empty();
}

// ---- session cache ------------------------------------------------------------------------------------------------------

int g_ap_supported = -1;  // wifictl has_ap_mode: a property of the adapter, asked once per session (as ES does)

void query_ap_mode(Ctx& cx, const Refresh& refresh) {
  if (g_ap_supported >= 0) return;
  Services* sv = cx.sv.get();
  auto rc = std::make_shared<int>(1);
  sv->jobs.post([sv, rc] { *rc = run_cmd(*sv, "wifictl has_ap_mode", 15000).exit_code; },
                [rc, refresh] {
                  g_ap_supported = *rc == 0 ? 1 : 0;
                  refresh();
                });
}

// ---- Wi-Fi: enable / country / local play ------------------------------------------------------------------------------

void set_wifi_enabled(Ctx& cx, bool on, const Refresh& refresh) {
  cx.conf.persist("wifi.enabled", on ? "1" : "0");
  Services* sv = cx.sv.get();
  std::string ssid = cx.conf.get_or("wifi.ssid", ""), key = cx.conf.get_or("wifi.key", ""), country = cx.conf.get_or("wifi.country", "");
  auto ok = std::make_shared<bool>(true);
  if (on) {
    sv->toasts.push(tr("Turning Wi-Fi on..."));
    sv->jobs.post([=] { *ok = enable_wifi(*sv, ssid, key, country); },
                  [sv, ok, refresh] {
                    sv->toasts.push(*ok ? tr("Wi-Fi is on") : tr("Wi-Fi configuration error"));
                    refresh();
                  });
  } else {
    sv->jobs.post([sv, ok] { *ok = run_cmd(*sv, "wifictl disable", 20000).exit_code == 0; },
                  [sv, ok, refresh] {
                    sv->toasts.push(*ok ? tr("Wi-Fi is off") : tr("Wi-Fi configuration error"));
                    refresh();
                  });
  }
  query_ap_mode(cx, refresh);
  refresh();
}

void set_country(Ctx& cx, const std::string& code, const Refresh& refresh) {
  if (code == cx.conf.get_or("wifi.country", "")) return;
  cx.conf.persist("wifi.country", code);
  Services* sv = cx.sv.get();
  std::string ssid = cx.conf.get_or("wifi.ssid", ""), key = cx.conf.get_or("wifi.key", "");
  auto ok = std::make_shared<bool>(true);
  sv->jobs.post([=] { *ok = enable_wifi(*sv, ssid, key, code); },
                [sv, ok, refresh] {
                  sv->toasts.push(*ok ? tr("Wi-Fi region updated") : tr("Wi-Fi configuration error"));
                  refresh();
                });
  refresh();
}

/// GuiMenu enable_adhoc callback: keys for netplay, then restart the radio in the new mode.
void set_local_play(Ctx& cx, bool on, const Refresh& refresh) {
  cx.conf.persist("wifi.adhoc.id", cx.conf.get_or("wifi.adhoc.id", "").empty() ? "1" : cx.conf.get_or("wifi.adhoc.id", "1"));
  cx.conf.persist("global.netplay.host", "192.168.80.1");
  cx.conf.persist("global.netplay.port", "55435");
  cx.conf.persist("global.netplay.relay", "none");
  cx.conf.persist("wifi.adhoc.enabled", on ? "1" : "0");
  if (cx.conf.get_bool("wifi.enabled", false)) {
    Services* sv = cx.sv.get();
    std::string ssid = cx.conf.get_or("wifi.ssid", ""), key = cx.conf.get_or("wifi.key", ""), country = cx.conf.get_or("wifi.country", "");
    auto ok = std::make_shared<bool>(true);
    sv->toasts.push(tr("Please wait..."));
    sv->jobs.post([=] {
      run_cmd(*sv, "wifictl disable", 20000);
      *ok = enable_wifi(*sv, ssid, key, country);
    },
                  [sv, ok, refresh] {
                    sv->toasts.push(*ok ? tr("Local play mode updated") : tr("Wi-Fi configuration error"));
                    refresh();
                  });
  }
  refresh();
}

// ---- services (SSH, Samba, ...) -----------------------------------------------------------------------------------------

/// Runs ES's command sequence on a job. The setting is stored first (the boot daemons read it); a failing `systemctl`
/// is reported. `rm` of a file that is not there is not an error.
const char* fail_msg() { return tr("The service did not answer"); }

void apply_service(Ctx& cx, const char* key, bool on, std::vector<std::string> cmds, const char* toast_ok) {
  cx.conf.persist(key, on ? "1" : "0");
  Services* sv = cx.sv.get();
  auto list = std::make_shared<std::vector<std::string>>(std::move(cmds));
  auto ok = std::make_shared<bool>(true);
  sv->jobs.post([sv, list, ok] {
    for (const std::string& c : *list) {
      int rc = run_cmd(*sv, c, 30000).exit_code;
      if (rc != 0 && !starts_with(c, "rm ")) *ok = false;
    }
  },
                [sv, ok, toast_ok] { sv->toasts.push(*ok ? toast_ok : fail_msg()); });
}


// ---- the main screen ----------------------------------------------------------------------------------------------------

struct NetState {
  std::map<std::string, bool> pending;  // setting -> value shown while its job runs
  std::string gadget_pending;
  std::string ts_url;
};

bool shown(const NetState& st, Ctx& cx, const char* key) {
  auto it = st.pending.find(key);
  if (it != st.pending.end()) return it->second;
  return cx.conf.get_or(key, "") == "1";  // ES: get(key) == "1"
}

std::vector<Option> country_options() {
  std::vector<Option> v;
  v.reserve(kCountryCount + 1);
  v.push_back({"", tr("N/A")});
  for (unsigned i = 0; i < kCountryCount; ++i) v.push_back({kCountries[i].code, kCountries[i].name});
  return v;
}

std::string country_name(const std::string& code) {
  if (code.empty()) return tr("N/A");
  for (unsigned i = 0; i < kCountryCount; ++i)
    if (code == kCountries[i].code) return kCountries[i].name;
  return code;
}

void build_network(MenuBuilder& m, Ctx& cx, const std::shared_ptr<NetState>& st) {
  const Refresh refresh = m.refresh;
  const bool wifi = has_wifi();
  const bool wifi_on = cx.conf.get_bool("wifi.enabled", false);
  const bool adhoc = cx.conf.get_bool("wifi.adhoc.enabled", false);

  // ---- information
  m.header(tr("INFORMATION"));
  std::vector<std::string> ips = local_ips();
  if (ips.size() > 1) {
    std::string all;
    for (const std::string& ip : ips) all += (all.empty() ? "" : "\n") + ip;
    Entry& e = m.action(tr("IP ADDRESS"), [all]() -> Transition { return ui::message(all, nullptr, tr("AVAILABLE IP ADDRESSES:")); });
    e.value(ips[0] + " (+)");
  } else {
    m.info(tr("IP ADDRESS"), ips.empty() ? tr("NOT CONNECTED") : ips[0]);
  }
  m.info(tr("INTERNET STATUS"), ips.empty() ? tr("NOT CONNECTED") : tr("CONNECTED"));
  m.toggle(tr("SHOW NETWORK INDICATOR"), [&cx] { return cx.es.get_bool("ShowNetworkIndicator", true); },
           [&cx](bool b) {
             cx.es.set_bool("ShowNetworkIndicator", b);
             cx.es.save();
             ui_prefs::reapply(cx);  // the status bar follows at once
           });

  // ---- settings
  m.header(tr("SETTINGS"));
  m.text(tr("HOSTNAME"), [&cx] { return cx.conf.get_or("system.hostname", ""); },
         [&cx](const std::string& v) {
           std::string h(trim(v));
           if (!valid_hostname(h)) {
             cx.sv->toasts.push(tr("Use letters, digits and dashes only"));
             return;
           }
           cx.conf.persist("system.hostname", h);
           cx.sv->toasts.push(tr("Hostname saved. It applies at the next boot"), 3000);
         })
      .hint(tr("The new name is used at the next boot."));

  if (wifi) {
    m.toggle(tr("ENABLE WI-FI"), [&cx] { return cx.conf.get_bool("wifi.enabled", false); }, [&cx, refresh](bool b) { set_wifi_enabled(cx, b, refresh); });
    if (wifi_on && !adhoc) {
      m.submenu(tr("WI-FI NETWORKS"), "menu.network.wifi");
      const std::string code = cx.conf.get_or("wifi.country", "");
      Entry& c = m.action(tr("WI-FI COUNTRY"), [&cx, refresh, code]() -> Transition {
        return ui::choose(tr("WI-FI COUNTRY"), country_options(), code, [refresh](Ctx& c2, const std::string& v) { set_country(c2, v, refresh); });
      });
      c.value(country_name(code));
      c.show_arrow = true;
      c.hint(tr("Sets the radio regulations: the channels you may use."));
    }
    if (wifi_on && g_ap_supported == 1)
      m.toggle(tr("LOCAL PLAY MODE"), [&cx] { return cx.conf.get_bool("wifi.adhoc.enabled", false); }, [&cx, refresh](bool b) { set_local_play(cx, b, refresh); })
          .hint(tr("Hosts an access point for netplay with other consoles."));
  }
  if (adhoc) {
    m.choice(tr("LOCAL PLAY ID"), {{"1", tr("1 (HOST)")}, {"2", tr("2 (CLIENT 1)")}, {"3", tr("3 (CLIENT 2)")}, {"4", tr("4 (CLIENT 3)")}},
             [&cx] {
               std::string id = cx.conf.get_or("wifi.adhoc.id", "");
               return id.empty() ? std::string("1") : id;
             },
             [&cx](const std::string& v) { cx.conf.persist("wifi.adhoc.id", v); });
  }

  // ---- what is installed on this image (an entry that does not apply is not shown)
  const bool ssh = exists("/usr/sbin/sshd");
  const bool smb = exists("/usr/sbin/smbd");
  const bool http = exists("/usr/lib/systemd/system/simple-http-server.service");
  std::string udc_function;
  const bool gadget = exists("/usr/bin/usbgadget") && usb_function(udc_function);
  const bool syncthing = exists("/usr/bin/syncthing");
  const bool cloud = exists("/usr/bin/rclonectl");
  const bool wireguard = exists("/storage/.config/wireguard/wg0.conf");
  const bool tailscale = exists("/usr/sbin/tailscale");
  const bool zerotier = exists("/usr/sbin/zerotier-cli");

  if (ssh || smb || http || gadget) m.header(tr("NETWORK SERVICES"));
  if (ssh)
    m.toggle(tr("ENABLE SSH"), [&cx, st] { return shown(*st, cx, "ssh.enabled"); },
             [&cx](bool b) {
               apply_service(cx, "ssh.enabled", b,
                             b ? std::vector<std::string>{"mkdir -p /storage/.cache/services/", "touch /storage/.cache/services/sshd.conf", "systemctl enable sshd", "systemctl start sshd"}
                               : std::vector<std::string>{"systemctl stop sshd", "systemctl disable sshd", "rm /storage/.cache/services/sshd.conf"},
                             b ? tr("SSH is on") : tr("SSH is off"));
             })
        .hint(tr("Remote access to the console over the network."));
  if (smb)
    m.toggle(tr("ENABLE SAMBA"), [&cx, st] { return shown(*st, cx, "samba.enabled"); },
             [&cx](bool b) {
               apply_service(cx, "samba.enabled", b,
                             b ? std::vector<std::string>{"mkdir -p /storage/.cache/services/", "touch /storage/.cache/services/smb.conf", "systemctl start nmbd", "systemctl start smbd"}
                               : std::vector<std::string>{"systemctl stop nmbd", "systemctl stop smbd", "rm /storage/.cache/services/smb.conf"},
                             b ? tr("Samba is on") : tr("Samba is off"));
             })
        .hint(tr("Shares the games folder with other computers."));
  if (http)
    m.toggle(tr("ENABLE SIMPLE HTTP SERVER"), [&cx, st] { return shown(*st, cx, "simplehttp.enabled"); },
             [&cx](bool b) {
               apply_service(cx, "simplehttp.enabled", b,
                             {b ? "systemctl enable --now simple-http-server" : "systemctl disable --now simple-http-server"},
                             b ? tr("HTTP server is on") : tr("HTTP server is off"));
             });
  if (gadget) {
    const std::string mode = st->gadget_pending.empty() ? usb_mode_from_function(udc_function) : st->gadget_pending;
    m.choice(tr("USB GADGET FUNCTION"), {{"disabled", tr("DISABLED")}, {"network", tr("NETWORK")}, {"file_transfer", tr("FILE TRANSFER")}},
             [mode] { return mode; },
             [&cx, st, refresh](const std::string& v) {
               st->gadget_pending = v;
               Services* sv = cx.sv.get();
               auto ip = std::make_shared<std::string>();
               auto ok = std::make_shared<bool>(true);
               sv->jobs.post([sv, v, ip, ok] {
                 *ok = run_cmd(*sv, "/usr/bin/usbgadget " + v, 30000).exit_code == 0;
                 if (v == "network") *ip = first_line(run_cmd(*sv, "/usr/bin/usbgadget address", 10000).out);
               },
                             [sv, st, refresh, v, ip, ok] {
                               st->gadget_pending.clear();
                               if (!*ok) sv->toasts.push(fail_msg());
                               else if (v == "network" && !ip->empty()) sv->toasts.push(std::string(tr("USB networking is on. The console is at")) + " " + *ip, 5000);
                               refresh();
                             });
             })
        .no_cycle()
        .hint(tr("What the console offers on its USB-C port."));
  }

  if (syncthing || cloud) m.header(tr("CLOUD SERVICES"));
  if (syncthing)
    m.toggle(tr("ENABLE SYNCTHING"), [&cx, st] { return shown(*st, cx, "syncthing.enabled"); },
             [&cx](bool b) {
               apply_service(cx, "syncthing.enabled", b, {b ? "systemctl start syncthing" : "systemctl stop syncthing"},
                             b ? tr("Syncthing is on") : tr("Syncthing is off"));
             });
  if (cloud)
    m.toggle(tr("MOUNT CLOUD DRIVE"), [&cx, st] { return shown(*st, cx, "clouddrive.mounted"); },
             [&cx, st, refresh](bool b) {
               st->pending["clouddrive.mounted"] = b;
               Services* sv = cx.sv.get();
               Ctx* cxp = &cx;
               auto ok = std::make_shared<bool>(true);
               sv->jobs.post([sv, b, ok] { *ok = run_cmd(*sv, b ? "rclonectl mount" : "rclonectl unmount", 60000).exit_code == 0; },
                             [sv, cxp, st, refresh, b, ok] {
                               st->pending.erase("clouddrive.mounted");
                               cxp->conf.persist("clouddrive.mounted", (b && *ok) ? "1" : "0");
                               sv->toasts.push(*ok ? (b ? tr("Cloud drive mounted") : tr("Cloud drive unmounted")) : fail_msg());
                               refresh();
                             });
               refresh();
             });

  if (wireguard || tailscale || zerotier) m.header(tr("VPN SERVICES"));
  if (wireguard)
    m.toggle(tr("WIREGUARD VPN"), [&cx, st] { return shown(*st, cx, "wireguard.up"); },
             [&cx](bool b) {
               const std::string conf = "/storage/.config/wireguard/wg0.conf";
               apply_service(cx, "wireguard.up", b,
                             b ? std::vector<std::string>{"systemctl start connman-vpn", "wg-quick up " + conf}
                               : std::vector<std::string>{"wg-quick down " + conf, "systemctl stop connman-vpn"},
                             b ? tr("WireGuard is on") : tr("WireGuard is off"));
             });
  if (tailscale) {
    m.toggle(tr("TAILSCALE VPN"), [&cx, st] { return shown(*st, cx, "tailscale.up"); },
             [&cx, st, refresh](bool b) {
               st->pending["tailscale.up"] = b;
               Services* sv = cx.sv.get();
               Ctx* cxp = &cx;
               auto status = std::make_shared<TsStatus>();
               sv->jobs.post([sv, b, status] {
                 if (b) {
                   run_cmd(*sv, "systemctl start tailscaled", 30000);
                   run_cmd(*sv, "tailscale up --timeout=7s", 30000);
                   *status = parse_tailscale_status(run_cmd(*sv, "tailscale status", 15000).out);
                 } else {
                   run_cmd(*sv, "tailscale down", 30000);
                   run_cmd(*sv, "systemctl stop tailscaled", 30000);
                 }
               },
                             [sv, cxp, st, refresh, b, status] {
                               st->pending.erase("tailscale.up");
                               bool up = b && !status->logged_out;
                               cxp->conf.persist("tailscale.up", up ? "1" : "0");
                               st->ts_url = b && status->logged_out ? status->url : std::string();
                               if (!b) sv->toasts.push(tr("Tailscale is off"));
                               else if (up) sv->toasts.push(tr("Tailscale is on"));
                               else sv->toasts.push(st->ts_url.empty() ? tr("Tailscale could not connect") : tr("Tailscale needs you to log in"), 3000);
                               refresh();
                             });
               refresh();
             });
    if (!st->ts_url.empty()) {
      const std::string url = st->ts_url;
      m.action(tr("TAILSCALE LOGIN"), [url]() -> Transition { return ui::message(url, nullptr, tr("TAILSCALE REAUTHENTICATE:")); }).value(tr("SHOW LINK"));
    }
  }
  if (zerotier)
    m.toggle(tr("ZeroTier One"), [&cx, st] { return shown(*st, cx, "zerotier.up"); },
             [&cx, st, refresh](bool b) {
               st->pending["zerotier.up"] = b;
               Services* sv = cx.sv.get();
               Ctx* cxp = &cx;
               auto up = std::make_shared<bool>(false);
               sv->jobs.post([sv, b, up] {
                 if (b) {
                   run_cmd(*sv, "systemctl start zerotier-one", 30000);
                   *up = zerotier_running(run_cmd(*sv, "zerotier-cli -D/storage/.config/zerotier/ info", 15000).out);
                 } else {
                   run_cmd(*sv, "systemctl stop zerotier-one", 30000);
                 }
               },
                             [sv, cxp, st, refresh, b, up] {
                               st->pending.erase("zerotier.up");
                               bool on = b && *up;
                               cxp->conf.persist("zerotier.up", on ? "1" : "0");
                               sv->toasts.push(!b ? tr("ZeroTier is off") : (on ? tr("ZeroTier is on") : tr("ZeroTier could not connect")));
                               refresh();
                             });
               refresh();
             });
}

std::unique_ptr<Screen> make_network() {
  auto st = std::make_shared<NetState>();
  MenuSpec s;
  s.title = tr("NETWORK SETTINGS");
  s.title_key = N_("NETWORK SETTINGS");
  s.build = [st](MenuBuilder& m, Ctx& cx) { build_network(m, cx, st); };
  s.on_open = [](Ctx& cx, const Refresh& refresh) {
    // GuiMenu patch 0016: scan in the background while the user is still on this menu, so WI-FI NETWORKS opens already filled.
    if (!has_wifi() || !cx.conf.get_bool("wifi.enabled", false)) return;
    if (!cx.conf.get_bool("wifi.adhoc.enabled", false)) cx.sv->cmd.spawn("wifictl prescan");
    query_ap_mode(cx, refresh);
  };
  return make_menu(std::move(s));
}

// ---- WI-FI NETWORKS -----------------------------------------------------------------------------------------------------

struct ListState {
  std::vector<WifiNet> nets;
  bool scanning = true;
  std::string connecting;  // SSID being joined (empty: none)
  unsigned gen = 0;        // a scan answer older than this is dropped
};

/// `wifictl scanlist` (scan) or `wifictl list` (no scan) + the signal/security iwd knows + the saved networks, on a job.
void start_scan(Ctx& cx, const std::shared_ptr<ListState>& st, const Refresh& refresh, bool scan, bool announce) {
  struct Out {
    std::vector<std::string> names, saved;
    IwdDetail detail;
  };
  auto out = std::make_shared<Out>();
  Services* sv = cx.sv.get();
  const unsigned gen = ++st->gen;
  const std::string current = cx.conf.get_or("wifi.ssid", "");
  if (scan) st->scanning = true;
  sv->jobs.post([sv, scan, out] {
    out->names = parse_ssid_lines(run_cmd(*sv, scan ? "wifictl scanlist" : "wifictl list", 30000).out);
    out->detail = parse_iwd_details(run_cmd(*sv, kDetailCommand, 8000).out);
    out->saved = saved_networks();
  },
                [st, gen, out, refresh, current] {
                  if (gen != st->gen) return;
                  // no answer from iwd about the connected network: trust wifi.ssid if the console has an address (ES)
                  st->nets = merge_networks(out->names, out->detail, out->saved, local_ips().empty() ? std::string() : current);
                  st->scanning = false;
                  refresh();
                });
  if (announce) refresh();
}

/// Joins a network iwd already has a profile for: `iwctl station <dev> connect <ssid>` (iwd uses the key it stored), then asks
/// iwd which network it is on when it can answer (the exit code of iwctl alone is not trusted).
bool join_saved(Services& sv, const std::string& ssid) {
  const std::string dev = wifi_device();
  if (dev.empty()) return false;
  CmdResult r = run_cmd(sv, "iwctl station " + shell_quote(dev) + " connect " + shell_quote(ssid), 45000);
  if (r.exit_code != 0) return false;
  const std::string detail = run_cmd(sv, kDetailCommand, 8000).out;
  if (detail.find("@ORDERED") == std::string::npos) return true;  // iwd has no station to ask (or did not answer): the exit code is all there is
  IwdDetail d = parse_iwd_details(detail);
  return d.has_connected && d.connected.ssid == ssid;
}

/// Joins `ssid`. `saved`: iwd knows the key (`join_saved`); otherwise the pair is stored in system.cfg BEFORE
/// connecting, exactly like ES, because `wifictl enable` / the wake-up scripts read wifi.ssid and wifi.key.
void connect_async(Ctx& cx, const std::string& ssid, const std::string& key, bool saved, std::function<void(bool)> finished) {
  Services* sv = cx.sv.get();
  Ctx* cxp = &cx;
  if (!saved) {
    cx.conf.persist("wifi.ssid", ssid);
    cx.conf.persist("wifi.key", key);
  }
  const std::string country = cx.conf.get_or("wifi.country", "");
  sv->toasts.push(std::string(tr("Connecting to")) + " " + ssid + "...", 4000);  // never the key
  auto ok = std::make_shared<bool>(false);
  sv->jobs.post([sv, ssid, key, saved, country, ok] {
    *ok = saved ? join_saved(*sv, ssid) : enable_wifi(*sv, ssid, key, country);
  },
                [sv, cxp, ssid, saved, ok, finished] {
                  if (*ok && saved) {
                    cxp->conf.persist("wifi.ssid", ssid);
                    cxp->conf.persist("wifi.key", "");
                  }
                  sv->toasts.push(*ok ? tr("Connected successfully") : tr("Failed to connect"));
                  if (finished) finished(*ok);
                });
}

std::string net_value(const WifiNet& n) {
  std::string v;
  if (n.connected) v = tr("CONNECTED");
  else if (n.saved) v = tr("SAVED");
  else if (n.open()) v = tr("OPEN");
  else if (n.enterprise()) v = tr("ENTERPRISE");
  else if (!n.security.empty()) v = tr("SECURED");
  if (n.signal_pct >= 0) v += std::string(v.empty() ? "" : "  ") + std::to_string(n.signal_pct) + "%";
  return v;
}

Transition pick_network(Ctx& cx, const std::shared_ptr<ListState>& st, const Refresh& refresh, const WifiNet& net) {
  if (!st->connecting.empty()) return Transition::none();
  auto joined = [st, refresh, &cx](bool) {
    st->connecting.clear();
    start_scan(cx, st, refresh, false, false);
    refresh();
  };
  auto begin = [&cx, st, refresh, joined](const std::string& ssid, const std::string& key, bool saved) {
    st->connecting = ssid;
    connect_async(cx, ssid, key, saved, joined);
    refresh();
  };
  if (net.enterprise()) {
    cx.sv->toasts.push(tr("Enterprise networks are not supported"));
    return Transition::none();
  }
  if (net.saved) {
    const std::string ssid = net.ssid;
    // the title is the network name alone: "NETWORK OPTIONS FOR: <name>" does not fit the popup and would cut the name
    return ui::choose(ssid, {{"connect", tr("CONNECT")}, {"forget", tr("FORGET")}}, "",
                      [ssid, begin, st, refresh](Ctx& c2, const std::string& what) {
                        if (what == "connect") {
                          begin(ssid, "", true);
                          return;
                        }
                        Services* sv = c2.sv.get();
                        Ctx* cxp = &c2;
                        auto ok = std::make_shared<bool>(false);
                        // `iwctl known-networks <ssid> forget` deletes iwd's profile; the folder is read again to be sure it is gone
                        sv->jobs.post(
                            [sv, ssid, ok] {
                              CmdResult r = run_cmd(*sv, "iwctl known-networks " + shell_quote(ssid) + " forget", 15000);
                              std::vector<std::string> left = saved_networks();
                              *ok = r.exit_code == 0 && (r.dry || std::find(left.begin(), left.end(), ssid) == left.end());
                            },
                            [sv, cxp, st, refresh, ok, ssid] {
                              if (*ok && cxp->conf.get_or("wifi.ssid", "") == ssid) {
                                // wifictl connect (boot, wake-up) reads this pair: left in place it would join the network again
                                cxp->conf.persist("wifi.key", "");
                                cxp->conf.persist("wifi.ssid", "");
                              }
                              sv->toasts.push(*ok ? tr("Network forgotten") : tr("The service did not answer"));
                              start_scan(*cxp, st, refresh, false, true);
                            });
                      });
  }
  // like ES, the key is asked for every network that is not saved, open ones included (accept it empty: that is what is sent)
  ui::KeyboardOpts ko;
  ko.title = std::string(tr("WI-FI KEY FOR ")) + net.ssid;
  ko.password = true;
  ko.max_len = 64;
  const std::string ssid = net.ssid;
  return ui::keyboard(ko, "", [begin, ssid](Ctx&, const std::string& key) { begin(ssid, key, false); });
}

void build_list(MenuBuilder& m, Ctx&, const std::shared_ptr<ListState>& st) {
  const Refresh refresh = m.refresh;
  if (st->scanning) {
    m.info(tr("SEARCHING WI-FI NETWORKS"), "...");
    return;
  }
  const bool busy = !st->connecting.empty();
  if (busy) m.info(tr("CONNECTING..."), st->connecting);
  if (st->nets.empty()) m.info(tr("NO WI-FI NETWORKS FOUND"));
  for (const WifiNet& n : st->nets) {
    m.action(n.ssid, [st, refresh, n](Ctx& cx) -> Transition { return pick_network(cx, st, refresh, n); }).value(net_value(n)).with_id("net:" + n.ssid).disabled(busy);
  }
  if (!st->nets.empty()) m.separator();
  m.action(tr("REFRESH"), [st, refresh](Ctx& cx) { start_scan(cx, st, refresh, true, true); }).disabled(busy);
  m.submenu(tr("INPUT MANUALLY"), "menu.network.manual").disabled(busy);
}

std::unique_ptr<Screen> make_wifi_list() {
  auto st = std::make_shared<ListState>();
  MenuSpec s;
  s.title = tr("WI-FI NETWORKS");
  s.title_key = N_("WI-FI NETWORKS");
  s.build = [st](MenuBuilder& m, Ctx& cx) { build_list(m, cx, st); };
  s.on_open = [st](Ctx& cx, const Refresh& refresh) { start_scan(cx, st, refresh, true, false); };
  return make_menu(std::move(s));
}

// ---- INPUT MANUALLY (hidden network) ----------------------------------------------------------------------------------------

struct ManualState {
  std::string ssid, key;
  bool busy = false;
};

std::unique_ptr<Screen> make_manual() {
  auto st = std::make_shared<ManualState>();
  MenuSpec s;
  s.title = tr("INPUT MANUALLY");
  s.title_key = N_("INPUT MANUALLY");
  s.build = [st](MenuBuilder& m, Ctx&) {
    const Refresh refresh = m.refresh;
    m.text(tr("WI-FI SSID"), [st] { return st->ssid; },
           [st, refresh](const std::string& v) {
             st->ssid = std::string(trim(v));
             refresh();
           });
    m.text(tr("WI-FI KEY"), [st] { return st->key; }, [st](const std::string& v) { st->key = v; }).secret();
    if (!st->ssid.empty() && !st->busy)
      m.action(tr("CONNECT"), [st, refresh](Ctx& cx) {
        st->busy = true;
        connect_async(cx, st->ssid, st->key, false, [st, refresh](bool) {
          st->busy = false;
          refresh();
        });
        refresh();
      });
  };
  return make_menu(std::move(s));
}

}  // namespace

void reset_session_cache() { g_ap_supported = -1; }

}  // namespace netmenu

ZF_REGISTER_SCREEN("menu.network", [](Ctx&, const std::string&) { return netmenu::make_network(); });
ZF_REGISTER_SCREEN("menu.network.wifi", [](Ctx&, const std::string&) { return netmenu::make_wifi_list(); });
ZF_REGISTER_SCREEN("menu.network.manual", [](Ctx&, const std::string&) { return netmenu::make_manual(); });

}  // namespace zf
