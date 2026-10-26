#include "menu_network_parse.hpp"

#include <algorithm>
#include <cstdlib>

#include "util.hpp"

namespace zf {
namespace netmenu {

// Same lookup of the station as wifictl / iwd_get-networks (first /net/connman/iwd/<phy>/<dev>), then the two properties.
// `2>/dev/null` and the final `true` keep a missing station from printing errors or failing.
const char* const kDetailCommand =
    "sp=$(busctl --no-pager tree net.connman.iwd 2>/dev/null | grep -o '/net/connman/iwd/[0-9]*/[0-9]*$' | head -1); "
    "if [ -n \"$sp\" ]; then "
    "echo @ORDERED; busctl --no-pager call net.connman.iwd \"$sp\" net.connman.iwd.Station GetOrderedNetworks 2>/dev/null; "
    "echo @CONNECTED; busctl --no-pager get-property net.connman.iwd \"$sp\" net.connman.iwd.Station ConnectedNetwork 2>/dev/null; "
    "fi; true";

namespace {

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/// Every double-quoted token of `s`, in order.
std::vector<std::string_view> quoted(std::string_view s) {
  std::vector<std::string_view> v;
  size_t i = 0;
  while (true) {
    size_t a = s.find('"', i);
    if (a == std::string_view::npos) break;
    size_t b = s.find('"', a + 1);
    if (b == std::string_view::npos) break;
    v.push_back(s.substr(a + 1, b - a - 1));
    i = b + 1;
  }
  return v;
}

}  // namespace

std::vector<std::string> parse_ssid_lines(std::string_view out) {
  std::vector<std::string> v;
  size_t i = 0;
  while (i <= out.size()) {
    size_t e = out.find('\n', i);
    if (e == std::string_view::npos) e = out.size();
    std::string_view line = out.substr(i, e - i);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty() && std::find(v.begin(), v.end(), line) == v.end()) v.emplace_back(line);
    i = e + 1;
  }
  return v;
}

bool known_network_name(std::string_view file, std::string& ssid, std::string& security) {
  size_t dot = file.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return false;
  std::string_view type = file.substr(dot + 1), stem = file.substr(0, dot);
  if (type != "psk" && type != "open" && type != "8021x") return false;
  std::string s;
  if (stem[0] == '=') {
    std::string_view hex = stem.substr(1);
    if (hex.empty() || hex.size() % 2 != 0) return false;
    for (size_t i = 0; i < hex.size(); i += 2) {
      int h = hexval(hex[i]), l = hexval(hex[i + 1]);
      if (h < 0 || l < 0) return false;
      s.push_back(char(h * 16 + l));
    }
  } else {
    s.assign(stem);
  }
  ssid = std::move(s);
  security.assign(type);
  return true;
}

bool split_net_path(std::string_view path, std::string& ssid, std::string& security) {
  if (path.substr(0, 17) != "/net/connman/iwd/") return false;
  size_t slash = path.rfind('/');
  std::string_view last = path.substr(slash + 1);
  size_t us = last.rfind('_');
  if (us == std::string_view::npos || us % 2 != 0) return false;
  std::string_view hex = last.substr(0, us);
  std::string s;
  s.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    int h = hexval(hex[i]), l = hexval(hex[i + 1]);
    if (h < 0 || l < 0) return false;
    s.push_back(char(h * 16 + l));
  }
  ssid = std::move(s);
  security.assign(last.substr(us + 1));
  return true;
}

IwdDetail parse_iwd_details(std::string_view out) {
  IwdDetail d;
  std::string_view ordered = out, connected;
  size_t mo = out.find("@ORDERED"), mc = out.find("@CONNECTED");
  if (mo != std::string_view::npos || mc != std::string_view::npos) {
    ordered = mo == std::string_view::npos ? std::string_view() : out.substr(mo + 8, (mc != std::string_view::npos && mc > mo ? mc : out.size()) - mo - 8);
    if (mc != std::string_view::npos) connected = out.substr(mc + 10);
  }
  // `a(on) 2 "/path" -4500 "/path" -7100`: after each quoted path comes its integer
  size_t pos = 0;
  while (true) {
    size_t a = ordered.find('"', pos);
    if (a == std::string_view::npos) break;
    size_t b = ordered.find('"', a + 1);
    if (b == std::string_view::npos) break;
    IwdNet n;
    bool ok = split_net_path(ordered.substr(a + 1, b - a - 1), n.ssid, n.security);
    size_t k = b + 1;
    while (k < ordered.size() && ordered[k] == ' ') ++k;
    size_t e = k;
    while (e < ordered.size() && (ordered[e] == '-' || (ordered[e] >= '0' && ordered[e] <= '9'))) ++e;
    long sig = 0;
    if (e > k) parse_int(ordered.substr(k, e - k), sig);
    n.signal = int(sig);
    if (ok) d.nets.push_back(std::move(n));
    pos = e > b ? e : b + 1;
  }
  // `o "/path"`
  for (std::string_view q : quoted(connected)) {
    IwdNet n;
    if (split_net_path(q, n.ssid, n.security)) {
      d.has_connected = true;
      d.connected = std::move(n);
      break;
    }
  }
  return d;
}

int signal_percent(int centi_dbm) {
  int dbm = centi_dbm / 100;
  return std::clamp(2 * (dbm + 100), 0, 100);
}

std::vector<WifiNet> merge_networks(const std::vector<std::string>& names, const IwdDetail& detail, const std::vector<std::string>& saved,
                                    const std::string& fallback_connected) {
  std::vector<std::string> list = names;
  if (list.empty())
    for (const IwdNet& n : detail.nets)
      if (!n.ssid.empty() && std::find(list.begin(), list.end(), n.ssid) == list.end()) list.push_back(n.ssid);
  std::vector<WifiNet> out;
  for (const std::string& ssid : list) {
    WifiNet w;
    w.ssid = ssid;
    for (const IwdNet& n : detail.nets) {
      if (n.ssid != ssid) continue;  // the strongest variant comes first
      w.security = n.security;
      w.signal_pct = signal_percent(n.signal);
      break;
    }
    w.connected = detail.has_connected ? detail.connected.ssid == ssid : (!fallback_connected.empty() && fallback_connected == ssid);
    w.saved = std::find(saved.begin(), saved.end(), ssid) != saved.end();
    out.push_back(std::move(w));
  }
  for (const std::string& s : saved) {
    if (std::any_of(out.begin(), out.end(), [&](const WifiNet& w) { return w.ssid == s; })) continue;
    WifiNet w;
    w.ssid = s;
    w.saved = true;
    w.connected = detail.has_connected ? detail.connected.ssid == s : (!fallback_connected.empty() && fallback_connected == s);
    out.push_back(std::move(w));
  }
  return out;
}

TsStatus parse_tailscale_status(std::string_view out) {
  TsStatus t;
  static const std::string_view kPrompt = "Log in at: ";
  size_t i = 0;
  while (i <= out.size()) {
    size_t e = out.find('\n', i);
    if (e == std::string_view::npos) e = out.size();
    std::string_view line = out.substr(i, e - i);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.substr(0, kPrompt.size()) == kPrompt) t.url.assign(trim(line.substr(kPrompt.size())));
    if (line.find("Logged out.") != std::string_view::npos) t.logged_out = true;
    i = e + 1;
  }
  return t;
}

bool zerotier_running(std::string_view out) {
  // ES keeps the verdict of the LAST line, so the two-line error of zerotier-cli reads as "running" there; here any line
  // with the error means the daemon does not answer.
  bool any = false;
  size_t i = 0;
  while (i <= out.size()) {
    size_t e = out.find('\n', i);
    if (e == std::string_view::npos) e = out.size();
    std::string_view line = trim(out.substr(i, e - i));
    if (!line.empty()) {
      any = true;
      if (line.find("Error connecting to the ZeroTier") != std::string_view::npos) return false;
    }
    i = e + 1;
  }
  return any;
}

bool valid_hostname(std::string_view s) {
  if (s.empty() || s.size() > 63 || s.front() == '-' || s.back() == '-') return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}

std::string usb_mode_from_function(std::string_view f) {
  std::string_view m = trim(f);
  if (m == "cdc") return "network";
  if (m == "mtp") return "file_transfer";
  return "disabled";
}

std::string first_line(std::string_view s) {
  size_t e = s.find('\n');
  return std::string(trim(s.substr(0, e)));
}

}  // namespace netmenu
}  // namespace zf
