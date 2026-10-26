// FreeType text: one face, any integer pixel size, hinted glyphs and kerning like ES `Font.cpp`
// (`FT_Set_Pixel_Sizes`, `FT_LOAD_DEFAULT | FT_LOAD_RENDER`, advance in 26.6 / 64, `FT_Get_Kerning`).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gfx.hpp"

typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;

namespace zf {

class Font {
 public:
  Font() = default;
  ~Font();
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;

  bool load(const std::string& path, std::string& err);
  /// Glyphs the main face lacks (CJK, Hangul, Arabic, FontAwesome icons) come from the same fallback fonts ES uses
  /// (`fontawesome-webfont`, `DroidSansFallbackFull`, `PyeojinGothic-Medium`, `Vazirmatn-Regular`). They are opened
  /// lazily, on the first missing code point, from: the directory of the main font, `$ZF_FONT_DIR`,
  /// `/storage/.config/emulationstation/resources`, `/usr/config/emulationstation/resources`. Nothing is loaded for Latin text.
  /// Number of fallback faces opened so far (tests).
  int fallbacks_open() const;

  /// Width of `s` at `px` (advance of every glyph plus kerning pairs).
  float measure(std::string_view s, int px);
  /// Height of the line box = the tallest ASCII glyph (ES `mMaxGlyphHeight`) = height of an image made by `render`.
  int line_height(int px);
  /// Height of the rendered 'S' (ES `getLetterHeight`).
  int letter_height(int px);
  /// One line as an RGBA image: `color` RGB everywhere, alpha = coverage * color alpha.
  Img render(std::string_view s, int px, Color color);

 private:
  struct SizeInfo {
    int max_h = 0;
    float cap_top = 0;
    int letter_h = 0;
  };
  const SizeInfo& size_info(int px);
  void set_px(int px);
  /// Which face draws `cp`: 0 = main, i = fallback i-1 (memoised); `face_at` returns it sized to `px`.
  int pick(uint32_t cp);
  FT_Face face_at(int idx, int px);
  float advance(uint32_t cp, int px);
  /// Rendered glyph (FT_LOAD_RENDER output, same pixels as loading it every time), cached per (px, code point): a long
  /// description repeats the same ~60 glyphs thousands of times.
  struct Glyph {
    bool ok = false;
    float bearing_x = 0, bearing_y = 0, adv = 0;
    unsigned w = 0, h = 0;
    std::vector<uint8_t> cov;  // w*h coverage, rows packed
  };
  const Glyph& glyph(uint32_t cp, int px);
  float kerning(uint32_t left, uint32_t right, int px);

  struct Fallback {
    const char* name;
    FT_Face face = nullptr;
    int px = 0;
    bool tried = false;
  };
  FT_Library lib_ = nullptr;
  FT_Face face_ = nullptr;
  int cur_px_ = 0;
  std::vector<std::string> font_dirs_;
  std::vector<Fallback> fb_;
  std::unordered_map<uint32_t, int8_t> face_of_;
  std::unordered_map<uint64_t, float> adv_;
  std::unordered_map<uint64_t, Glyph> glyphs_;
  std::unordered_map<int, SizeInfo> info_;
};

}  // namespace zf
