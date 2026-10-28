#include "imgdec.hpp"

#include <climits>
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

}  // namespace zf
