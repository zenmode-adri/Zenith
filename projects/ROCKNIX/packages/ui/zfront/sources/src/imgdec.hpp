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
/// Like `decode_image_file`, but a picture bigger than `max_w x max_h` comes out already reduced to fit inside it (same shape).
/// A WebP is scaled while it is decoded: the full-size RGBA (8 MB for 1920x1080) is never built and the reduction costs no second pass.
bool decode_image_file_fit(const std::string& path, uint32_t max_w, uint32_t max_h, Img& out);
/// Same, for a picture that will be cropped to `max_w x max_h`: it comes out reduced to the smallest size that still covers the box.
bool decode_image_file_cover(const std::string& path, uint32_t max_w, uint32_t max_h, Img& out);

}  // namespace zf
