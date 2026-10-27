#include "motion.hpp"

namespace zf::motion {

OpRange& status_range() {
  static OpRange r;
  return r;
}

void fade(std::vector<Op>& ops, size_t from, float a) {
  a = std::clamp(a, 0.0f, 1.0f);
  if (a >= 1.0f) return;
  for (size_t i = from; i < ops.size(); ++i) {
    Op& o = ops[i];
    if (o.kind == Op::Rect || o.kind == Op::Image) o.color[3] = uint8_t(std::lround(float(o.color[3]) * a));
  }
}

void shift_y(std::vector<Op>& ops, size_t from, float dy, float w, float h) {
  if (dy == 0.0f) return;
  for (size_t i = from; i < ops.size(); ++i) {
    Op& o = ops[i];
    if (o.kind == Op::Unclip) continue;
    if (o.kind == Op::Rect && o.w >= w - 1.0f && o.h >= h - 1.0f) continue;
    o.y += dy;
  }
}

}  // namespace zf::motion
