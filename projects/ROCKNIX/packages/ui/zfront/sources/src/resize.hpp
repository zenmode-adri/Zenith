// Separable image resampling (same kernels and edge handling as the Rust `image` crate used by the prototype):
// vertical pass first, 8-bit rounding between passes.
#pragma once
#include <cstdint>
#include <vector>

namespace zf {

enum class Filter { Triangle, CatmullRom };

/// RGBA8 -> RGBA8, channels resampled independently (premultiply before calling if the alpha matters).
std::vector<uint8_t> resize_rgba(const uint8_t* src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, Filter f);

}  // namespace zf
