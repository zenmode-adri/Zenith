#include "screensaver.hpp"

#include <time.h>

#include "app.hpp"

namespace zf {
namespace screensaver {

namespace {
Mode g_mode = Mode::Off;
long long (*g_probe)() = nullptr;
}  // namespace

Mode mode() { return g_mode; }
void set_mode(Mode m) { g_mode = m; }

Mode mode_for_behavior(std::string_view behavior) { return behavior == "dim" ? Mode::Dim : Mode::Black; }

void draw(const Ctx& cx, std::vector<Op>& out) {
  if (g_mode == Mode::Off) return;
  out.push_back(Op::rect(0, 0, cx.w, cx.h, {0, 0, 0, g_mode == Mode::Dim ? kDimAlpha : uint8_t(255)}));
}

long ms_to_next_minute(double sec_into_minute) {
  if (sec_into_minute < 0) sec_into_minute = 0;
  if (sec_into_minute >= 60) sec_into_minute = 59.999;
  return 60000 - long(sec_into_minute * 1000.0) + 20;
}

long long clock_offset_ns() {
  if (g_probe) return g_probe();
  timespec b, m;
  clock_gettime(CLOCK_BOOTTIME, &b);
  clock_gettime(CLOCK_MONOTONIC, &m);
  return (long long)(b.tv_sec - m.tv_sec) * 1000000000LL + (b.tv_nsec - m.tv_nsec);
}

void set_clock_offset_probe(long long (*probe)()) { g_probe = probe; }

}  // namespace screensaver
}  // namespace zf
