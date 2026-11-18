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
  /// A charger is connected: a charger `online`, or the kernel's own status says Charging / Full. Unlike `charging` it does not stop at what the
  /// charger file says (the R36S's chip does not see the charger, but the gauge reports Charging / Full). Only the bedside clock and the plug-in
  /// notice (dock.hpp) read it, so it is not part of the comparison (a change of it alone does not redraw the bar).
  bool plugged = false;
  /// `plugged` comes from the charger's own `online` file (R36 Ultra): reliable, no flicker to filter. False when it is guessed from the
  /// battery's status (R36S: the RK817 does not see its charger).
  bool from_charger = false;
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
  /// The day as XMB's corner box shows it ("dd/mm"). Empty: read from the clock when drawn (the device). Set only by `demo`, so the
  /// pictures of the tests do not change with the day they run (the XMB hashes of test_theme_sel failed every day but the one they were made).
  std::string date;

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
  /// How many times the battery was really read from the kernel so far. A caller that wants fresh data (the plug-in filter of dock.hpp) compares
  /// it between two polls: the source re-reads only every ~30 s or on a uevent, and the same old value must not count as a new reading.
  virtual unsigned battery_reads() const { return 0; }
  /// Read the battery (and WiFi) at the next `poll` instead of waiting for the half-minute; nothing else is reset.
  virtual void reread() {}
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
/// An adapter under `<sysfs>/class/bluetooth`, a bluetooth rfkill switch in state 1 and the bluetooth service running
/// (no adapter, blocked, or switched off: false).
bool probe_bluetooth(const std::string& sysfs_root);
/// Something that changes the bar without a kernel event (the Bluetooth switch starts or stops a service) asks for an
/// immediate re-read; the app takes the request in `poll_status`.
void request_status_refresh();
bool take_status_refresh();
void probe_time(std::string& out);
/// Battery under `<sysfs>/class/power_supply`; nullopt without battery. One-shot helper (tests).
std::optional<Battery> read_battery(const std::string& sysfs_root);

}  // namespace zf
