#include "status.hpp"

#include <functional>

#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
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
    else if (k == "bt") s.bluetooth = flag(v);
    else if (k == "time") s.time = std::string(v);
    else if (k == "frame") {  // a step of the charging animation, pinned
      long n;
      if (!parse_int(v, n) || n < 0 || n > 3) {
        err = "--status: frame 0-3";
        return false;
      }
      s.charge_frame = int8_t(n);
    } else {
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
    // the chargers (rk817-charger, usb, ac...): their `online` says whether the console is plugged in
    if (DIR* d = opendir(base.c_str())) {
      while (dirent* e = readdir(d)) {
        const std::string p = base + "/" + e->d_name;
        if (e->d_name[0] == '.' || p == dir) continue;
        int fd = ::open((p + "/online").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) online_.push_back(fd);
      }
      closedir(d);
    }
    full_latch_ = false;
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
    char buf[32];
    ssize_t n = st_ >= 0 ? ::pread(st_, buf, sizeof buf - 1, 0) : -1;
    const std::string_view s = n > 0 ? trim(std::string_view(buf, size_t(n))) : std::string_view();
    if (std::optional<bool> on = plugged()) {
      // a charger is known: plugged in is what the bolt shows (the status of the battery wanders while plugged: "Full", then
      // "Charging" again for the top-off, "Not charging" for a moment; seen on the R36 Ultra)
      b.charging = *on;
    } else {
      // ES rule: charging unless Discharging / Not charging; else current_avg > 0
      b.charging = !s.empty() && s != "Discharging" && s != "Not charging";
      if (!b.charging && cur_ >= 0) {
        long cur;
        if (read_long(cur_, cur) && cur > 0) b.charging = true;
      }
    }
    // Once full while plugged, 100 % until the charger is pulled, as on phones: the charger lets the battery sag to ~97 % and
    // tops it up again, so the gauge goes 100, 97, 98... for days on a console left plugged in.
    if (!b.charging) full_latch_ = false;
    else if (s == "Full" || b.percent >= 100) full_latch_ = true;
    else if (b.percent < 90) full_latch_ = false;  // a weak charger: the level really falls while plugged
    if (full_latch_) b.percent = 100;
    return b;
  }

 private:
  static bool read_long(int fd, long& out) {
    char buf[32];
    ssize_t n = ::pread(fd, buf, sizeof buf - 1, 0);
    return n > 0 && parse_int(trim(std::string_view(buf, size_t(n))), out);
  }
  /// Any charger online; nullopt when there is no charger (or none can be read).
  std::optional<bool> plugged() const {
    bool any = false, on = false;
    for (int fd : online_) {
      long v;
      if (!read_long(fd, v)) continue;
      any = true;
      on = on || v == 1;
    }
    return any ? std::optional<bool>(on) : std::nullopt;
  }
  void close_all() {
    for (int* f : {&cap_, &st_, &cur_})
      if (*f >= 0) ::close(*f);
    cap_ = st_ = cur_ = -1;
    for (int fd : online_) ::close(fd);
    online_.clear();
  }
  int cap_ = -1, st_ = -1, cur_ = -1;
  std::vector<int> online_;
  bool full_latch_ = false;
};

/// The kernel's uevents (NETLINK_KOBJECT_UEVENT, group 1): non-blocking, -1 when it cannot be opened.
int open_uevents() {
  int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
  if (fd < 0) return -1;
  sockaddr_nl a{};
  a.nl_family = AF_NETLINK;
  a.nl_groups = 1;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

/// Wakes ~2 times per minute: at each minute change and 30 s later (battery and WiFi are read on both).
class RealStatusSource : public StatusSource {
 public:
  RealStatusSource(std::string sysfs, StatusProbes p) : sysfs_(std::move(sysfs)), p_(p) {
    if (!p_.wifi) p_.wifi = probe_wifi;
    if (!p_.time) p_.time = probe_time;
    ue_ = p_.uevent_fd == -2 ? open_uevents() : p_.uevent_fd;
    TimePoint now = Clock::now();
    next_slow_ = clock_deadline_ = now;
    poll(now);
  }
  ~RealStatusSource() override {
    if (ue_ >= 0) ::close(ue_);
  }
  RealStatusSource(const RealStatusSource&) = delete;
  RealStatusSource& operator=(const RealStatusSource&) = delete;
  const Status& status() const override { return s_; }
  int wake_fd() const override { return ue_; }
  void invalidate() override {
    clock_deadline_ = next_slow_ = TimePoint{};
    net_until_ = Clock::now() + seconds(20);
  }
  void freeze(const Status& s) override {
    s_ = s;
    frozen_ = true;
  }
  std::optional<TimePoint> next_deadline() const override {
    if (frozen_) return std::nullopt;
    return std::min(next_slow_, clock_deadline_);
  }
  bool poll(TimePoint now) override {
    const int hits = drain_uevents();  // read even when frozen: the loop polls the descriptor and would spin on it
    if (frozen_) return false;
    const bool power = hits != 0;
    if (hits & kNet) net_until_ = now + seconds(20);  // an address (DHCP) comes some seconds after the link: look again every 2 s
    bool due_clock = now >= clock_deadline_, changed = false;
    if (due_clock) {
      p_.time(scratch_);  // reused buffer: "HH:MM" fits the small-string storage, no allocation
      if (scratch_ != s_.time) {
        s_.time = scratch_;
        changed = true;
      }
      clock_deadline_ = now + milliseconds(ms_to_next_minute());
    }
    if (due_clock || now >= next_slow_ || power) {  // a charger plugged or pulled shows at once, not at the next half minute
      if (!bat_.resolved()) bat_.resolve(sysfs_);
      std::optional<Battery> b = bat_.read();
      bool w = p_.wifi();
      bool bt = p_.bluetooth ? p_.bluetooth() : probe_bluetooth(sysfs_);
      changed |= !(b == s_.battery) || w != s_.wifi || bt != s_.bluetooth;
      s_.battery = b;
      s_.wifi = w;
      s_.bluetooth = bt;
      next_slow_ = now + (now < net_until_ ? seconds(2) : seconds(30));
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
  /// What arrived since the last call: kPower (a power_supply uevent, or some were lost), kNet (the network or Bluetooth came or went).
  static constexpr int kPower = 1, kNet = 2;
  int drain_uevents() {
    if (ue_ < 0) return 0;
    int hit = 0;
    char buf[2048];
    for (;;) {
      ssize_t n = ::recv(ue_, buf, sizeof buf, MSG_DONTWAIT);
      if (n > 0) {
        static constexpr char kKey[] = "SUBSYSTEM=power_supply";
        static constexpr char kNetKey[] = "SUBSYSTEM=net", kRf[] = "SUBSYSTEM=rfkill", kBt[] = "SUBSYSTEM=bluetooth";
        if (::memmem(buf, size_t(n), kKey, sizeof kKey - 1)) hit |= kPower;
        if (::memmem(buf, size_t(n), kNetKey, sizeof kNetKey - 1) || ::memmem(buf, size_t(n), kRf, sizeof kRf - 1) || ::memmem(buf, size_t(n), kBt, sizeof kBt - 1)) hit |= kNet;
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && errno == ENOBUFS) {  // the socket overflowed (zfront was away during a game): read the battery again
        hit |= kPower | kNet;
        continue;
      }
      break;
    }
    return hit;
  }
  std::string sysfs_;
  StatusProbes p_;
  int ue_ = -1;
  BatteryFiles bat_;
  Status s_;
  std::string scratch_;
  TimePoint next_slow_, clock_deadline_, net_until_{};
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

bool probe_bluetooth(const std::string& sysfs_root) {
  const auto has_entries = [](const std::string& dir, const std::function<bool(const char*)>& pick) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    bool found = false;
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      if (pick(e->d_name)) {
        found = true;
        break;
      }
    }
    closedir(d);
    return found;
  };
  if (!has_entries(sysfs_root + "/class/bluetooth", [](const char*) { return true; })) return false;
  const std::string rf = sysfs_root + "/class/rfkill/";
  return has_entries(rf, [&](const char* name) {
    std::string type, state;
    if (!read_file(rf + name + "/type", type) || type.rfind("bluetooth", 0) != 0) return false;
    return read_file(rf + name + "/state", state) && state.rfind("1", 0) == 0;
  });
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
