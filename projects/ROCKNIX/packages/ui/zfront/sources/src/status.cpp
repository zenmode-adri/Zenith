#include "status.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <vector>

namespace zf {

const char* Status::battery_icon(Battery b) {
  if (b.charging) return "battery-icon-charging.svg";
  if (b.percent > 75) return "battery-icon-100.svg";
  if (b.percent > 50) return "battery-icon-75.svg";
  if (b.percent > 25) return "battery-icon-50.svg";
  if (b.percent > 5) return "battery-icon-25.svg";
  return "battery-icon-0.svg";
}

const char* Status::wifi_icon() const { return wifi ? "network-icon-on.svg" : nullptr; }

Status Status::demo(const std::string& time) {
  Status s;
  s.time = time;
  s.battery = Battery{100, true};
  s.wifi = true;
  return s;
}

bool Status::parse_override(const std::string& spec, const Status& base, Status& out, std::string& err) {
  Status s = base;
  std::optional<long> pct;
  if (s.battery) pct = s.battery->percent;
  bool chg = s.battery ? s.battery->charging : false;
  size_t i = 0;
  while (i <= spec.size()) {
    size_t e = spec.find(',', i);
    if (e == std::string::npos) e = spec.size();
    std::string_view kv = trim(std::string_view(spec).substr(i, e - i));
    i = e + 1;
    if (kv.empty()) continue;
    size_t eq = kv.find('=');
    if (eq == std::string_view::npos) {
      err = "--status: '" + std::string(kv) + "' no es clave=valor";
      return false;
    }
    std::string_view k = kv.substr(0, eq), v = kv.substr(eq + 1);
    auto flag = [&](std::string_view x) { return x == "1" || x == "true" || x == "on" || x == "yes"; };
    if (k == "battery" && v == "none") pct.reset();
    else if (k == "battery") {
      long n;
      if (!parse_int(v, n) || n < 0) {
        err = "--status: battery 0-100";
        return false;
      }
      pct = n > 100 ? 100 : n;
    } else if (k == "charging") chg = flag(v);
    else if (k == "wifi") s.wifi = flag(v);
    else if (k == "time") s.time = std::string(v);
    else {
      err = "--status: clave desconocida '" + std::string(k) + "'";
      return false;
    }
  }
  s.battery.reset();
  if (pct) s.battery = Battery{uint8_t(*pct), chg};
  out = s;
  return true;
}

namespace {
using std::chrono::milliseconds;
using std::chrono::seconds;

/// Battery attribute files, opened once and re-read with pread (one syscall per value, no allocation).
class BatteryFiles {
 public:
  BatteryFiles() = default;
  BatteryFiles(const BatteryFiles&) = delete;
  BatteryFiles& operator=(const BatteryFiles&) = delete;
  ~BatteryFiles() { close_all(); }
  bool resolved() const { return cap_ >= 0; }

  /// `power_supply/battery`, else the first entry (sorted) with "bat" in its name, like ES.
  bool resolve(const std::string& sysfs) {
    close_all();
    std::string base = sysfs + "/class/power_supply";
    std::string dir = base + "/battery";
    if (!is_dir(dir)) {
      std::vector<std::string> names;
      if (DIR* d = opendir(base.c_str())) {
        while (dirent* e = readdir(d)) {
          std::string n = e->d_name;
          std::string low = n;
          std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
          if (n[0] != '.' && low.find("bat") != std::string::npos) names.push_back(n);
        }
        closedir(d);
      }
      if (names.empty()) return false;
      std::sort(names.begin(), names.end());
      dir = base + "/" + names[0];
    }
    cap_ = ::open((dir + "/capacity").c_str(), O_RDONLY | O_CLOEXEC);
    if (cap_ < 0) return false;
    st_ = ::open((dir + "/status").c_str(), O_RDONLY | O_CLOEXEC);
    cur_ = ::open((dir + "/current_avg").c_str(), O_RDONLY | O_CLOEXEC);
    return true;
  }

  /// nullopt (and unresolved again) when the battery vanished or `capacity` is garbage.
  std::optional<Battery> read() {
    if (cap_ < 0) return std::nullopt;
    long pct;
    if (!read_long(cap_, pct)) {
      close_all();
      return std::nullopt;
    }
    Battery b;
    b.percent = uint8_t(std::clamp(pct, 0L, 100L));
    // ES rule: charging unless Discharging / Not charging; else current_avg > 0
    char buf[32];
    ssize_t n = st_ >= 0 ? ::pread(st_, buf, sizeof buf - 1, 0) : -1;
    if (n > 0) {
      std::string_view s = trim(std::string_view(buf, size_t(n)));
      b.charging = !s.empty() && s != "Discharging" && s != "Not charging";
    }
    if (!b.charging && cur_ >= 0) {
      long cur;
      if (read_long(cur_, cur) && cur > 0) b.charging = true;
    }
    return b;
  }

 private:
  static bool read_long(int fd, long& out) {
    char buf[32];
    ssize_t n = ::pread(fd, buf, sizeof buf - 1, 0);
    return n > 0 && parse_int(trim(std::string_view(buf, size_t(n))), out);
  }
  void close_all() {
    for (int* f : {&cap_, &st_, &cur_})
      if (*f >= 0) ::close(*f);
    cap_ = st_ = cur_ = -1;
  }
  int cap_ = -1, st_ = -1, cur_ = -1;
};

/// Wakes ~2 times per minute: at each minute change and 30 s later (battery and WiFi are read on both).
class RealStatusSource : public StatusSource {
 public:
  RealStatusSource(std::string sysfs, StatusProbes p) : sysfs_(std::move(sysfs)), p_(p) {
    if (!p_.wifi) p_.wifi = probe_wifi;
    if (!p_.time) p_.time = probe_time;
    TimePoint now = Clock::now();
    next_slow_ = clock_deadline_ = now;
    poll(now);
  }
  const Status& status() const override { return s_; }
  void freeze(const Status& s) override {
    s_ = s;
    frozen_ = true;
  }
  std::optional<TimePoint> next_deadline() const override {
    if (frozen_) return std::nullopt;
    return std::min(next_slow_, clock_deadline_);
  }
  bool poll(TimePoint now) override {
    if (frozen_) return false;
    bool due_clock = now >= clock_deadline_, changed = false;
    if (due_clock) {
      p_.time(scratch_);  // reused buffer: "HH:MM" fits the small-string storage, no allocation
      if (scratch_ != s_.time) {
        s_.time = scratch_;
        changed = true;
      }
      clock_deadline_ = now + milliseconds(ms_to_next_minute());
    }
    if (due_clock || now >= next_slow_) {
      if (!bat_.resolved()) bat_.resolve(sysfs_);
      std::optional<Battery> b = bat_.read();
      bool w = p_.wifi();
      changed |= !(b == s_.battery) || w != s_.wifi;
      s_.battery = b;
      s_.wifi = w;
      next_slow_ = now + seconds(30);
    }
    return changed;
  }

 private:
  /// Wall-clock milliseconds to the next minute boundary (+20 ms so the new minute is already showing).
  static long ms_to_next_minute() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long into = long(ts.tv_sec % 60) * 1000 + ts.tv_nsec / 1000000;
    return 60000 - into + 20;
  }
  std::string sysfs_;
  StatusProbes p_;
  BatteryFiles bat_;
  Status s_;
  std::string scratch_;
  TimePoint next_slow_, clock_deadline_;
  bool frozen_ = false;
};
}  // namespace

bool probe_wifi() {
  ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) != 0) return false;
  bool found = false;
  for (ifaddrs* p = ifa; p && !found; p = p->ifa_next)
    found = (p->ifa_flags & IFF_UP) && !(p->ifa_flags & IFF_LOOPBACK) && p->ifa_addr && p->ifa_addr->sa_family == AF_INET;
  freeifaddrs(ifa);
  return found;
}

void probe_time(std::string& out) {
  out.clear();
  time_t t = time(nullptr);
  tzset();  // cheap when /etc/localtime did not change
  tm tmv{};
  if (!localtime_r(&t, &tmv) || tmv.tm_year + 1900 < 2000) return;  // clock not set (no useful RTC): draw nothing
  char b[16];
  int n = std::snprintf(b, sizeof b, "%02d:%02d", tmv.tm_hour % 100, tmv.tm_min % 100);
  out.assign(b, size_t(n));
}

std::optional<Battery> read_battery(const std::string& sysfs_root) {
  BatteryFiles f;
  if (!f.resolve(sysfs_root)) return std::nullopt;
  return f.read();
}

std::unique_ptr<StatusSource> make_status_source(const std::string& sysfs_root) {
  return std::make_unique<RealStatusSource>(sysfs_root, StatusProbes{});
}
std::unique_ptr<StatusSource> make_status_source(const std::string& sysfs_root, const StatusProbes& probes) {
  return std::make_unique<RealStatusSource>(sysfs_root, probes);
}

}  // namespace zf
