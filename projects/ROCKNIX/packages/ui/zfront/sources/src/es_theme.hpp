// ES theme -> zfront theme translator ("inspired by", not identical).
//
// EmulationStation themes (XML with <include>, <variables>, <subset>, ${var}, <view> ...) are read at load time, flattened, and
// the part zfront can use is mapped onto the variables of a Zenith theme: colours (background, text, dim text, accent, selection,
// card surface, menu panel, separator), the font, and the system logos. Positions, videos and animations are NOT mapped: the
// layout is Zenith's. The result is a generated theme folder (Zenith's layout + a rewritten theme.xml + recoloured PNGs + links to
// the ES theme's font and logos) that `Theme::load` reads like any other, and a coverage report.
//
// Safety: the XML comes from third parties. Includes are limited in depth (8), count and size (2 MB per file); no path may leave the
// theme folder (`..`, absolute, `~`, symlinks out); a broken theme gives a false return with the reason, never a crash.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "theme.hpp"  // Color

namespace zf::estheme {

/// Where ES keeps its theme settings, and the screen the theme is for.
struct Options {
  std::string ratio = "4/3";  // ES `screen.ratio`
  unsigned width = 640, height = 480;
  std::string lang = "en";    // ES `lang` filter
  /// ES setting lookup (es_settings.cfg): `ThemeColorSet`, `ThemeIconSet`, `ThemeMenu`, `ThemeSystemView`, `ThemeGamelistView`,
  /// `ThemeRegionName`, `subset.<name>`. Null result (or no function): the theme's own default (first option of each subset).
  std::function<const std::string*(const std::string&)> setting;
};

/// One ES element after includes, subsets and `${}` were applied: later definitions of the same (view, type, name) override
/// earlier ones property by property, like ES does.
struct Element {
  std::string view, type, name;
  std::map<std::string, std::string> props;
  std::string parent;  // type of the enclosing element ("itemTemplate", "container"...), "" at view level
};

/// One `<subset>` of a theme as ES lists it in THEME CONFIGURATION (ThemeData::mSubsets): the choices the user can make.
struct SubsetOption {
  std::string name, display;  // `display` is the option's displayName (its name when it has none), ${} resolved
};
struct SubsetInfo {
  std::string name, display;               // `display`: the subset's displayName, or the theme variable `subset.<name>`; "" = none
  std::vector<std::string> applies_to;     // `appliesTo` (views): ES lists those apart, under GAMELIST THEME OPTIONS
  std::vector<SubsetOption> options;       // the options that pass their filters, in file order; the first is the default
};

struct Resolved {
  std::string root;                          // theme folder
  std::vector<SubsetInfo> subset_infos;      // every subset the theme declares, in order of appearance (options of equal subsets merged)
  std::map<std::string, std::string> vars;   // <variables> after ${} expansion
  std::map<std::string, std::string> subsets;  // chosen option of each subset
  std::vector<Element> elems;
  std::vector<std::string> warnings;         // includes that were not found/allowed, filters that could not be read...
};

/// Flatten the ES theme in `dir` (its `theme.xml`). False with `err` when it cannot be read as a theme at all.
bool resolve(const std::string& dir, const Options& opt, Resolved& out, std::string& err);

/// The subsets of the theme in `dir` for THEME CONFIGURATION (ES `ThemeData::getSubSets`); empty when the theme has none or cannot be read.
/// Filters (`if`, `lang`, screen size) are evaluated like `resolve` does, with the choices `opt.setting` gives.
std::vector<SubsetInfo> list_subsets(const std::string& dir, const Options& opt);

/// `rel` (as written in a theme: `./a/b.png`) inside `root`; "" when it does not exist, is absolute, `~`, climbs out with `..`, or is a
/// symlink that leaves the theme folder.
std::string theme_file(const std::string& root, const std::string& rel);

/// Evaluate an ES `if` expression (`{screen.ratio} == '4/3' && ...`) against `vars` (strings or numbers). Invalid -> `ok` false.
bool eval_condition(const std::string& expr, const std::map<std::string, std::string>& vars, bool& ok);

struct ReportLine {
  std::string item;    // "background", "text", "font"...
  std::string status;  // "translated" | "derived" | "adjusted" | "zenith" | "unsupported"
  std::string detail;
};
struct Report {
  std::vector<ReportLine> lines;
  void add(std::string item, std::string status, std::string detail) { lines.push_back({std::move(item), std::move(status), std::move(detail)}); }
  std::string text() const;
};

using Rgb = std::array<uint8_t, 3>;
/// The colours of a Zenith variant (the keys of `tools/mkvariant.py`).
struct Palette {
  Rgb bg, txt, dim, dim_dark, tile_txt, accent, accent_dark, accent_soft, surface, panel, separator, track, switch_on, switch_off, hint,
      input_border;
};

struct Translation {
  Palette pal{};
  bool dark = true;
  std::string font_src;     // ES theme font file to use ("" = Zenith's)
  std::string logo_dir;     // folder of the ES system logos ("" = none found)
  std::string logo_prefix;  // file name = prefix + <system.theme> + suffix
  std::string logo_suffix;
  Report report;
};

/// `xmb`, `artbook`, `iconic` or `techdweeb` for the themes zfront has remade (remake.hpp), "" for any other: colours, font and logos over Zenith's layout.
std::string remade_style(const std::string& es_dir);

/// Colours/font/logos of a flattened theme. Never fails: whatever cannot be read is taken from Zenith (`light`/`dark` defaults, which
/// also say which of the two the theme is closer to when it does not name a background).
void translate(const Resolved& r, const Palette& light, const Palette& dark, Translation& out);

/// Zenith theme folders the generated one is made from (`theme-light`, `theme-dark`).
struct Bases {
  std::string light, dark;
};
struct Generated {
  std::string dir;  // generated theme folder (`Theme::load` it)
  bool dark = true;
  bool cached = false;      // the folder of a previous run (same inputs) was reused
  Report report;            // empty when `cached`
  std::string report_text;  // the coverage report, also when `cached`
};
/// Translate the ES theme in `es_dir` and write the Zenith-structured folder `<out_root>/<name of es_dir>`. An old folder of that name
/// is replaced only when it carries our marker file. False with `err` when the ES theme or the Zenith base cannot be used.
bool generate(const std::string& es_dir, const Bases& bases, const std::string& out_root, const Options& opt, Generated& out, std::string& err);

/// Where generated themes go: `$ZF_ES_THEME_OUT`, else `/storage/.cache/zfront-estheme` (kept across reboots), else `/run/zfront-estheme`, else `/tmp/zfront-estheme`.
std::string default_out_root();

/// WCAG contrast ratio of two colours (1..21) and the relative luminance used for it.
double contrast(const Rgb& a, const Rgb& b);
/// Like `fix_contrast` against several backgrounds at once (the colour must reach `min` on all of them).
bool fix_multi(Rgb& fg, const std::vector<Rgb>& bgs, double min);
/// Moves `fg` towards white or black (whichever helps) until it reaches `min` against `bg`. True when it changed.
bool fix_contrast(Rgb& fg, const Rgb& bg, double min);

}  // namespace zf::estheme
