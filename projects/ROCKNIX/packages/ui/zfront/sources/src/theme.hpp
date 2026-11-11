// Minimal ES-theme reader: only the `<variables>` blocks of `theme.xml` and of the `layout.xml` it includes (the block
// whose `if` matches the current `screen.ratio` overrides the default one). It is NOT a general theme engine.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
  /// Optional numeric variable: `def` (silently) when missing or not a number. For the properties that only the themes
  /// beyond Zenith set; `def` is the value Zenith has always had, so a theme that does not say it renders as before.
  float f(std::string_view name, float def) const;
  /// Optional text variable: `def` when missing or empty.
  std::string_view str(std::string_view name, std::string_view def) const;
  Color color(std::string_view name) const;
  /// `./assets/x.png` relative to the theme folder.
  std::string path(std::string_view rel) const;
  /// Full paths of the extra fonts the theme declares in `fontFallback` (comma separated, relative to the theme folder): for glyphs
  /// its `font` lacks (CJK, emoji...). Empty for a theme that does not say it.
  std::vector<std::string> font_fallbacks() const;
  bool has(std::string_view name) const;
  /// Sets (or replaces) a variable after loading: the user's own choices over the theme's (see boot::apply_user_theme).
  void set_var(std::string_view name, std::string_view value);
  /// Name of the first essential variable the theme lacks (colours, font, the carousel and list geometry for the current
  /// ratio), "" when it is complete. A theme that fails this is not drawn: zfront uses Zenith instead.
  std::string missing() const;

 private:
  std::string dir_;
  std::unordered_map<std::string, std::string> vars_;
};

/// `screen.ratio` string ES would use for a WxH screen (closest of its fixed list).
const char* ratio_name(unsigned w, unsigned h);
/// `RRGGBB` or `RRGGBBAA` to RGBA (missing/invalid digits are 0, alpha defaults to 255).
Color parse_color(std::string_view s);

}  // namespace zf
