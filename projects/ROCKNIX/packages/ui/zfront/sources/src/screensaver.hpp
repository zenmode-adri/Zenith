// MODULE `screensaver`: what the screensaver looks like and the small pure pieces behind it. The decision "start / stop"
// lives in oscontract (inactivity timer, events, bl_power); this file only holds
//   * the mode the frame is drawn in (Off / Dim / Black) and the overlay for it,
//   * `ResumeDetector`: notices that the machine was suspended (CLOCK_BOOTTIME runs on in suspend, CLOCK_MONOTONIC does not),
//   * `next_minute`: the only wake-up a dimmed screen with a clock needs.
//
// ES parity (SystemScreenSaver::renderScreenSaver): black = opaque black over the view, dim = black at alpha 0xA0. Toasts are
// not drawn while it is on (Window::render skips them). zfront does not port the video / slideshow screensavers.
#pragma once
#include <cstdint>
#include <ctime>
#include <optional>
#include <string_view>
#include <vector>

#include "gfx.hpp"
#include "util.hpp"

namespace zf {

struct Ctx;

namespace screensaver {

enum class Mode : uint8_t { Off, Dim, Black };
constexpr uint8_t kDimAlpha = 0xA0;  // ES: 0x000000A0

Mode mode();
void set_mode(Mode m);

/// `ScreenSaverBehavior` -> what zfront does: "dim" -> Dim; "black" and everything zfront does not port (video, slideshow, a
/// missing key) -> Black. ("suspend" is handled by oscontract: it suspends instead of covering the screen.)
Mode mode_for_behavior(std::string_view behavior);

/// The overlay of the current mode (nothing when Off): one full-screen rectangle.
void draw(const Ctx& cx, std::vector<Op>& out);

/// Wall-clock milliseconds from `sec_into_minute` (0..59.999) to the next minute boundary, +20 ms so the new minute already
/// shows (same rule as the status bar's clock).
long ms_to_next_minute(double sec_into_minute);

/// Suspend detector. `check(boottime_ns - monotonic_ns)` is true once per suspend (the offset between the two clocks grew by
/// more than `threshold_ms`) and re-bases itself; `peek` only looks. The first call only records the base.
class ResumeDetector {
 public:
  explicit ResumeDetector(long threshold_ms = 1000) : threshold_ms_(threshold_ms) {}
  bool peek(long long offset_ns) const { return have_ && offset_ns - base_ > (long long)threshold_ms_ * 1000000LL; }
  bool check(long long offset_ns) {
    bool r = peek(offset_ns);
    base_ = offset_ns;
    have_ = true;
    return r;
  }
  void reset() { have_ = false; }

 private:
  long long base_ = 0;
  long threshold_ms_;
  bool have_ = false;
};

/// CLOCK_BOOTTIME - CLOCK_MONOTONIC in ns (two clock_gettime calls). Replaceable by tests.
long long clock_offset_ns();
void set_clock_offset_probe(long long (*probe)());  // nullptr restores the real one

}  // namespace screensaver
}  // namespace zf
