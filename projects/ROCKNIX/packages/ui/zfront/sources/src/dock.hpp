// MODULE `dock`: the plug-in notice and the bedside clock. PURE LOGIC: no SDL, no Ctx, no sysfs, no clock reads. The caller hands in the time
// (`Clock`) and what it knows (`Env`), so `tests/test_dock.cpp` runs it on the PC with made-up time.
//
// What it decides
//   * "the charger is plugged in", filtered (`plugged`, `take_plug_event`): the state of the battery wanders while plugged (R36S: the chip does
//     not see the charger; "charging" comes from the kernel's Charging / Full, and Full -> Discharging -> Full for 8 s was seen). Plugging needs
//     `kPlugConfirmMs` steady, UNplugging needs `kUnplugConfirmMs` steady (longer than any flicker seen), the first `kStartupGraceMs` after the
//     first reading never raise the event (already plugged at start), and two events are at least `kEventCooldownMs` apart.
//   * when the notice (the pill "Charging 78 %") is on screen and when it asks for a frame (`anim_active`, `anim_t`, `next_deadline`).
//   * the bedside clock's own anti burn-in drift (`burn_in_offset`, in grid cells) and its minute (`ms_to_next_minute`).
// WHEN the clock comes is NOT here: it is the screensaver's inactivity timer (oscontract.cpp), which takes `DockClock` minutes instead of
// `ScreenSaverTime` while `plugged()` and shows `screensaver::Mode::Clock`. So busy jobs, games, suspend and "the first key only wakes it" are the
// screensaver's tested rules and the two never overlap.
//
// Times are milliseconds. `Clock::mono_ms` is a monotonic clock; `Clock::wall_ms` is the real time (epoch ms).
#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace zf::dock {

using Ms = int64_t;

struct Clock {
  Ms mono_ms = 0;
  Ms wall_ms = 0;
};

constexpr Ms kPlugConfirmMs = 3000;     // plugged state must be steady this long to count
constexpr Ms kUnplugConfirmMs = 15000;  // unplugged state must be steady this long (longer than the 8 s flicker seen)
constexpr Ms kStartupGraceMs = 10000;   // no plug event this soon after the first reading
constexpr Ms kEventCooldownMs = 60000;  // two plug events at least this far apart
constexpr Ms kAnimMs = 2400;            // length of the notice
constexpr Ms kFrameMs = 33;             // ~30 fps while it moves
constexpr Ms kResampleMs = 2000;        // a change is due but only stale data is in: ask for a re-read, look again soon
constexpr Ms kMinuteSlackMs = 20;       // past the boundary, so the new minute already shows (same as screensaver::ms_to_next_minute)
constexpr int kDefaultIdleMinutes = 0;  // off unless chosen: like ES, a plugged-in console sleeps as always

constexpr int kShiftX = 2, kShiftY = 1;  // burn-in drift, in grid cells, each way

/// `DockClock` setting: "off" | "1" | "5" | "10" (anything else, or empty: 5). Returns 0 for Off.
int parse_minutes(std::string_view s);

struct Offset {
  int dx = 0, dy = 0;  // cells
};
/// Where the clock sits this minute, in cells from its centred position. The cycle is 9 minutes long and no two minutes in a row are alike.
Offset burn_in_offset(int64_t wall_minute);

/// Milliseconds from `wall_ms` to the next minute boundary, + `kMinuteSlackMs`.
Ms ms_to_next_minute(Ms wall_ms);

struct Config {
  bool motion = true;       // Ctx::ui_motion: without it the notice is one still frame and a redraw at its end
  bool charge_anim = true;  // false: the plug-in event is still counted but nothing is drawn
};

/// What the caller knows at this moment.
struct Env {
  bool plugged = false;  // the kernel says Charging / Full, or a charger is online (Battery::plugged)
  bool quiet = false;     // a game runs or the screensaver is on (oscontract): no notice, the person is not looking
  /// When `plugged` was actually READ from the kernel (monotonic ms), or -1 = "just now". The status source re-reads the battery only every
  /// ~30 s or on a uevent: between reads `update` is called with the same old value, and that must not confirm anything (an 8 s blip
  /// sampled once would otherwise be "steady" for 15 s). A change is confirmed only by a FRESH reading at least the confirm time after the
  /// first one that showed it.
  Ms sample_ms = -1;
  /// `plugged` comes from the charger's own `online` file (Battery::from_charger): an UNplug counts at once. The 15 s wait is for the
  /// R36S, whose gauge flickers; on the R36 Ultra pulling the cable for 1.4 s and plugging it back showed no notice (the unplug was never
  /// confirmed, so the plug was not a new one).
  bool reliable = false;
};

class Dock {
 public:
  void configure(const Config& c);
  const Config& config() const { return cfg_; }

  /// Once per loop iteration (cheap, allocation free). True when the frame must be redrawn: a notice frame is due or it just ended.
  bool update(const Clock& now, const Env& env);

  /// True when a change is waiting for a fresh reading (its confirm time has passed): the caller should ask the status source to read the
  /// battery again. Cheap.
  bool wants_fresh_sample(const Clock& now) const { return raw_ != stable_ && now.mono_ms >= raw_since_ + confirm_ms(raw_); }

  /// True once per confirmed plug-in (and only when the notice is allowed: not while quiet, not within the first seconds).
  bool take_plug_event();

  bool plugged() const { return stable_; }
  bool anim_active(const Clock& now) const { return anim_on_ && now.mono_ms - anim_start_ < kAnimMs; }
  /// 0..1 through the notice (0 when none).
  float anim_t(const Clock& now) const;

  /// The loop's next wake-up for this module (monotonic ms), or nullopt (the loop sleeps: zero wake-ups at rest).
  std::optional<Ms> next_deadline(const Clock& now) const;

 private:
  Ms confirm_ms(bool to_plugged) const { return to_plugged ? kPlugConfirmMs : (reliable_ ? 0 : kUnplugConfirmMs); }
  bool reliable_ = false;

  Config cfg_;
  bool known_ = false;
  bool raw_ = false, stable_ = false;
  Ms raw_since_ = 0, first_ = 0, last_sample_ = -1;
  bool has_event_ = false;
  Ms last_event_ = 0;
  bool event_pending_ = false;
  bool anim_on_ = false;
  Ms anim_start_ = 0;
  bool redraw_ = false;
};

}  // namespace zf::dock
