// Pure parsing helpers of the network menu (no I/O, no UI): output of `wifictl`, of iwd through busctl, of tailscale and
// zerotier, hostnames, USB gadget modes. Kept apart so the tests feed them real samples.
//
// Where the samples come from (all of them are in the OS image):
//  * `wifictl scanlist|list` prints one SSID per line, strongest first (iwd_get-networks: `busctl ... GetOrderedNetworks`,
//    the SSID is the hex part of the object path decoded back to bytes).
//  * That busctl call also carries what the script throws away: the signal (100 x dBm) and the security type (the suffix
//    of the object path). The menu asks iwd once more, with `kDetailCommand`, to show the signal and the padlock.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace zf {
namespace netmenu {

/// One network as iwd reports it in `Station.GetOrderedNetworks`.
struct IwdNet {
  std::string ssid;
  std::string security;  // "open", "psk", "8021x", "wep"... (suffix of the object path); empty when unknown
  int signal = 0;        // 100 x dBm, as iwd gives it (-4500 = -45 dBm)
};

struct IwdDetail {
  std::vector<IwdNet> nets;  // strongest first
  bool has_connected = false;
  IwdNet connected;  // Station.ConnectedNetwork (signal not known)
};

/// Shell command that prints the two properties above between the `@ORDERED` / `@CONNECTED` markers. It prints nothing
/// when iwd has no station (no Wi-Fi, access point mode) and never fails.
extern const char* const kDetailCommand;

/// Lines of `wifictl scanlist|list|list_saved`: trailing CR removed, empty lines dropped, duplicates removed (order kept).
std::vector<std::string> parse_ssid_lines(std::string_view out);

/// Name of a file of iwd's state folder (`/storage/.cache/iwd`, one per network it has a profile for) -> SSID and security:
/// "Casa 5G.psk" -> "Casa 5G"; "=4361736132.psk" (iwd writes `=` + hex when the SSID has anything but letters, digits, space,
/// `-` and `_`) -> "Casa2". False for anything else (`ap`, `.ap` profiles, stray files).
bool known_network_name(std::string_view file, std::string& ssid, std::string& security);

/// `/net/connman/iwd/0/3/4361736132_psk` -> ssid "Casa2", security "psk". False for anything that is not a network path.
bool split_net_path(std::string_view path, std::string& ssid, std::string& security);

/// Output of `kDetailCommand` (or of a bare `busctl call ... GetOrderedNetworks`).
IwdDetail parse_iwd_details(std::string_view out);

/// 0..100 from iwd's 100 x dBm (linear between -100 dBm and -50 dBm, the usual conversion).
int signal_percent(int centi_dbm);

struct WifiNet {
  std::string ssid;
  std::string security;   // see IwdNet; empty = unknown
  int signal_pct = -1;    // -1 = unknown / not in range
  bool connected = false;
  bool saved = false;
  bool open() const { return security == "open"; }
  bool enterprise() const { return security == "8021x"; }
};

/// The list the screen shows: `names` (what ES shows, in ES order) enriched with the iwd detail, plus the saved networks
/// that are out of range (ES does the same). `fallback_connected`: SSID to mark as connected when iwd did not say.
std::vector<WifiNet> merge_networks(const std::vector<std::string>& names, const IwdDetail& detail, const std::vector<std::string>& saved,
                                    const std::string& fallback_connected);

struct TsStatus {
  bool logged_out = false;
  std::string url;  // "Log in at: <url>"
};
/// `tailscale status` (ES `IsTailscaleUp`).
TsStatus parse_tailscale_status(std::string_view out);
/// `zerotier-cli info`: false when the daemon does not answer (ES `IsZeroTierUp`).
bool zerotier_running(std::string_view out);

/// `[A-Za-z0-9-]{1,63}`, no leading or trailing dash (what the boot script can use as a hostname).
bool valid_hostname(std::string_view s);

/// Content of `/sys/class/udc/<udc>/function` -> the mode names of `usbgadget` (ES `getUsbGadgetMode`).
std::string usb_mode_from_function(std::string_view function_file);

/// First line of `s`, trimmed.
std::string first_line(std::string_view s);

}  // namespace netmenu
}  // namespace zf
