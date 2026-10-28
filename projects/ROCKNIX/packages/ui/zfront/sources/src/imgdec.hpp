// Image file decoding: PNG/JPEG with stb_image, WebP (lossless, lossy, alpha) with the vendored libwebp decoder.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "gfx.hpp"

namespace zf {

/// Decode from memory; the format comes from the magic bytes. Output is straight-alpha RGBA8.
/// Images with more pixels than this are refused before decoding.
constexpr uint64_t kMaxImagePixels = 16u << 20;
bool decode_image(const uint8_t* data, size_t size, Img& out);
bool decode_image_file(const std::string& path, Img& out);

}  // namespace zf
