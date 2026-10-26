#include "imgdec.hpp"

#include <cstring>

#include "stb_image.h"
#include "src/webp/decode.h"
#include "util.hpp"

namespace zf {

bool decode_image(const uint8_t* data, size_t size, Img& out) {
  if (size < 12) return false;
  if (std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0) {
    int w = 0, h = 0;
    uint8_t* px = WebPDecodeRGBA(data, size, &w, &h);
    if (!px) return false;
    out.w = uint32_t(w);
    out.h = uint32_t(h);
    out.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
    WebPFree(px);
    return true;
  }
  int w = 0, h = 0, comp = 0;
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
