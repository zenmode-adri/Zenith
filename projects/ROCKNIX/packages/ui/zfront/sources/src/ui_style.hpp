// Visual language of the Zenith menus: colours (read from the theme's <view name="menu">), proportional metrics (all
// fractions of the screen, from the layout variables) and the few shapes the drawing model lacks (rounded rectangles,
// discs, small stroke icons), made as tiny images and cached in `Assets`.
//
// Everything here is shared by menu.cpp, ui_dialogs.cpp, ui_osd.cpp and toast.cpp. The numbers are in
// `cpp/docs/menu-spec.md`.
#pragma once
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "gfx.hpp"
#include "theme.hpp"

namespace zf::ui {

/// RGB mix (t = 0 gives `a`, t = 1 gives `b`), alpha of `a`.
Color mix(Color a, Color b, float t);
/// Alpha multiplied by `k` (0..1).
Color with_alpha(Color c, float k);

/// How a selector marks the selected row (theme variable `selStyle`): the filled pill of Zenith, only a ring in the accent
/// colour, a line under the row, or a thin accent bar at its left edge.
enum class SelStyle : uint8_t { Fill, Outline, Underline, Bar };
/// "fill" | "outline" | "underline" | "bar"; anything else (or empty) is Fill.
SelStyle parse_sel_style(std::string_view s);

/// Accessibility (SCREEN & SOUND > Appearance): text size and high contrast. Process-wide, like `ui_prefs`; `ui_prefs::reapply` sets it from
/// es_settings.cfg (`TextSize` = small | normal | large | xlarge, `HighContrast`). `gen` grows at every change so the screens that keep their
/// layout between frames (Menu) lay out again at once.
enum class TextSize : uint8_t { Small, Normal, Large, XLarge };
/// "small" | "large" | "xlarge"; anything else (or empty) is Normal.
TextSize parse_text_size(std::string_view s);
/// Factor of the text of menus, dialogs, help bars: 0.9, 1, 1.15, 1.3.
float text_scale(TextSize s);
struct Access {
  TextSize size = TextSize::Normal;
  bool contrast = false;
  unsigned gen = 0;
  float scale() const { return text_scale(size); }
};
Access& access();
/// Sets both; `gen` grows only when something changed.
void set_access(TextSize size, bool contrast);

/// Colours of the menu view. Taken from `theme.xml` (`menuBackground`, `menuText`, `menuSwitch`, `menuVolume`,
/// `menuGroup`) with `${var}` resolved; whatever the file does not say is derived from the base variables
/// (`bg txt dim cyan cyanDark cyanSoft`).
struct Style {
  bool dark = true;
  Color bg, panel, line, txt, dim, dim_dark, cyan, cyan_dark, cyan_soft;
  Color sw_on, sw_off, knob, track;     // switch + slider
  Color group_txt, group_bg;            // section headers
  Color osd_bg, osd_bar, osd_track, osd_txt;
  Color key, key_alt, btn;              // on-screen keyboard keys; dialog buttons
  Color danger, danger_soft, scrim;     // not in the theme: fixed per light/dark
  float corner = 1.0f;                  // theme variable `corner`: 0 = square corners, 1 = pills (Zenith)
  SelStyle sel_style = SelStyle::Fill;  // theme variable `selStyle`: how the selected row / element is marked (Zenith: fill)
  bool header_center = false;           // theme variable `headerAlign` = center: screen titles centred in the column (Zenith: left)
  /// Accent colour of the outline / underline / bar selector styles (the help bar's main-action colour).
  Color accent() const { return dark ? cyan : cyan_dark; }
  /// Corner radius of a surface (selector, help bar, toast, dialog button) of height `h`: half of it at corner = 1.
  int radius(float h) const { return int(h / 2.0f * corner); }
  /// With `access().contrast` the colours are the high-contrast ones: pure white (dark pages) or black (light pages) text, opaque panels,
  /// a selector with no translucency. The theme's own colours are parsed once; the contrast copy is made from them on every call.
  static Style from(const Theme& th);
};

/// Layout in pixels. Every value is a fraction of the screen (see menu-spec.md); nothing is a fixed pixel count.
struct Metrics {
  float W = 0, H = 0;
  int em = 0;                           // text size of rows and dialogs = floor(fMenu * H)
  int title_px = 0, small_px = 0, foot_px = 0;
  float margin = 0;                     // mx * W
  float col_x = 0, col_w = 0;           // content column (centred, at most kColMaxH * H wide)
  float row_h = 0, pill_h = 0;          // integers; (row_h - pill_h) is even
  float list_top = 0, list_h = 0;       // viewport of the list = listPosY.., listH
  float header_cy = 0;                  // vertical centre of the title (avY)
  float help_y = 0;                     // vertical centre of the help bar (helpY)
  int lines = 0;                        // rows that fit the viewport
  float title_left = 0;                 // where a left-aligned title starts
  float hint_y = 0;                     // vertical centre of the hint under the list
  bool grid = false;                    // every theme: all of the above comes from remake::Grid (the cells of the views)
  float inset = 0;                      // where the text of a row starts inside it: two cells
  int u = 0;                            // the cell of the grid, px
  float help_h = 0, help_bottom = 0;    // the help row: its height (whole cells) and its bottom (the bottom margin)
  static Metrics from(const Theme& th, float w, float h);
};

/// Constants of the spec (document them in menu-spec.md when you change one).
constexpr float kTitleScale = 1.22f;    // title size = fMenuTitle * this
constexpr float kColMaxH = 1.8f;        // content column width <= 1.8 * H (every target screen is narrower: full width)
constexpr float kPillInset = 0.55f;     // text inset inside the selector pill, in pill heights

enum class Icon : uint8_t { ChevronRight, ChevronLeft, Check, Sun };

/// Rounded rectangles, discs and icons as images. Handles per `Assets` instance are checked every call (a marker image),
/// so a new `Assets` never sees ids of another.
class Shapes {
 public:
  /// Filled rounded rectangle. Coordinates are snapped to whole pixels, `r` is clamped to half the smaller side.
  /// The pieces do not overlap (safe with translucent colours).
  void rrect(Assets& a, std::vector<Op>& out, float x, float y, float w, float h, int r, Color c);
  /// Pill = rounded rectangle with radius h/2.
  void pill(Assets& a, std::vector<Op>& out, float x, float y, float w, float h, Color c) { rrect(a, out, x, y, w, h, int(h / 2.0f), c); }
  /// Filled circle of integer diameter `d` whose top-left is (x, y).
  void disc(Assets& a, std::vector<Op>& out, float x, float y, int d, Color c) { rrect(a, out, x, y, float(d), float(d), d / 2, c); }
  /// Ring: the outline of a rounded rectangle, `t` pixels thick, nothing inside (safe over any background).
  void ring(Assets& a, std::vector<Op>& out, float x, float y, float w, float h, int r, float t, Color c);
  /// Selector of the selected row / element in the box (x, y, w, h), drawn as the theme's `selStyle`: `fill` colour for Fill,
  /// `accent` for the other three. The line / ring weight and the bar width follow the height.
  void selector(Assets& a, const Style& st, std::vector<Op>& out, float x, float y, float w, float h, Color fill, Color accent);
  /// `fill` rounded rectangle inside a `border` one of `t` pixels.
  void framed(Assets& a, std::vector<Op>& out, float x, float y, float w, float h, int r, Color fill, Color border, float t);
  /// Stroke icon in a `px` x `px` box at (x, y), tinted.
  void icon(Assets& a, std::vector<Op>& out, Icon k, float x, float y, int px, Color tint);

 private:
  void bind(Assets& a);
  const std::array<ImgId, 4>& corners(Assets& a, int r);
  const std::array<ImgId, 4>& ring_corners(Assets& a, int r, int t);
  const Assets* owner_ = nullptr;
  ImgId marker_ = 0;
  struct R {
    int r;
    std::array<ImgId, 4> q;
  };
  struct I {
    Icon k;
    int px;
    ImgId id;
  };
  struct RR {
    int r, t;
    std::array<ImgId, 4> q;
  };
  std::vector<R> rs_;
  std::vector<RR> rrs_;
  std::vector<I> is_;
};

/// Where the header of the screen that was just drawn sits, so the overlays (OSD pill, toasts) keep clear of its title.
/// `Menu::draw` and `KeyboardScreen::draw` publish it every frame; `draw_toasts` reads it and clears it afterwards, so a
/// screen that does not publish one (the carousel) leaves `valid` false. Main thread only.
struct HeaderBox {
  bool valid = false;
  float title_x0 = 0, title_x1 = 0, title_y0 = 0, title_y1 = 0;  // glyph box of the title
  float line_y = 0;                                              // the hairline under the header
};
HeaderBox& header_box();

/// A rendered line of text kept between frames. Rendered white, tinted when drawn, so one image serves every state.
struct Txt {
  ImgId id = 0;
  float w = 0, h = 0;
  bool ok = false;     // `set` was called
  bool blank = false;  // empty string: nothing to draw
  /// True while the image can be drawn (not evicted).
  bool alive(const Assets& a) const { return ok && (blank || !a.img(id).rgba.empty()); }
  /// Renders `s` (cut with "..." to `max_w` when > 0).
  void set(Assets& a, std::string_view s, int px, float max_w = 0);
  /// Word-wrapped block (`text_wrap`), lines left aligned.
  void set_wrap(Assets& a, std::string_view s, int px, float max_w, size_t max_lines, float spacing);
  void reset() { *this = Txt(); }
};

/// Screen titles (theme variable `headerAlign`). Left (Zenith): the title starts at `left_x`. Center: it is centred in the content
/// column and its width is capped so that it stays clear of the status stack (wifi / battery / clock), whose operations are
/// `out[stack_from ..]`. `dflt` is the width the left-aligned title would get.
float title_max_w(const Style& st, const Metrics& m, const std::vector<Op>& out, size_t stack_from, float dflt);
/// x of a title of width `title_w`: `left_x` when left-aligned, centred in the column otherwise.
float title_x(const Style& st, const Metrics& m, float left_x, float title_w);

/// One entry of a help bar: icon `assets/help-<icon>.png` and the (translated) text.
struct Prompt {
  const char* icon;
  const char* text;
};

/// One entry of the help pill, already rendered: the button icon, the text (white, tinted when drawn) and whether it is the main
/// action of the screen (drawn in the accent colour).
struct HelpItem {
  ImgId icon = 0;
  float iw = 0, ih = 0;
  ImgId text = 0;
  float tw = 0, th = 0;
  bool primary = false;
};
/// The help bar of every screen: a floating pill (theme panel colour, hairline, soft shadow) centred at `help_y`, icons 1.55x the
/// capital height, the main action in the accent colour. Entries that do not fit the width are left out from the end.
/// Its bottom never goes under the side margin (3 % of the width) of a screen `H` tall: a theme's `helpY` set it lower, closer to the
/// edge than anything at the top or the sides.
/// With `grid` (the metrics of a theme on the grid): its height and bottom from there and its width in whole pairs of cells, still centred.
void help_pill(Assets& a, const Style& st, float W, float H, float help_y, float px, std::vector<HelpItem>& items, std::vector<Op>& out,
               const Metrics* grid = nullptr);
/// Top edge (whole pixels) of the help pill of a screen with metrics `m`: the same geometry `help_pill` uses.
float help_pill_top(Assets& a, const Theme& th, const Metrics& m);

/// Help bar with the same geometry as the carousel/list ones (`scene.cpp: help_bar`), but its images are kept between
/// frames: it rebuilds only when the prompt table (compared by address) or the size changes.
class HelpBar {
 public:
  void draw(Assets& a, const Theme& th, const Metrics& m, const Style& st, const Prompt* p, size_t n, std::vector<Op>& out);

 private:
  struct Item {
    ImgId icon = 0;
    float iw = 0, ih = 0;
    Txt t;
  };
  const Prompt* key_ = nullptr;
  size_t n_ = 0;
  float px_ = 0;
  std::vector<Item> items_;
  std::vector<HelpItem> hi_;  // scratch of draw(): no allocation per frame once it has grown
  // the grid's help row: labels translated once per prompts and language
  const Prompt* gkey_ = nullptr;
  size_t gn_ = 0;
  unsigned ggen_ = 0;
  std::vector<std::string> glabels_;
  std::vector<std::pair<const char*, const char*>> grow_;
};

/// `t` drawn with its left edge at x and vertically centred on cy (whole pixels).
void put_text(std::vector<Op>& out, const Txt& t, float x, float cy, Color tint);
/// Same, right edge at x.
void put_text_right(std::vector<Op>& out, const Txt& t, float x_right, float cy, Color tint);

/// Text colour that reads on `fill`: near black on light fills, white on dark ones.
Color on_color(Color fill);

/// Moves `v` towards `target` (exponential, time constant `tau` seconds, snaps within half a pixel). True while moving.
inline bool approach(float& v, float target, float dt, float tau = 0.055f) {
  float d = target - v;
  if (d > -0.5f && d < 0.5f) {
    bool moved = v != target;
    v = target;
    return moved;
  }
  v += d * (1.0f - std::exp(-dt / tau));
  return true;
}

/// Floor of a size, never below 1 (text sizes are whole pixels, like ES).
inline int px_floor(float v) { return v < 1.0f ? 1 : int(v); }

}  // namespace zf::ui
