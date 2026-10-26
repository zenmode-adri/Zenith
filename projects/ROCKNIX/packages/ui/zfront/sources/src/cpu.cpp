#include "cpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

#include "resize.hpp"

namespace zf {

namespace {

struct Clip {
  bool on = false;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

inline int iround(float v) { return int(std::lround(v)); }

void blend(Canvas& cv, const Clip& clip, int x, int y, const float src[4]) {
  if (x < 0 || y < 0 || x >= int(cv.w) || y >= int(cv.h) || src[3] <= 0.0f) return;
  if (clip.on && (x < clip.x0 || y < clip.y0 || x >= clip.x1 || y >= clip.y1)) return;
  size_t i = (size_t(y) * cv.w + size_t(x)) * 4;
  float a = src[3];
  for (int c = 0; c < 3; ++c) {
    float d = float(cv.px[i + size_t(c)]);
    float v = std::round(src[c] * a + d * (1.0f - a));
    cv.px[i + size_t(c)] = uint8_t(std::min(std::max(v, 0.0f), 255.0f));
  }
  float da = float(cv.px[i + 3]) / 255.0f;
  float v = std::round((a + da * (1.0f - a)) * 255.0f);
  cv.px[i + 3] = uint8_t(std::min(std::max(v, 0.0f), 255.0f));
}

// Premultiplied-alpha resize cache (resizing straight alpha bleeds the colour of transparent pixels).
using Key = std::tuple<ImgId, uint32_t, uint32_t>;
Sampling g_sampling = Sampling::CatmullRom;

const std::vector<uint8_t>& resized(std::map<Key, std::vector<uint8_t>>& cache, const Assets& a, ImgId id, uint32_t w, uint32_t h) {
  Key k{id, w, h};
  auto it = cache.find(k);
  if (it != cache.end()) return it->second;
  const Img& src = a.img(id);
  std::vector<uint8_t> pm = src.rgba;
  for (size_t i = 0; i < pm.size(); i += 4) {
    uint32_t al = pm[i + 3];
    for (int c = 0; c < 3; ++c) pm[i + size_t(c)] = uint8_t((uint32_t(pm[i + size_t(c)]) * al + 127) / 255);
  }
  std::vector<uint8_t> out = (src.w == w && src.h == h) ? std::move(pm) : resize_rgba(pm.data(), src.w, src.h, w, h, g_sampling == Sampling::Triangle ? Filter::Triangle : Filter::CatmullRom);
  return cache.emplace(k, std::move(out)).first->second;
}

}  // namespace

Canvas cpu_render(const std::vector<Op>& ops, const Assets& a, uint32_t w, uint32_t h, Sampling sampling) {
  g_sampling = sampling;
  Canvas cv;
  cv.w = w;
  cv.h = h;
  cv.px.assign(size_t(w) * h * 4, 0);
  Clip clip;
  std::map<Key, std::vector<uint8_t>> cache;
  for (const Op& op : ops) {
    switch (op.kind) {
      case Op::Clip:
        clip = {true, iround(op.x), iround(op.y), iround(op.x + op.w), iround(op.y + op.h)};
        break;
      case Op::Unclip:
        clip.on = false;
        break;
      case Op::Rect: {
        int x0 = iround(op.x), y0 = iround(op.y), x1 = iround(op.x + op.w), y1 = iround(op.y + op.h);
        float col[4] = {float(op.color[0]), float(op.color[1]), float(op.color[2]), float(op.color[3]) / 255.0f};
        for (int yy = y0; yy < y1; ++yy)
          for (int xx = x0; xx < x1; ++xx) blend(cv, clip, xx, yy, col);
        break;
      }
      case Op::Image: {
        if (a.img(op.id).rgba.empty()) break;
        int x0 = iround(op.x), y0 = iround(op.y);
        uint32_t dw = uint32_t(std::max(iround(op.x + op.w) - x0, 1)), dh = uint32_t(std::max(iround(op.y + op.h) - y0, 1));
        const std::vector<uint8_t>& im = resized(cache, a, op.id, dw, dh);
        float ta = float(op.color[3]) / 255.0f;
        for (uint32_t sy = 0; sy < dh; ++sy) {
          for (uint32_t sx = 0; sx < dw; ++sx) {
            const uint8_t* p = &im[(size_t(sy) * dw + sx) * 4];
            float al = float(p[3]) / 255.0f * ta;
            if (al <= 0.0f) continue;
            // p is premultiplied: undo it so `blend` (straight alpha) gets the colour back
            float inv = p[3] == 0 ? 0.0f : 255.0f / float(p[3]);
            float col[4] = {std::min(float(p[0]) * inv, 255.0f) * float(op.color[0]) / 255.0f,
                            std::min(float(p[1]) * inv, 255.0f) * float(op.color[1]) / 255.0f,
                            std::min(float(p[2]) * inv, 255.0f) * float(op.color[2]) / 255.0f, al};
            blend(cv, clip, x0 + int(sx), y0 + int(sy), col);
          }
        }
        break;
      }
    }
  }
  return cv;
}

// ---- PNG writer (stored deflate)

namespace {
uint32_t crc_table[256];
bool crc_init = false;
uint32_t crc32(const uint8_t* d, size_t n, uint32_t crc = 0xFFFFFFFFu) {
  if (!crc_init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      crc_table[i] = c;
    }
    crc_init = true;
  }
  for (size_t i = 0; i < n; ++i) crc = crc_table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}
void put32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(uint8_t(v >> 24));
  o.push_back(uint8_t(v >> 16));
  o.push_back(uint8_t(v >> 8));
  o.push_back(uint8_t(v));
}
void chunk(std::vector<uint8_t>& o, const char* type, const std::vector<uint8_t>& data) {
  put32(o, uint32_t(data.size()));
  std::vector<uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  o.insert(o.end(), td.begin(), td.end());
  put32(o, crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
}
}  // namespace

bool save_png(const Canvas& cv, const std::string& path, std::string& err) {
  std::vector<uint8_t> raw;
  raw.reserve((size_t(cv.w) * 4 + 1) * cv.h);
  for (uint32_t y = 0; y < cv.h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), cv.px.begin() + long(size_t(y) * cv.w * 4), cv.px.begin() + long(size_t(y + 1) * cv.w * 4));
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  size_t pos = 0;
  while (pos < raw.size() || raw.empty()) {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n & 0xFF));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n & 0xFF));
    z.push_back(uint8_t((~n >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
    pos += n;
    if (last) break;
  }
  uint32_t s1 = 1, s2 = 0;
  for (uint8_t b : raw) {
    s1 = (s1 + b) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  put32(z, (s2 << 16) | s1);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  put32(ihdr, cv.w);
  put32(ihdr, cv.h);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8 bit RGBA
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    err = path + ": cannot create";
    return false;
  }
  bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  if (!ok) err = path + ": write failed";
  return ok;
}

}  // namespace zf
