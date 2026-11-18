#include "imgdec.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#include "stb_image.h"
#include "src/webp/decode.h"
#include "util.hpp"

namespace zf {

// A hostile or corrupt image can announce 40000 x 40000: the header says so long before any pixel is decoded, and the RGBA
// buffer (plus the decoder's own) would exhaust the 1 GB of RAM. 16 Mpx is above any real cover or theme background.
static bool dims_ok(int w, int h) { return w > 0 && h > 0 && uint64_t(w) * uint64_t(h) <= kMaxImagePixels; }

bool decode_image(const uint8_t* data, size_t size, Img& out) {
  if (size < 12 || size > size_t(INT_MAX)) return false;
  if (std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0) {
    int w = 0, h = 0;
    if (!WebPGetInfo(data, size, &w, &h) || !dims_ok(w, h)) return false;
    uint8_t* px = WebPDecodeRGBA(data, size, &w, &h);
    if (!px) return false;
    out.w = uint32_t(w);
    out.h = uint32_t(h);
    out.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
    WebPFree(px);
    return true;
  }
  int w = 0, h = 0, comp = 0;
  if (!stbi_info_from_memory(data, int(size), &w, &h, &comp) || !dims_ok(w, h)) return false;
  unsigned char* px = stbi_load_from_memory(data, int(size), &w, &h, &comp, 4);
  if (!px) return false;
  out.w = uint32_t(w);
  out.h = uint32_t(h);
  out.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
  stbi_image_free(px);
  return true;
}

bool decode_image_file(const std::string& path, Img& out) {
  std::string buf;
  if (!read_file(path, buf)) return false;
  return decode_image(reinterpret_cast<const uint8_t*>(buf.data()), buf.size(), out);
}

static bool file_scaled(const std::string& path, uint32_t max_w, uint32_t max_h, bool cover, Img& out) {
  std::string buf;
  if (!read_file(path, buf)) return false;
  const uint8_t* data = reinterpret_cast<const uint8_t*>(buf.data());
  const size_t size = buf.size();
  if (max_w != UINT32_MAX && max_h != UINT32_MAX && size >= 12 && size <= size_t(INT_MAX) && std::memcmp(data, "RIFF", 4) == 0 &&
      std::memcmp(data + 8, "WEBP", 4) == 0) {
    int w = 0, h = 0;
    if (!WebPGetInfo(data, size, &w, &h) || !dims_ok(w, h)) return false;
    const float s = cover ? std::max(float(max_w) / float(w), float(max_h) / float(h)) : std::min(float(max_w) / float(w), float(max_h) / float(h));
    if (s < 1.0f) {
      int nw = std::max(1, int(std::lround(float(w) * s))), nh = std::max(1, int(std::lround(float(h) * s)));
      if (cover) nw = std::max<int>(nw, int(max_w)), nh = std::max<int>(nh, int(max_h));
      WebPDecoderConfig cfg;
      if (WebPInitDecoderConfig(&cfg)) {
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = nw;
        cfg.options.scaled_height = nh;
        cfg.output.colorspace = MODE_RGBA;
        if (WebPDecode(data, size, &cfg) == VP8_STATUS_OK && cfg.output.u.RGBA.rgba) {
          out.w = uint32_t(nw);
          out.h = uint32_t(nh);
          out.rgba.resize(size_t(nw) * size_t(nh) * 4);
          for (int y = 0; y < nh; ++y)
            std::memcpy(&out.rgba[size_t(y) * size_t(nw) * 4], cfg.output.u.RGBA.rgba + size_t(y) * size_t(cfg.output.u.RGBA.stride), size_t(nw) * 4);
          WebPFreeDecBuffer(&cfg.output);
          return true;
        }
        WebPFreeDecBuffer(&cfg.output);  // fall back to the plain decode below
      }
    }
  }
  return decode_image(data, size, out);
}

bool decode_image_file_fit(const std::string& path, uint32_t max_w, uint32_t max_h, Img& out) { return file_scaled(path, max_w, max_h, false, out); }
bool decode_image_file_cover(const std::string& path, uint32_t max_w, uint32_t max_h, Img& out) { return file_scaled(path, max_w, max_h, true, out); }

}  // namespace zf
