// MODULE `status` (owner: status agent). System status for the top-right bar: battery, WiFi and clock.
// Contract with the rest: the loop asks `StatusSource::next_deadline()` to know when to wake, calls `poll(now)` and
// redraws only if it returns true; `App` copies `status()` into `Ctx::status`. No child processes, no polling.
//
// Skeleton: `FixedStatusSource` (demo values). The status agent writes the real source in status.cpp and returns it
// from `make_status_source()`; nothing else needs to change.
#pragma once
#include <memory>
#include <optional>
#include <string>

#include "util.hpp"

namespace zf {

struct Battery {
  uint8_t percent = 100;
  bool charging = false;
  bool operator==(const Battery& o) const { return percent == o.percent && charging == o.charging; }
};

struct Status {
  /// "HH:MM"; empty when the clock is not set (nothing is drawn).
  std::string time;
  /// None when the device has no battery (nothing is drawn).
  std::optional<Battery> battery;
  bool wifi = false;

  bool operator==(const Status& o) const { return time == o.time && battery == o.battery && wifi == o.wifi; }
  bool operator!=(const Status& o) const { return !(*this == o); }

  /// Theme icon files (ES thresholds: >75, >50, >25, >5, else empty; charging has its own).
  static const char* battery_icon(Battery b);
  /// nullptr while disconnected: with WiFi off nothing is drawn (user decision, like ES).
  const char* wifi_icon() const;
  /// The deterministic bar of the harness (`12:04`, 100 % charging, WiFi on).
  static Status demo(const std::string& time = "12:04");
  /// Harness override `battery=23,charging=0,wifi=0,time=07:13` (`battery=none` hides the battery); missing keys
  /// keep `base`.
  static bool parse_override(const std::string& spec, const Status& base, Status& out, std::string& err);
};

class StatusSource {
 public:
  virtual ~StatusSource() = default;
  virtual const Status& status() const = 0;
  /// Re-read what is due; true when `status()` changed (the bar must be redrawn).
  virtual bool poll(TimePoint now) = 0;
  /// When the loop must wake up for the bar (nullopt: never).
  virtual std::optional<TimePoint> next_deadline() const = 0;
  /// Pin the status (harness `--status`).
  virtual void freeze(const Status& s) = 0;
};

/// The source the app uses (`sysfs_root` is "/sys" on the device).
std::unique_ptr<StatusSource> make_status_source(const std::string& sysfs_root);

/// Probes of the real source, replaceable in tests. `time` writes "HH:MM" (empty: clock not set).
struct StatusProbes {
  bool (*wifi)() = nullptr;
  void (*time)(std::string& out) = nullptr;
};
std::unique_ptr<StatusSource> make_status_source(const std::string& sysfs_root, const StatusProbes& probes);
/// Real probes: any up non-loopback interface with IPv4 (getifaddrs); local time via tzset+localtime_r (year >= 2000).
bool probe_wifi();
void probe_time(std::string& out);
/// Battery under `<sysfs>/class/power_supply`; nullopt without battery. One-shot helper (tests).
std::optional<Battery> read_battery(const std::string& sysfs_root);

}  // namespace zf
