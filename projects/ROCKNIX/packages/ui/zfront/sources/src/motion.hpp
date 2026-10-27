// Small shared helpers for UI motion: easing and in-place fade / shift of an already built range of draw operations.
// Everything here only touches position and alpha, which the renderer draws for free (no new image, no extra pass).
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

#include "gfx.hpp"

namespace zf::motion {

/// Ease-out cubic, `t` in [0, 1].
inline float ease_out(float t) {
  const float u = 1.0f - std::clamp(t, 0.0f, 1.0f);
  return 1.0f - u * u * u;
}

/// Frame-rate independent first-order approach: `x` moves towards `to` closing `rate` of the distance per second.
inline float approach(float x, float to, float rate, float dt) { return x + (to - x) * (1.0f - std::exp(-rate * dt)); }

/// Where the last `status_bar` call put its ops (indices into the vector it appended to; empty = it drew none since the reset).
/// A screen that fades / slides in or out has the same bar as the screen below it: `App` keeps that bar in place so the two do not
/// show as a moving ghost of each other.
struct OpRange {
  size_t from = 0, to = 0;
};
OpRange& status_range();

/// Linear mix of two colours (`t` = 0 gives `a`, 1 gives `b`), alpha included.
inline Color mix(Color a, Color b, float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  Color c = a;
  for (int i = 0; i < 4; ++i) c[i] = uint8_t(std::lround(float(a[i]) + (float(b[i]) - float(a[i])) * t));
  return c;
}

/// Multiplies the alpha of the Rect / Image operations in `ops[from, ...)` by `a` (0..1).
void fade(std::vector<Op>& ops, size_t from, float a);

/// Moves the operations in `ops[from, ...)` down by `dy` pixels, except the ones that cover the whole `w` x `h` screen (the
/// background / scrim of a window: it only fades, it must not slide and show an edge).
void shift_y(std::vector<Op>& ops, size_t from, float dy, float w, float h);

}  // namespace zf::motion
