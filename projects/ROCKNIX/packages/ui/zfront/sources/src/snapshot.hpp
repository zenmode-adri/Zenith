// Instant start: the last carousel frame, kept on disk at half resolution and shown by the window as soon as it exists, before the
// theme and the systems are loaded (the real first frame replaces it ~1.6 s later).
//   file   `<dir>/last-frame.zfs`: "ZFSNAP1\n<key text>\n", u32 w, u32 h (little endian), w*h RGB565 pixels, u32 FNV-1a of the pixels
//   key    theme, ThemeSet, screen size, language and the build of the binary: a snapshot of another key is never shown
//   write  temp + fsync + rename (`write_atomic`); the size is bounded by `kMaxPixels`
//   reads  a missing, stale, truncated or corrupt file is simply "no snapshot"
// No SDL here (tests link it); the window side is `sdl_splash` / the capture in sdlr.cpp.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zf::snapshot {

/// Largest stored picture (half of 1280x720 fits twice over); a bigger one is not written and not read.
constexpr uint64_t kMaxPixels = 1u << 20;

struct Key {
  std::string theme;  // explicit --theme / --es-theme, empty when the ThemeSet decides
  std::string theme_set;
  std::string lang;
  std::string build;      // build_id()
  uint32_t w = 0, h = 0;  // full screen size
  /// One line, no newline inside; equal text = same key.
  std::string text() const;
};

struct Image {
  uint32_t w = 0, h = 0;
  std::vector<uint16_t> px;  // RGB565
};

/// Where the file lives: `$ZF_SNAPSHOT_DIR`, else `/storage/.cache/zfront`.
std::string dir();
std::string file_path(const std::string& d);
/// Size and modification time of the running binary: a new build never shows an old picture.
std::string build_id();

/// Half-size picture from tightly packed RGBA bytes (2x2 box average, odd last row/column dropped). Empty when too small or too big.
Image downscale_half(const uint8_t* rgba, uint32_t w, uint32_t h);
uint16_t pack565(uint8_t r, uint8_t g, uint8_t b);

/// Atomic write into `d` (created when missing). False with `err` set when nothing was written (true when writes are blocked).
bool save(const std::string& d, const Key& key, const Image& im, std::string& err);
/// The picture saved for `key`; nullopt when the file is missing, for another key, damaged or oversized (`why` says which, for the log).
std::optional<Image> load(const std::string& d, const Key& key, std::string& why);

/// Decides when the carousel has been at rest long enough to take the picture: once per rest period, at most one picture every
/// `min_gap` seconds. Times are seconds on any steady clock.
class Gate {
 public:
  explicit Gate(double rest = 3.0, double min_gap = 30.0) : rest_(rest), gap_(min_gap) {}
  /// Call every loop turn; true once when the picture must be taken now.
  bool poll(bool eligible, double now);
  /// When `poll` would fire, if it is waiting for that; nullopt when not eligible or already taken in this rest.
  std::optional<double> deadline() const;

 private:
  double rest_, gap_;
  std::optional<double> since_;
  bool taken_ = false;
  double last_ = -1e18;
};

}  // namespace zf::snapshot
