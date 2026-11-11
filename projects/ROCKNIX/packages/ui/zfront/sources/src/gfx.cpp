#include <atomic>
#include "gfx.hpp"

#include <cstring>

#include <unordered_map>

#include <mutex>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "imgdec.hpp"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "resize.hpp"
#include "text.hpp"
#include "util.hpp"

namespace zf {

static uint64_t next_assets_uid() {
  static std::atomic<uint64_t> n{1};
  return n.fetch_add(1);
}

Assets::Assets() : uid_(next_assets_uid()), font_(new Font()) {}
Assets::~Assets() = default;
Assets::Assets(Assets&&) noexcept = default;
Assets& Assets::operator=(Assets&&) noexcept = default;

bool Assets::load_font(const std::string& path, std::string& err, const std::vector<std::string>& theme_fallbacks) {
  ellipsis_cache_.clear();  // cuts were measured with the old face
  return font_->load(path, err, theme_fallbacks);
}
bool Assets::load_alt_font(const std::string& path, std::string& err) {
  auto f = std::make_unique<Font>();
  if (!f->load(path, err)) return false;
  alt_ = std::move(f);
  ellipsis_cache_.clear();
  return true;
}

ImgId Assets::push(std::string key, Img&& img) {
  bytes_ += img.rgba.size();
  cpu_bytes_ += img.rgba.size();
  img.state = 0;
  imgs_.push_back(std::move(img));
  last_used_.push_back(frame_);
  ImgId id = ImgId(imgs_.size() - 1);
  by_key_[key] = id;
  keys_.push_back(std::move(key));
  return id;
}

bool Assets::lookup(const std::string& key, ImgId& id) const {
  auto it = by_key_.find(key);
  if (it == by_key_.end()) return false;
  id = it->second;
  return true;
}

void Assets::end_frame(const std::vector<Op>& ops, size_t budget) {
  ++frame_;
  for (const Op& op : ops)
    if (op.kind == Op::Image && op.id < last_used_.size()) last_used_[op.id] = frame_;
  if (bytes_ <= budget) return;
  std::vector<ImgId> cand;
  for (ImgId i = 0; i < imgs_.size(); ++i)
    if (imgs_[i].state != 2 && !pinned_.count(i) && last_used_[i] < frame_) cand.push_back(i);
  std::sort(cand.begin(), cand.end(), [&](ImgId a, ImgId b) { return last_used_[a] < last_used_[b]; });
  for (ImgId i : cand) {
    if (bytes_ <= budget) break;
    evict(i);
  }
}

// Every image is RGBA8: its budget size is w*h*4 whether or not the CPU copy is still allocated.
void Assets::evict(ImgId i) {
  Img& im = imgs_[i];
  bytes_ -= size_t(im.w) * im.h * 4;
  cpu_bytes_ -= im.rgba.size();
  std::vector<uint8_t>().swap(im.rgba);
  im.state = 2;
  if (!keys_[i].empty()) {
    by_key_.erase(keys_[i]);
    std::string().swap(keys_[i]);  // clear() would keep the buffer of every text key for the rest of the run
  }
  if (track_evicted_) evicted_log_.push_back(i);
}

size_t Assets::drop_unpinned() {
  const size_t before = bytes_;
  for (ImgId i = 0; i < imgs_.size(); ++i)
    if (imgs_[i].state != 2 && !pinned_.count(i)) evict(i);
  by_key_.rehash(0);  // the bucket array of the evicted keys
  ellipsis_cache_.clear();
  ellipsis_cache_.rehash(0);
  return before - bytes_;
}

void Assets::release_pixels(ImgId id) {
  if (id >= imgs_.size()) return;
  Img& im = imgs_[id];
  if (im.state != 0 || pinned_.count(id)) return;
  cpu_bytes_ -= im.rgba.size();
  std::vector<uint8_t>().swap(im.rgba);
  im.state = 1;
}

void Assets::forget_released() {
  for (ImgId i = 0; i < imgs_.size(); ++i)
    if (imgs_[i].state == 1) evict(i);
}

bool Assets::file(const std::string& path, ImgId& id) { return file_fit(path, UINT32_MAX, UINT32_MAX, id); }

static std::string fit_key(const std::string& path, uint32_t max_w, uint32_t max_h) {
  char head[40];
  std::snprintf(head, sizeof head, "f:%ux%u:", max_w, max_h);
  return head + path;
}

/// An SVG cover (the Tools icons of the image are .svg in their gamelist), rasterized straight at the size that fits
/// `max_w x max_h`, as ES draws it. Touches no `Assets` state, like `decode_fit` (worker thread).
static bool rasterize_svg_fit(const std::string& path, uint32_t max_w, uint32_t max_h, Img& im) {
  std::string data;
  if (!read_file(path, data)) return false;
  data.push_back('\0');  // nanosvg parses in place and needs a NUL-terminated, writable buffer
  NSVGimage* image = nsvgParse(data.data(), "px", 96.0f);
  if (!image || image->width <= 0 || image->height <= 0) {
    if (image) nsvgDelete(image);
    return false;
  }
  float s = std::min(float(max_w) / image->width, float(max_h) / image->height);
  im.w = std::max<uint32_t>(uint32_t(std::lround(image->width * s)), 1);
  im.h = std::max<uint32_t>(uint32_t(std::lround(image->height * s)), 1);
  im.rgba.assign(size_t(im.w) * im.h * 4, 0);
  NSVGrasterizer* rast = nsvgCreateRasterizer();
  nsvgRasterize(rast, image, 0, 0, s, im.rgba.data(), int(im.w), int(im.h), int(im.w) * 4);
  nsvgDeleteRasterizer(rast);
  nsvgDelete(image);
  return true;
}

namespace {
std::mutex g_accent_m;
std::unordered_map<std::string, Color>& accents() {
  static std::unordered_map<std::string, Color> m;
  return m;
}
/// Most colourful hue of the picture: 12 hue buckets weighted by saturation and brightness, their mean colour.
Color strongest_colour(const Img& im) {
  double sum[12][3] = {}, weight[12] = {};
  for (size_t i = 0; i + 3 < im.rgba.size(); i += 4) {
    if (im.rgba[i + 3] < 128) continue;
    const float r = im.rgba[i] / 255.0f, g = im.rgba[i + 1] / 255.0f, b = im.rgba[i + 2] / 255.0f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    if (d < 0.05f) continue;
    float h = mx == r ? std::fmod((g - b) / d + 6.0f, 6.0f) : mx == g ? (b - r) / d + 2.0f : (r - g) / d + 4.0f;
    const int k = std::min(11, int(h * 2.0f));
    const double w = double(d / mx) * (0.3 + mx);
    weight[k] += w, sum[k][0] += w * r, sum[k][1] += w * g, sum[k][2] += w * b;
  }
  int best = 0;
  for (int k = 1; k < 12; ++k)
    if (weight[k] > weight[best]) best = k;
  if (weight[best] <= 0) return Color{200, 200, 200, 255};
  return Color{uint8_t(sum[best][0] / weight[best] * 255), uint8_t(sum[best][1] / weight[best] * 255), uint8_t(sum[best][2] / weight[best] * 255), 255};
}
void box_blur(Img& im) {
  std::vector<uint8_t> o = im.rgba;
  const int w = int(im.w), h = int(im.h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 4; ++c) {
        int s = 0, n = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            s += im.rgba[(size_t(yy) * w + xx) * 4 + c], ++n;
          }
        o[(size_t(y) * w + x) * 4 + c] = uint8_t(s / n);
      }
  im.rgba.swap(o);
}
void round_corners(Img& im, int r) {
  if (r <= 0) return;
  r = std::min({r, int(im.w) / 2, int(im.h) / 2});
  for (int y = 0; y < r; ++y)
    for (int x = 0; x < r; ++x) {
      const float d = std::hypot(float(r) - (float(x) + 0.5f), float(r) - (float(y) + 0.5f)) - float(r) + 0.5f;
      if (d <= 0) continue;
      const float keep = std::max(0.0f, 1.0f - d);
      for (int ky : {y, int(im.h) - 1 - y})
        for (int kx : {x, int(im.w) - 1 - x}) {
          uint8_t& al = im.rgba[(size_t(ky) * im.w + kx) * 4 + 3];
          al = uint8_t(float(al) * keep);
        }
    }
}
}  // namespace

bool Assets::accent_of(const std::string& baked_path, Color& out) {
  std::lock_guard<std::mutex> lock(g_accent_m);
  auto it = accents().find(baked_path);
  if (it == accents().end()) return false;
  out = it->second;
  return true;
}

bool Assets::decode_fit(const std::string& full, uint32_t max_w, uint32_t max_h, Img& im) {
  const size_t mark = full.rfind(kBakeMark);
  if (mark != std::string::npos) {
    const std::string path = full.substr(0, mark), flags = full.substr(mark + std::strlen(kBakeMark));
    bool crop = false, gray = false, white = false, light = false;
    int radius = 0;
    for (size_t i = 0; i < flags.size(); ++i) {
      const char f = flags[i];
      if (f == 'c') crop = true;
      else if (f == 'g') gray = true;
      else if (f == 'w') white = true;
      else if (f == 'a') light = true;
      else if (f == 'r') {
        radius = 0;
        while (i + 1 < flags.size() && flags[i + 1] >= '0' && flags[i + 1] <= '9') radius = radius * 10 + (flags[++i] - '0');
      }
    }
    if (crop && path_ext_lower(path) != ".svg") {
      if (!decode_image_file(path, im) || im.w == 0 || im.h == 0) return false;
      const float s = std::max(float(max_w) / float(im.w), float(max_h) / float(im.h));
      const uint32_t nw = std::max<uint32_t>(uint32_t(std::lround(float(im.w) * s)), max_w), nh = std::max<uint32_t>(uint32_t(std::lround(float(im.h) * s)), max_h);
      if (nw != im.w || nh != im.h) im.rgba = resize_rgba(std::move(im.rgba), im.w, im.h, nw, nh, Filter::Triangle);
      const uint32_t ox = (nw - max_w) / 2, oy = (nh - max_h) / 2;
      std::vector<uint8_t> out(size_t(max_w) * max_h * 4);
      for (uint32_t y = 0; y < max_h; ++y) std::memcpy(&out[size_t(y) * max_w * 4], &im.rgba[(size_t(y + oy) * nw + ox) * 4], size_t(max_w) * 4);
      im.rgba.swap(out);
      im.w = max_w;
      im.h = max_h;
    } else if (!decode_fit(path, max_w, max_h, im)) {
      return false;
    }
    if (light) {
      const Color c = strongest_colour(im);
      {
        std::lock_guard<std::mutex> lock(g_accent_m);
        if (accents().size() > 256) accents().clear();
        accents()[full] = c;
      }
      box_blur(im);
      box_blur(im);
    }
    if (gray || white)
      for (size_t i = 0; i + 3 < im.rgba.size(); i += 4) {
        const uint8_t v = white ? 255 : uint8_t((im.rgba[i] * 77 + im.rgba[i + 1] * 150 + im.rgba[i + 2] * 29) >> 8);
        im.rgba[i] = im.rgba[i + 1] = im.rgba[i + 2] = v;
      }
    round_corners(im, radius);
    return true;
  }
  const std::string& path = full;
  if (path_ext_lower(path) == ".svg") return rasterize_svg_fit(path, max_w, max_h, im);
  if (!decode_image_file(path, im)) return false;
  if (im.w > max_w || im.h > max_h) {
    float s = std::min(float(max_w) / float(im.w), float(max_h) / float(im.h));
    uint32_t nw = std::max<uint32_t>(uint32_t(std::lround(float(im.w) * s)), 1);
    uint32_t nh = std::max<uint32_t>(uint32_t(std::lround(float(im.h) * s)), 1);
    im.rgba = resize_rgba(std::move(im.rgba), im.w, im.h, nw, nh, Filter::Triangle);
    im.w = nw;
    im.h = nh;
  }
  return true;
}

bool Assets::find_file_fit(const std::string& path, uint32_t max_w, uint32_t max_h, ImgId& id) const { return lookup(fit_key(path, max_w, max_h), id); }

ImgId Assets::add_file_fit(const std::string& path, uint32_t max_w, uint32_t max_h, Img&& img) {
  std::string key = fit_key(path, max_w, max_h);
  ImgId id;
  if (lookup(key, id)) return id;
  return push(std::move(key), std::move(img));
}

bool Assets::file_fit(const std::string& path, uint32_t max_w, uint32_t max_h, ImgId& id) {
  if (find_file_fit(path, max_w, max_h, id)) return true;
  if (max_w != UINT32_MAX) ++inline_fits_;
  Img im;
  if (!decode_fit(path, max_w, max_h, im)) return false;
  id = add_file_fit(path, max_w, max_h, std::move(im));
  return true;
}

bool Assets::svg(const std::string& path, uint32_t height, ImgId& id) {
  std::string key = "s:" + std::to_string(height) + ":" + path;
  if (lookup(key, id)) return true;
  std::string data;
  if (!read_file(path, data)) return false;
  data.push_back('\0');  // nanosvg parses in place and needs a NUL-terminated, writable buffer
  NSVGimage* image = nsvgParse(data.data(), "px", 96.0f);
  if (!image || image->height <= 0 || image->width <= 0) {
    if (image) nsvgDelete(image);
    return false;
  }
  float scale = float(height) / image->height;
  uint32_t w = uint32_t(std::max(std::ceil(image->width * scale), 1.0f));
  NSVGrasterizer* rast = nsvgCreateRasterizer();
  Img im;
  im.w = w;
  im.h = height;
  im.rgba.assign(size_t(w) * height * 4, 0);
  nsvgRasterize(rast, image, 0, 0, scale, im.rgba.data(), int(w), int(height), int(w) * 4);
  nsvgDeleteRasterizer(rast);
  nsvgDelete(image);
  id = push(std::move(key), std::move(im));
  return true;
}

// ---- text

static std::string color_key(Color c) {
  char b[16];
  std::snprintf(b, sizeof b, "%02x%02x%02x%02x", c[0], c[1], c[2], c[3]);
  return b;
}

static int px_int(float px) { return std::max(int(std::floor(px)), 1); }

float Assets::measure(std::string_view s, float px) { return face().measure(s, px_int(px)); }
float Assets::line_height(float px) { return float(face().line_height(px_int(px))); }
float Assets::letter_height(float px) { return float(face().letter_height(px_int(px))); }

ImgId Assets::text(std::string_view s, float px, Color color) {
  int ipx = px_int(px);
  std::string key = std::string(alt_on_ ? "T:" : "t:") + std::to_string(ipx) + ":" + color_key(color) + ":";
  key.append(s);
  ImgId id;
  if (lookup(key, id)) return id;
  return push(std::move(key), face().render(s, ipx, color));
}

static std::string cut_ellipsis(Assets& a, std::string_view s, float px, float max_w) {
  // drop code points from the end until `<text>...` fits; spaces before the ellipsis are dropped too
  std::vector<size_t> starts;
  for (size_t i = 0; i < s.size();) {
    starts.push_back(i);
    utf8_next(s, i);
  }
  while (!starts.empty()) {
    size_t keep = starts.back();
    starts.pop_back();
    std::string_view h = s.substr(0, keep);
    while (!h.empty() && (h.back() == ' ' || h.back() == '\t' || h.back() == '\n' || h.back() == '\r')) h.remove_suffix(1);
    std::string cand(h);
    cand += "\xE2\x80\xA6";
    if (a.measure(cand, px) <= max_w) return cand;
  }
  return "\xE2\x80\xA6";
}

ImgId Assets::text_ellipsis(std::string_view s, float px, Color color, float max_w) {
  if (measure(s, px) <= max_w) return text(s, px, color);
  // cut_ellipsis measures a new string per dropped letter: the result is kept per (face, size, width, text), for the rows
  // that are drawn again every frame
  char kb[40];
  std::snprintf(kb, sizeof kb, "%c%d:%.2f:", alt_on_ ? 'T' : 't', px_int(px), double(max_w));
  std::string key = kb;
  key.append(s);
  auto it = ellipsis_cache_.find(key);
  if (it == ellipsis_cache_.end()) {
    if (ellipsis_cache_.size() >= 512) ellipsis_cache_.clear();
    it = ellipsis_cache_.emplace(std::move(key), cut_ellipsis(*this, s, px, max_w)).first;
  }
  return text(it->second, px, color);
}

ImgId Assets::text_wrap(std::string_view s, float px, Color color, float max_w, size_t max_lines, float line_spacing) {
  char nb[96];
  std::snprintf(nb, sizeof nb, "w:%d:%.1f:%zu:%.2f:", px_int(px), double(max_w), max_lines, double(line_spacing));
  std::string key = std::string(alt_on_ ? "A" : "") + nb + color_key(color) + ":";
  key.append(s);
  ImgId id;
  if (lookup(key, id)) return id;

  std::vector<std::string> lines;
  bool truncated = false;
  size_t pos = 0;
  while (true) {
    size_t e = s.find('\n', pos);
    std::string_view para = s.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
    std::string cur;
    bool stop = false;
    size_t i = 0;
    while (i < para.size()) {
      while (i < para.size() && (para[i] == ' ' || para[i] == '\t' || para[i] == '\r')) ++i;
      size_t j = i;
      while (j < para.size() && !(para[j] == ' ' || para[j] == '\t' || para[j] == '\r')) ++j;
      if (j == i) break;
      std::string word(para.substr(i, j - i));
      i = j;
      std::string cand = cur.empty() ? word : cur + " " + word;
      if (cur.empty() || measure(cand, px) <= max_w) {
        cur = cand;
      } else {
        lines.push_back(cur);
        cur = word;
        if (lines.size() >= max_lines) {
          truncated = true;
          stop = true;
          break;
        }
      }
    }
    if (stop) break;
    if (!cur.empty() || para.empty()) {
      if (lines.size() >= max_lines) {
        truncated = true;
        break;
      }
      lines.push_back(cur);
    }
    if (e == std::string_view::npos) break;
    pos = e + 1;
  }
  if (lines.size() > max_lines) lines.resize(max_lines);
  if (truncated && !lines.empty()) {
    std::string t = lines.back() + "\xE2\x80\xA6";
    while (measure(t, px) > max_w && utf8_len(t) > 1) {
      // drop the ellipsis and one character, put the ellipsis back
      t.resize(t.size() - 3);
      size_t k = t.size();
      while (k > 0 && (static_cast<uint8_t>(t[k - 1]) & 0xC0) == 0x80) --k;
      if (k > 0) --k;
      t.resize(k);
      t += "\xE2\x80\xA6";
    }
    lines.back() = t;
  }
  float line_h = line_height(px);
  uint32_t step = uint32_t(std::max(std::round(line_h * line_spacing), 1.0f));
  // The lines are drawn straight from the font into the result, NOT through `text()`: nothing else asks for them, so
  // caching each line would hold the same pixels twice (about as much again as the wrapped image) and the cached copy
  // of a line that was uploaded to a texture has no CPU pixels to copy from.
  std::vector<Img> lines_img;
  lines_img.reserve(lines.size());
  for (const auto& l : lines) lines_img.push_back(face().render(l, px_int(px), color));
  uint32_t w = 1;
  for (const Img& i : lines_img) w = std::max(w, i.w);
  uint32_t h = std::max<uint32_t>(step * uint32_t(std::max<size_t>(lines.size(), 1)), 1);
  Img out;
  out.w = w;
  out.h = h;
  out.rgba.assign(size_t(w) * h * 4, 0);
  for (size_t n = 0; n < lines_img.size(); ++n) {
    const Img& src = lines_img[n];
    for (uint32_t row = 0; row < src.h; ++row) {
      uint32_t y = uint32_t(n) * step + row;
      if (y >= h) break;
      std::copy_n(&src.rgba[size_t(row) * src.w * 4], size_t(src.w) * 4, &out.rgba[size_t(y) * w * 4]);
    }
  }
  return push(std::move(key), std::move(out));
}

}  // namespace zf
