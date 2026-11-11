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
  /// A Bluetooth adapter is present and switched on (only the remade themes draw it).
  bool bluetooth = false;
  /// XMB while charging: the cells lit in the PSP's charging animation (0-3), stepped by `App`; -1: the cells show the level.
  int8_t charge_frame = -1;

  bool operator==(const Status& o) const {
    return time == o.time && battery == o.battery && wifi == o.wifi && bluetooth == o.bluetooth && charge_frame == o.charge_frame;
  }
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
  /// A descriptor that becomes readable when the status may have changed (the kernel's uevents: a charger plugged or pulled),
  /// or -1. The loop only polls it; `poll` reads it.
  virtual int wake_fd() const { return -1; }
  /// The machine slept: everything is old, read it all again at the next `poll` (the WiFi too, which comes back some seconds later).
  virtual void invalidate() {}
};

/// The source the app uses (`sysfs_root` is "/sys" on the device).
std::unique_ptr<StatusSource> make_status_source(const std::string& sysfs_root);

/// Probes of the real source, replaceable in tests. `time` writes "HH:MM" (empty: clock not set).
struct StatusProbes {
  bool (*wifi)() = nullptr;
  void (*time)(std::string& out) = nullptr;
  bool (*bluetooth)() = nullptr;
  /// -2: the kernel's uevent socket (NETLINK_KOBJECT_UEVENT); -1: none; >= 0: this descriptor (tests). The source closes it.
  int uevent_fd = -2;
};
std::unique_ptr<StatusSource> make_status_source(const std::string& sysfs_root, const StatusProbes& probes);
/// Real probes: any up non-loopback interface with IPv4 (getifaddrs); local time via tzset+localtime_r (year >= 2000).
bool probe_wifi();
/// An adapter under `<sysfs>/class/bluetooth` and a bluetooth rfkill switch in state 1 (no adapter, or switched off: false).
bool probe_bluetooth(const std::string& sysfs_root);
void probe_time(std::string& out);
/// Battery under `<sysfs>/class/power_supply`; nullopt without battery. One-shot helper (tests).
std::optional<Battery> read_battery(const std::string& sysfs_root);

}  // namespace zf
