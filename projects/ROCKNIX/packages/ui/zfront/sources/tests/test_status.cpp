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
  CHECK(bat(fake({{"rk-bat/capacity", "7"}, {"usb/online", "1"}})) == (Battery{7, false}));
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
