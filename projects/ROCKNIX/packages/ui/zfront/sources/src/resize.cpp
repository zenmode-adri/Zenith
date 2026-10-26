#include "resize.hpp"

#include <algorithm>
#include <cmath>

namespace zf {

namespace {

float kernel(Filter f, float x) {
  float a = std::fabs(x);
  if (f == Filter::Triangle) return a < 1.0f ? 1.0f - a : 0.0f;
  // Catmull-Rom = Mitchell-Netravali with B=0, C=0.5
  const float b = 0.0f, c = 0.5f;
  float k;
  if (a < 1.0f) k = (12.0f - 9.0f * b - 6.0f * c) * a * a * a + (-18.0f + 12.0f * b + 6.0f * c) * a * a + (6.0f - 2.0f * b);
  else if (a < 2.0f) k = (-b - 6.0f * c) * a * a * a + (6.0f * b + 30.0f * c) * a * a + (-12.0f * b - 48.0f * c) * a + (8.0f * b + 24.0f * c);
  else k = 0.0f;
  return k / 6.0f;
}

float support(Filter f) { return f == Filter::Triangle ? 1.0f : 2.0f; }

uint8_t to_u8(float v) {
  v = std::min(std::max(v, 0.0f), 255.0f);
  return uint8_t(std::lround(v));
}

// One axis. `vertical`: resample rows (`n_in` -> `n_out` rows of `other` columns), else columns.
void pass(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, uint32_t w, uint32_t h, uint32_t nw, uint32_t nh, bool vertical, Filter f) {
  const uint32_t n_in = vertical ? h : w, n_out = vertical ? nh : nw;
  const uint32_t other = vertical ? w : h;
  float ratio = float(n_in) / float(n_out);
  float sratio = ratio < 1.0f ? 1.0f : ratio;
  float src_support = support(f) * sratio;
  std::vector<float> ws;
  for (uint32_t o = 0; o < n_out; ++o) {
    float input = (float(o) + 0.5f) * ratio;
    long left = long(std::floor(input - src_support));
    left = std::min(std::max(left, 0L), long(n_in) - 1);
    long right = long(std::ceil(input + src_support));
    right = std::min(std::max(right, left + 1), long(n_in));
    input -= 0.5f;
    ws.clear();
    float sum = 0;
    for (long i = left; i < right; ++i) {
      float w_ = kernel(f, (float(i) - input) / sratio);
      ws.push_back(w_);
      sum += w_;
    }
    for (float& w_ : ws) w_ /= sum;
    for (uint32_t k = 0; k < other; ++k) {
      float t[4] = {0, 0, 0, 0};
      for (size_t i = 0; i < ws.size(); ++i) {
        size_t idx = vertical ? (size_t(left + long(i)) * w + k) : (size_t(k) * w + size_t(left + long(i)));
        const uint8_t* p = &in[idx * 4];
        for (int c = 0; c < 4; ++c) t[c] += float(p[c]) * ws[i];
      }
      size_t oi = vertical ? (size_t(o) * nw + k) : (size_t(k) * nw + o);
      for (int c = 0; c < 4; ++c) out[oi * 4 + size_t(c)] = to_u8(t[c]);
    }
  }
}

}  // namespace

std::vector<uint8_t> resize_rgba(const uint8_t* src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, Filter f) {
  std::vector<uint8_t> in(src, src + size_t(sw) * sh * 4);
  if (sw == dw && sh == dh) return in;
  std::vector<uint8_t> mid(size_t(sw) * dh * 4);
  pass(in, mid, sw, sh, sw, dh, true, f);
  std::vector<uint8_t> out(size_t(dw) * dh * 4);
  pass(mid, out, sw, dh, dw, dh, false, f);
  return out;
}

}  // namespace zf
