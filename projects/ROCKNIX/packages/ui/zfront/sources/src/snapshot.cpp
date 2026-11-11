#include "snapshot.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "settings.hpp"
#include "util.hpp"

namespace zf::snapshot {

namespace {
constexpr char kMagic[] = "ZFSNAP1\n";

uint32_t fnv(const uint8_t* d, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ d[i]) * 16777619u;
  return h;
}
void put32(std::string& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(char((v >> (8 * i)) & 0xFF));
}
uint32_t get32(const std::string& s, size_t p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= uint32_t(uint8_t(s[p + size_t(i)])) << (8 * i);
  return v;
}
}  // namespace

std::string Key::text() const {
  auto clean = [](std::string s) {
    for (char& c : s)
      if (c == '\n' || c == '\r') c = ' ';
    return s;
  };
  return "theme=" + clean(theme) + "|set=" + clean(theme_set) + "|lang=" + clean(lang) + "|build=" + clean(build) + "|size=" + std::to_string(w) + "x" +
         std::to_string(h);
}

std::string dir() {
  if (const char* e = std::getenv("ZF_SNAPSHOT_DIR"); e && *e) return e;
  return "/storage/.cache/zfront";
}
std::string file_path(const std::string& d) { return path_join(d, "last-frame.zfs"); }

std::string build_id() {
  struct stat st;
  if (::stat("/proc/self/exe", &st) != 0) return "?";
  return std::to_string(long(st.st_size)) + "-" + std::to_string(long(st.st_mtime));
}

uint16_t pack565(uint8_t r, uint8_t g, uint8_t b) { return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

Image downscale_half(const uint8_t* rgba, uint32_t w, uint32_t h) {
  Image im;
  if (!rgba || w < 2 || h < 2) return im;
  const uint32_t ow = w / 2, oh = h / 2;
  if (uint64_t(ow) * oh > kMaxPixels) return im;
  im.w = ow;
  im.h = oh;
  im.px.resize(size_t(ow) * oh);
  for (uint32_t y = 0; y < oh; ++y)
    for (uint32_t x = 0; x < ow; ++x) {
      unsigned s[3] = {0, 0, 0};
      for (uint32_t dy = 0; dy < 2; ++dy)
        for (uint32_t dx = 0; dx < 2; ++dx) {
          const uint8_t* p = rgba + (size_t(2 * y + dy) * w + (2 * x + dx)) * 4;
          s[0] += p[0], s[1] += p[1], s[2] += p[2];
        }
      im.px[size_t(y) * ow + x] = pack565(uint8_t(s[0] / 4), uint8_t(s[1] / 4), uint8_t(s[2] / 4));
    }
  return im;
}

bool save(const std::string& d, const Key& key, const Image& im, std::string& err) {
  if (im.w == 0 || im.h == 0 || uint64_t(im.w) * im.h > kMaxPixels || im.px.size() != size_t(im.w) * im.h) {
    err = "snapshot: bad picture";
    return false;
  }
  if (writes_blocked()) return true;
  ::mkdir(d.c_str(), 0755);  // the parent (/storage/.cache) exists; an existing folder is fine
  std::string out(kMagic);
  out += key.text();
  out.push_back('\n');
  put32(out, im.w);
  put32(out, im.h);
  std::string body;
  body.reserve(im.px.size() * 2);
  for (uint16_t v : im.px) {
    body.push_back(char(v & 0xFF));
    body.push_back(char(v >> 8));
  }
  out += body;
  put32(out, fnv(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
  return write_atomic(file_path(d), out, &err);
}

std::optional<Image> load(const std::string& d, const Key& key, std::string& why) {
  why.clear();
  struct stat st;
  const std::string path = file_path(d);
  if (::stat(path.c_str(), &st) != 0) {
    why = "no snapshot";
    return std::nullopt;
  }
  if (st.st_size <= 0 || uint64_t(st.st_size) > kMaxPixels * 2 + 4096) {
    why = "snapshot size out of bounds";
    return std::nullopt;
  }
  std::string buf;
  if (!read_file(path, buf)) {
    why = "snapshot unreadable";
    return std::nullopt;
  }
  const size_t ml = sizeof(kMagic) - 1;
  if (buf.size() < ml || std::memcmp(buf.data(), kMagic, ml) != 0) {
    why = "snapshot: not ours";
    return std::nullopt;
  }
  const size_t nl = buf.find('\n', ml);
  if (nl == std::string::npos) {
    why = "snapshot: damaged header";
    return std::nullopt;
  }
  if (buf.compare(ml, nl - ml, key.text()) != 0) {
    why = "snapshot: other theme, size, language or build";
    return std::nullopt;
  }
  const size_t p = nl + 1;
  if (buf.size() < p + 8 + 4) {
    why = "snapshot: truncated";
    return std::nullopt;
  }
  const uint32_t w = get32(buf, p), h = get32(buf, p + 4);
  if (w == 0 || h == 0 || uint64_t(w) * h > kMaxPixels || buf.size() != p + 8 + size_t(w) * h * 2 + 4) {
    why = "snapshot: wrong size";
    return std::nullopt;
  }
  const uint8_t* body = reinterpret_cast<const uint8_t*>(buf.data()) + p + 8;
  const size_t bytes = size_t(w) * h * 2;
  if (fnv(body, bytes) != get32(buf, p + 8 + bytes)) {
    why = "snapshot: checksum";
    return std::nullopt;
  }
  Image im;
  im.w = w;
  im.h = h;
  im.px.resize(size_t(w) * h);
  for (size_t i = 0; i < im.px.size(); ++i) im.px[i] = uint16_t(body[2 * i] | (body[2 * i + 1] << 8));
  return im;
}

bool Gate::poll(bool eligible, double now) {
  if (!eligible) {
    since_.reset();
    taken_ = false;
    return false;
  }
  if (!since_) since_ = now;
  if (taken_ || now - *since_ < rest_ || now - last_ < gap_) return false;
  taken_ = true;
  last_ = now;
  return true;
}

std::optional<double> Gate::deadline() const {
  if (!since_ || taken_) return std::nullopt;
  return std::max(*since_ + rest_, last_ + gap_);
}

}  // namespace zf::snapshot
