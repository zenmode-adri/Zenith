// Backend independent drawing: the list of draw operations (`Op`), decoded images and the image cache (`Assets`).
//
// Memory: every string and every cover is an `Img`. `Assets::end_frame` evicts the least recently drawn ones when the
// total goes over a byte budget (an evicted image keeps its id and size but its `rgba` is empty; asking for it again
// creates a new one). Images the scenes keep for ever (theme tile, frame...) are `pin`ned.
// Text is made with FreeType (text.hpp), SVG with nanosvg, PNG/JPG with stb_image and WebP with libwebp (imgdec.hpp).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "theme.hpp"  // Color

namespace zf {

using ImgId = uint32_t;

/// Straight-alpha RGBA8 image. `rgba` is empty after eviction or after the renderer uploaded it and released the CPU
/// copy (`w`/`h` stay valid); `Assets::evicted`/`has_pixels` tell the two apart.
struct Img {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
  /// Assets bookkeeping: 0 = live, 1 = CPU pixels released (the texture holds them), 2 = evicted.
  uint8_t state = 0;
};

struct Op {
  enum Kind : uint8_t { Rect, Image, Clip, Unclip };
  Kind kind = Unclip;
  ImgId id = 0;
  float x = 0, y = 0, w = 0, h = 0;
  /// Rect: fill colour. Image: tint multiplied into the pixels (255 = unchanged).
  Color color = {255, 255, 255, 255};

  /// Filled rectangle (pixels, may be fractional).
  static Op rect(float x, float y, float w, float h, Color c) { return Op{Rect, 0, x, y, w, h, c}; }
  /// Image scaled into the rectangle, multiplied by `tint`.
  static Op image(ImgId id, float x, float y, float w, float h, Color tint = {255, 255, 255, 255}) { return Op{Image, id, x, y, w, h, tint}; }
  /// Everything drawn until the next `unclip` is cut to this rectangle (no nesting).
  static Op clip(float x, float y, float w, float h) { return Op{Clip, 0, x, y, w, h, {255, 255, 255, 255}}; }
  static Op unclip() { return Op{}; }
};

class Font;

class Assets {
 public:
  Assets();
  ~Assets();
  Assets(const Assets&) = delete;
  Assets& operator=(const Assets&) = delete;
  Assets(Assets&&) noexcept;
  Assets& operator=(Assets&&) noexcept;

  /// Loads the TTF used by every text call.
  bool load_font(const std::string& path, std::string& err);

  const Img& img(ImgId id) const { return imgs_[id]; }
  /// Unique per instance for the whole run (an address can be reused by a later Assets: caches must key on this).
  uint64_t uid() const { return uid_; }
  size_t count() const { return imgs_.size(); }

  /// Keep this image for ever (theme assets whose ids the scenes store).
  void pin(ImgId id) { pinned_.insert(id); }
  /// Bytes of decoded pixels currently held: the budget counts every live image, whether its pixels are still in RAM
  /// or already only in a texture (on this console the GPU shares the RAM, so a texture is as big as its image).
  size_t bytes() const { return bytes_; }
  /// Of those, the bytes whose CPU copy is still allocated.
  size_t cpu_bytes() const { return cpu_bytes_; }

  /// The renderer made a texture from image `id`: drop the CPU copy so the same pixels are not held twice. Pinned
  /// images keep theirs (the scenes hold their ids and the textures are rebuilt from them after a game). Idempotent.
  void release_pixels(ImgId id);
  /// The CPU pixels are there (an upload is possible).
  bool has_pixels(ImgId id) const { return imgs_[id].state == 0; }
  /// Evicted: not drawable; asking for the same thing again makes a new image with a new id.
  bool evicted(ImgId id) const { return imgs_[id].state == 2; }
  /// All textures are gone (window recreated, render device reset): the images that only lived in a texture are
  /// evicted, so the scenes build them again by key on the next frame. Pinned images are untouched.
  void forget_released();
  /// Call once per drawn frame with the frame's ops: marks the images they use and evicts the least recently used
  /// ones (never pinned, never used in this frame) until the total is under `budget` bytes.
  void end_frame(const std::vector<Op>& ops, size_t budget);

  /// Load (once) a png/jpg/webp file at its own size. Returns false (and logs nothing) if it cannot be decoded.
  bool file(const std::string& path, ImgId& id);
  /// Like `file` but downscaled at decode time (keeping the aspect ratio) so it fits `max_w x max_h`: covers are
  /// decoded once at the size they will be drawn, not at their full resolution.
  bool file_fit(const std::string& path, uint32_t max_w, uint32_t max_h, ImgId& id);
  /// The three steps of `file_fit`, for decoding off the main thread (covers.hpp): `decode_fit` touches no `Assets`
  /// state, so a worker may call it; `find_file_fit` only looks the image up; `add_file_fit` inserts a decoded one
  /// under the key `file_fit` uses (an id already there wins and `img` is dropped).
  static bool decode_fit(const std::string& path, uint32_t max_w, uint32_t max_h, Img& out);
  bool find_file_fit(const std::string& path, uint32_t max_w, uint32_t max_h, ImgId& id) const;
  ImgId add_file_fit(const std::string& path, uint32_t max_w, uint32_t max_h, Img&& img);
  bool find(const std::string& key, ImgId& id) const { return lookup(key, id); } ImgId add(std::string key, Img&& img) { return push(std::move(key), std::move(img)); }  // procedural UI images (ui_style.cpp)
  /// Rasterize an SVG at the given pixel height (width follows the aspect ratio). Icons are white so the caller tints.
  bool svg(const std::string& path, uint32_t height, ImgId& id);

  // ---- text (FreeType, integer pixel sizes: callers pass `floor`ed sizes, like ES)
  /// Width in pixels of `s` at `px` (advances plus kerning, same as `text`).
  float measure(std::string_view s, float px);
  /// One line of text; white coverage in alpha, colour from `color`.
  ImgId text(std::string_view s, float px, Color color);
  /// One line cut to `max_w` pixels with a trailing "..." when it does not fit.
  ImgId text_ellipsis(std::string_view s, float px, Color color, float max_w);
  /// Greedy word wrap into one image of at most `max_lines` lines (`line_spacing` multiplies the line height);
  /// the last line gets an ellipsis if text is left over. Explicit `\n` start a new line.
  ImgId text_wrap(std::string_view s, float px, Color color, float max_w, size_t max_lines, float line_spacing);
  /// Height in pixels of one line of text at `px` (the height of an image made by `text`).
  float line_height(float px);
  /// Height of a capital letter at `px` (ES `Font::getLetterHeight`, measured on 'S').
  float letter_height(float px);

 private:
  ImgId push(std::string key, Img&& img);
  bool lookup(const std::string& key, ImgId& id) const;

  std::vector<Img> imgs_;
  uint64_t uid_;
  std::unordered_map<std::string, ImgId> by_key_;
  std::vector<uint64_t> last_used_;
  std::vector<std::string> keys_;  // empty after eviction
  std::unordered_set<ImgId> pinned_;
  uint64_t frame_ = 0;
  size_t bytes_ = 0, cpu_bytes_ = 0;
  void evict(ImgId id);
  std::unique_ptr<Font> font_;
};

}  // namespace zf
