// Minimal ES-theme reader: only the `<variables>` blocks of `theme.xml` and of the `layout.xml` it includes (the block
// whose `if` matches the current `screen.ratio` overrides the default one). It is NOT a general theme engine.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace zf {

using Color = std::array<uint8_t, 4>;  // straight RGBA

class Theme {
 public:
  /// `dir` is a merged theme folder (`theme-dark`); `ratio` an ES `screen.ratio` string ("1/1", "4/3"...).
  static bool load(const std::string& dir, const std::string& ratio, Theme& out, std::string& err);
  /// Variables from XML text (tests): layout first, theme second (theme wins), like `load`.
  static bool from_xml(const std::string& layout_xml, const std::string& theme_xml, const std::string& ratio, Theme& out, std::string& err);

  const std::string& dir() const { return dir_; }
  std::string_view str(std::string_view name) const;
  /// Numeric variable; 0 (and a warning on stderr) when missing or not a number.
  float f(std::string_view name) const;
  Color color(std::string_view name) const;
  /// `./assets/x.png` relative to the theme folder.
  std::string path(std::string_view rel) const;
  bool has(std::string_view name) const;

 private:
  std::string dir_;
  std::unordered_map<std::string, std::string> vars_;
};

/// `screen.ratio` string ES would use for a WxH screen (closest of its fixed list).
const char* ratio_name(unsigned w, unsigned h);
/// `RRGGBB` or `RRGGBBAA` to RGBA (missing/invalid digits are 0, alpha defaults to 255).
Color parse_color(std::string_view s);

}  // namespace zf
