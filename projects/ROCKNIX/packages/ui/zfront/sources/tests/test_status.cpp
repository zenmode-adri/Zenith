#include <filesystem>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

#include "check.hpp"
#include "status.hpp"

using namespace zf;
using namespace std::chrono_literals;

static std::string g_root;
static int g_n = 0;

static void put(const std::string& root, const std::string& rel, const std::string& content) {
  std::string path = root + "/" + rel;
  for (size_t i = root.size() + 1; i < path.size(); ++i)
    if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
  std::ofstream(path, std::ios::trunc) << content;
}

static std::string fake(std::initializer_list<std::pair<const char*, const char*>> files) {
  std::string root = g_root + "/s" + std::to_string(g_n++);
  mkdir(root.c_str(), 0755);
  put(root, "class/power_supply/.keep", "");
  for (auto& f : files) put(root, std::string("class/power_supply/") + f.first, f.second);
  return root;
}

static void write_all(const std::string& p, const char* text) {
  std::ofstream(p) << text;
}
static bool g_wifi = true;
static std::string g_time = "10:00";

int main() {
  char tmpl[] = "/tmp/zf-status-XXXXXX";
  g_root = mkdtemp(tmpl);

  // battery: discharging, charging, full, not charging, current_avg, other name, none, garbage, clamp
  auto bat = [](const std::string& r) { return read_battery(r); };
  CHECK(bat(fake({{"battery/capacity", "23\n"}, {"battery/status", "Discharging\n"}})) == (Battery{23, false}));
  CHECK(bat(fake({{"battery/capacity", "80"}, {"battery/status", "Charging"}})) == (Battery{80, true}));
  CHECK(bat(fake({{"battery/capacity", "100"}, {"battery/status", "Full"}})) == (Battery{100, true}));
  CHECK(bat(fake({{"battery/capacity", "50"}, {"battery/status", "Not charging"}})) == (Battery{50, false}));
  CHECK(bat(fake({{"battery/capacity", "50"}, {"battery/status", "Discharging"}, {"battery/current_avg", "120000"}})) ==
        (Battery{50, true}));
  CHECK(bat(fake({{"battery/capacity", "50"}, {"battery/status", "Discharging"}, {"battery/current_avg", "-90000"}})) ==
        (Battery{50, false}));
  CHECK(bat(fake({{"rk-bat/capacity", "7"}, {"usb/online", "1"}})) == (Battery{7, true}));  // plugged in: the charger says so
  // a charger is known: its `online` decides, whatever the battery's status says while plugged (R36 Ultra: "Charging" for the
  // top-off, "Not charging" for a moment) or unplugged (R36S: "Not charging" and "Discharging" alternate)
  CHECK(bat(fake({{"battery/capacity", "97"}, {"battery/status", "Charging"}, {"rk817-charger/online", "1"}})) == (Battery{97, true}));
  CHECK(bat(fake({{"battery/capacity", "97"}, {"battery/status", "Not charging"}, {"rk817-charger/online", "1"}})) == (Battery{97, true}));
  CHECK(bat(fake({{"battery/capacity", "60"}, {"battery/status", "Not charging"}, {"rk817-charger/online", "0"}})) == (Battery{60, false}));
  CHECK(bat(fake({{"battery/capacity", "60"}, {"battery/status", "Charging"}, {"rk817-charger/online", "0"}, {"battery/current_avg", "5000"}})) ==
        (Battery{60, false}));
  CHECK(bat(fake({{"battery/capacity", "60"}, {"ac/online", "0"}, {"usb/online", "1"}})) == (Battery{60, true}));  // any charger
  CHECK(bat(fake({{"battery/capacity", "99"}, {"battery/status", "Full"}, {"rk817-charger/online", "1"}})) == (Battery{100, true}));
  CHECK(!bat(fake({{"usb/online", "1"}})).has_value());
  CHECK(!bat(fake({})).has_value());
  CHECK(!bat(fake({{"battery/capacity", "abc"}})).has_value());
  CHECK_EQ(bat(fake({{"battery/capacity", "250"}}))->percent, 100);

  // icon thresholds (ES): >75 >50 >25 >5, charging has its own, wifi icon only when connected
  auto ic = [](int p, bool c) { return std::string(Status::battery_icon(Battery{uint8_t(p), c})); };
  CHECK(ic(100, false) == "battery-icon-100.svg");
  CHECK(ic(76, false) == "battery-icon-100.svg");
  CHECK(ic(75, false) == "battery-icon-75.svg");
  CHECK(ic(51, false) == "battery-icon-75.svg");
  CHECK(ic(50, false) == "battery-icon-50.svg");
  CHECK(ic(26, false) == "battery-icon-50.svg");
  CHECK(ic(25, false) == "battery-icon-25.svg");
  CHECK(ic(6, false) == "battery-icon-25.svg");
  CHECK(ic(5, false) == "battery-icon-0.svg");
  CHECK(ic(0, false) == "battery-icon-0.svg");
  CHECK(ic(3, true) == "battery-icon-charging.svg");
  Status s;
  CHECK(s.wifi_icon() == nullptr);
  s.wifi = true;
  CHECK(std::string(s.wifi_icon()) == "network-icon-on.svg");
  {  // Bluetooth: an adapter and its switch on; without either, nothing
    Status o;
    std::string e2;
    CHECK(Status::parse_override("bt=1", Status{}, o, e2));
    CHECK(o.bluetooth);
    char tmpl[] = "/tmp/zf-test-bt-XXXXXX";
    const std::string r = mkdtemp(tmpl);
    CHECK(!probe_bluetooth(r));
    CHECK(mkdir((r + "/class").c_str(), 0755) == 0);
    CHECK(mkdir((r + "/class/bluetooth").c_str(), 0755) == 0);
    CHECK(mkdir((r + "/class/bluetooth/hci0").c_str(), 0755) == 0);
    CHECK(mkdir((r + "/class/rfkill").c_str(), 0755) == 0);
    CHECK(mkdir((r + "/class/rfkill/rfkill0").c_str(), 0755) == 0);
    CHECK(!probe_bluetooth(r));
    write_all(r + "/class/rfkill/rfkill0/type", "bluetooth\n");
    write_all(r + "/class/rfkill/rfkill0/state", "0\n");
    CHECK(!probe_bluetooth(r));
    write_all(r + "/class/rfkill/rfkill0/state", "1\n");
    CHECK(probe_bluetooth(r));
    write_all(r + "/class/rfkill/rfkill0/type", "wlan\n");
    CHECK(!probe_bluetooth(r));
    std::error_code ec;
    std::filesystem::remove_all(r, ec);
  }

  // override parser
  Status base = Status::demo("12:04"), out;
  std::string err;
  CHECK(Status::parse_override("battery=23,charging=0,wifi=0,time=07:13", base, out, err));
  CHECK(out == (Status{"07:13", Battery{23, false}, false}));
  CHECK(Status::parse_override("battery=none", base, out, err) && !out.battery);
  CHECK(!Status::parse_override("foo=1", base, out, err));
  CHECK(!Status::parse_override("battery", base, out, err));

  // real probes do not crash; the time is empty or HH:MM
  std::string t;
  probe_time(t);
  CHECK(t.empty() || (t.size() == 5 && t[2] == ':'));
  (void)probe_wifi();

  // monitor: nothing is re-read before its deadline; changes are reported once
  std::string r = fake({{"battery/capacity", "60"}, {"battery/status", "Discharging"}});
  StatusProbes pr;
  pr.wifi = [] { return g_wifi; };
  pr.time = [](std::string& o) { o = g_time; };
  auto src = make_status_source(r, pr);
  CHECK(src->status().battery == (Battery{60, false}));
  CHECK(src->status().wifi);
  CHECK(src->status().time == "10:00");
  put(r, "class/power_supply/battery/capacity", "59");
  TimePoint t0 = Clock::now();
  CHECK(!src->poll(t0));
  CHECK_EQ(int(src->status().battery->percent), 60);
  auto dl = src->next_deadline();
  CHECK(dl && *dl > t0 && *dl <= t0 + 61s);
  // 30 s later: battery refreshed (fd kept open, value re-read) and wifi change seen
  g_wifi = false;
  CHECK(src->poll(t0 + 31s));
  CHECK_EQ(int(src->status().battery->percent), 59);
  CHECK(!src->status().wifi);
  CHECK(!src->poll(t0 + 31s));  // same values: no redraw
  // minute change
  g_time = "10:01";
  CHECK(src->poll(t0 + 62s));
  CHECK(src->status().time == "10:01");
  // battery removed: status loses it; comes back when it reappears
  put(r, "class/power_supply/battery/capacity", "garbage");
  CHECK(src->poll(t0 + 100s));
  CHECK(!src->status().battery);
  put(r, "class/power_supply/battery/capacity", "42");
  CHECK(src->poll(t0 + 140s));
  CHECK(src->status().battery == (Battery{42, false}));
  // clock not set: empty time, nothing drawn
  g_time.clear();
  CHECK(src->poll(t0 + 200s));
  CHECK(src->status().time.empty());
  // deadlines stay within a minute (~2 wake-ups per minute)
  CHECK(*src->next_deadline() <= t0 + 200s + 61s);

  // a charger too weak for the load: the level really falls while plugged, and below 90 % the bar stops saying 100 %
  {
    std::string fr = fake({{"battery/capacity", "100"}, {"battery/status", "Full"}, {"rk817-charger/online", "1"}});
    StatusProbes fp = pr;
    fp.uevent_fd = -1;
    auto fs = make_status_source(fr, fp);
    TimePoint f0 = Clock::now();
    CHECK(fs->status().battery == (Battery{100, true}));
    put(fr, "class/power_supply/battery/capacity", "92");
    put(fr, "class/power_supply/battery/status", "Charging");
    fs->poll(f0 + 31s);
    CHECK(fs->status().battery == (Battery{100, true}));
    put(fr, "class/power_supply/battery/capacity", "85");
    fs->poll(f0 + 62s);
    CHECK(fs->status().battery == (Battery{85, true}));
  }

  // full while plugged: 100 % until the charger is pulled (the charger lets it sag to ~97 % and tops it up, for days)
  {
    std::string fr = fake({{"battery/capacity", "100"}, {"battery/status", "Full"}, {"rk817-charger/online", "1"}});
    StatusProbes fp = pr;
    fp.uevent_fd = -1;
    auto fs = make_status_source(fr, fp);
    TimePoint f0 = Clock::now();
    CHECK(fs->status().battery == (Battery{100, true}));
    put(fr, "class/power_supply/battery/capacity", "97");
    put(fr, "class/power_supply/battery/status", "Charging");
    CHECK(!fs->poll(f0 + 31s));
    CHECK(fs->status().battery == (Battery{100, true}));
    put(fr, "class/power_supply/battery/status", "Not charging");  // a moment between top-offs: still plugged, still full
    CHECK(!fs->poll(f0 + 62s));
    CHECK(fs->status().battery == (Battery{100, true}));
    put(fr, "class/power_supply/rk817-charger/online", "0");  // pulled: the real level
    put(fr, "class/power_supply/battery/status", "Discharging");
    CHECK(fs->poll(f0 + 93s));
    CHECK(fs->status().battery == (Battery{97, false}));
    put(fr, "class/power_supply/rk817-charger/online", "1");  // plugged again below full: the real level, charging
    put(fr, "class/power_supply/battery/status", "Charging");
    CHECK(fs->poll(f0 + 124s));
    CHECK(fs->status().battery == (Battery{97, true}));
  }

  // plugging or pulling the charger shows at once: the kernel's power_supply uevent makes the next poll read the battery,
  // without waiting for the half minute; other uevents do not
  {
    std::string ur = fake({{"battery/capacity", "80"}, {"battery/status", "Discharging"}, {"rk817-charger/online", "0"}});
    int sv[2];
    CHECK_EQ(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, sv), 0);
    StatusProbes up = pr;
    up.uevent_fd = sv[0];
    auto us = make_status_source(ur, up);
    CHECK_EQ(us->wake_fd(), sv[0]);
    TimePoint u0 = Clock::now();
    CHECK(us->status().battery == (Battery{80, false}));
    put(ur, "class/power_supply/rk817-charger/online", "1");
    put(ur, "class/power_supply/battery/status", "Charging");
    CHECK(!us->poll(u0));  // no uevent, before the deadline: not read yet
    static const char kInput[] = "change@/devices/platform/x\0ACTION=change\0SUBSYSTEM=input\0";
    CHECK(::send(sv[1], kInput, sizeof kInput, 0) > 0);
    CHECK(!us->poll(u0));  // another subsystem: nothing read
    static const char kPower[] = "change@/devices/platform/rk817-charger\0ACTION=change\0SUBSYSTEM=power_supply\0POWER_SUPPLY_ONLINE=1\0";
    CHECK(::send(sv[1], kPower, sizeof kPower, 0) > 0);
    CHECK(us->poll(u0));
    CHECK(us->status().battery == (Battery{80, true}));
    char b;
    CHECK(::recv(sv[0], &b, 1, MSG_DONTWAIT) < 0);  // drained: the loop does not spin on it
    // the network or Bluetooth coming or going shows at once too, and an address that arrives after the link is seen within 2 s
    g_wifi = !g_wifi;
    static const char kNet[] = "add@/devices/virtual/net/wlan0\0ACTION=change\0SUBSYSTEM=net\0";
    CHECK(::send(sv[1], kNet, sizeof kNet, 0) > 0);
    CHECK(us->poll(u0));
    CHECK(us->status().wifi == g_wifi);
    CHECK(*us->next_deadline() <= u0 + 3s);
    g_wifi = !g_wifi;
    CHECK(us->poll(u0 + 3s));  // not a uevent: the quick follow-up poll
    CHECK(us->status().wifi == g_wifi);
    // frozen (harness): still drained
    us->freeze(us->status());
    CHECK(::send(sv[1], kPower, sizeof kPower, 0) > 0);
    CHECK(!us->poll(u0));
    CHECK(::recv(sv[0], &b, 1, MSG_DONTWAIT) < 0);
    ::close(sv[1]);  // sv[0] belongs to the source
  }

  // back from a suspend the deadlines (a clock that stood still) are far away: `invalidate` makes the next poll read everything
  {
    std::string vr = fake({{"battery/capacity", "70"}, {"battery/status", "Discharging"}});
    StatusProbes vp = pr;
    vp.uevent_fd = -1;
    auto vs = make_status_source(vr, vp);
    TimePoint v0 = Clock::now();
    g_time = "10:05";
    g_wifi = !g_wifi;
    put(vr, "class/power_supply/battery/capacity", "69");
    CHECK(!vs->poll(v0));  // not due: old values
    vs->invalidate();
    CHECK(vs->poll(v0));
    CHECK(vs->status().time == "10:05");
    CHECK_EQ(int(vs->status().battery->percent), 69);
    CHECK(vs->status().wifi == g_wifi);
  }

  // freeze pins the values and removes the deadline
  Status fz{"01:02", std::nullopt, false};
  src->freeze(fz);
  CHECK(!src->poll(t0 + 1000s));
  CHECK(src->status() == fz);
  CHECK(!src->next_deadline().has_value());

  std::string cmd = "rm -rf " + g_root;
  if (std::system(cmd.c_str()) != 0) {}
  return zt::done();
}
