// XMB, Art Book, Iconic and TechDweeb remade for zfront: each keeps what makes it recognisable (the place and kind of its carousel, its pictures, its
// list) and draws it with one layout that adapts to any screen. Only generated themes with the variable `esStyle` come here (es_theme_map.cpp, for
// those four folders); Zenith and any other theme never do.
//
// `Grid` is the automatic layout: every size comes from the design at 640x480 through the short side of the screen, with a minimum for text so that small
// screens stay readable; every position is a fraction of the screen or follows from what is above it. What does not fit is left out, never squeezed.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

#include "detailed.hpp"
#include "gfx.hpp"
#include "recent.hpp"
#include "status.hpp"
#include "theme.hpp"

namespace zf {
struct SystemItem;
struct InfoFade;
struct RecentRow;
}  // namespace zf

namespace zf::remake {

enum class Kind : uint8_t { None, Xmb, Art, Iconic, Tech, Zen };
Kind kind_of(const Theme& th);
inline bool active(const Theme& th) { return kind_of(th) != Kind::None; }
/// The cross: left and right step the systems, down goes to the "continue playing" column that hangs from the chosen one.
inline bool cross(const Theme& th) { return kind_of(th) == Kind::Xmb; }
inline bool next(const Theme& th) { return kind_of(th) == Kind::Zen; }
/// The themes with the row of games to resume.
inline bool resumes(const Theme& th) { return kind_of(th) == Kind::Xmb || kind_of(th) == Kind::Zen; }

/// The grid of every screen: square cells of `u` px (8 at 720x720), margins of three cells on all four sides, twelve equal columns with
/// gutters of two cells between the side margins. Vertically everything is on whole cells from top(); horizontally the columns are equal
/// (not whole cells: 41.3 px at 720), so an edge is on a column or on a cell line counted from margin().
/// Containers (rows, cards, pills, bands) sit on the lines in both directions; text and icons start on a vertical line (test_layout.cpp).
struct Grid {
  float w, h, s;
  int u;           // the cell, px
  int mx, my;      // side margin and top/bottom margin
  Grid(float W, float H)
      : w(W), h(H), s(std::min(W / 640.0f, H / 480.0f)), u(std::max(4, int(std::min(W, H)) / 90)),
        mx(3 * u), my(3 * u + ((int(H) - 6 * u) % u) / 2) {}
  /// A length of the old 640x480 design (being replaced by cells: `U`).
  int P(float v) const { return int(v * s + 0.5f); }
  /// A font size of the design, never under `floor` px.
  int F(float v, int floor = 12) const { return std::max(floor, int(v * s + 0.5f)); }
  /// n cells.
  int U(int n) const { return n * u; }
  /// `v` px on the nearest whole number of cells (at least `min` cells).
  int snap(float v, int min = 1) const { return std::max(min, int(std::lround(v / float(u)))) * u; }
  /// The side margin: where column 0 starts and the last one ends.
  int margin() const { return mx; }
  /// The top and bottom margin: the first horizontal line and the last.
  int top() const { return my; }
  int bottom() const { return int(h) - my; }
  /// The horizontal line k cells under the top margin.
  int row(int k) const { return my + k * u; }
  float gutter() const { return float(2 * u); }
  float colw() const { return (w - 2.0f * float(mx) - 22.0f * float(u)) / 12.0f; }
  float col(int i) const { return float(mx) + float(i) * (colw() + 2.0f * float(u)); }
  float span(int n) const { return float(n) * colw() + float(n - 1) * 2.0f * float(u); }
  /// The status bar: four cells, its top on the top margin, the same in every theme.
  int bar_h() const { return 4 * u; }
  int status_y() const { return my + bar_h() / 2; }
  /// Where the content starts: two cells under the bar.
  int content_top() const { return my + bar_h() + 2 * u; }
  /// The help row: three cells, its bottom on the bottom margin. Nothing else goes under `help_top`.
  int help_h() const { return 3 * u; }
  int help_y() const { return bottom() - help_h() / 2; }
  int help_top() const { return bottom() - help_h() - 2 * u; }
};

/// The box of something a view drew, for the tests that measure the layout (test_layout.cpp). `grid`: its edge must be on a column.
/// `cell`: a container (row, card, pill, band, picture frame): its four edges on the lines of the cells.
struct Box {
  float x, y, w, h;
  std::string tag;
  char anchor;
  bool grid;
  bool cell = false;
};
/// Where the views report their boxes; nullptr (the device): they do not.
std::vector<Box>*& audit_sink();
/// Reports one box. Callers that build the tag ask `audit_sink()` first: on the device nothing is reported.
void audit(float x, float y, float w, float h, std::string tag, char anchor = 'L', bool grid = false, bool cell = false);
/// Indexes into the ops of the status bar of what it draws behind its icons (XMB box, Iconic pill and discs, TechDweeb plate and frame): set
/// by the test that measures the icons; nothing is recorded on the device.
std::vector<size_t>*& status_bg_sink();
/// Ink rows of `s` at `px`, measured on a copy nothing draws (the drawn one may have lost its pixels to the renderer).
bool text_ink(Assets& a, std::string_view s, int px, int& t, int& b);

/// The line of a status bar: the top row and height of the ink of its hour (a "0" without one) as text() draws it centred on `cy`, in the
/// face in use. Its icons and percent are made exactly that tall and put on it (test_status_ink.cpp measures it).
struct InkLine {
  float top = 0, h = 0;
};
InkLine ink_line(Assets& a, std::string_view s, int px, float cy);
/// First and last row of `im` with ink (alpha over half); false when it has none.
bool ink_rows(const Img& im, int& t, int& b);
/// The size, searched from `px`, at which the ink of `s` is `h` rows tall (the nearest if none is).
int px_for_ink(Assets& a, std::string_view s, int h, int px);
/// The charging bolt of the status bars, centred on (cx, cy), with a ring of a pixel in `ring` around it, the ring no taller than `h`.
void status_bolt(Assets& a, std::vector<Op>& ops, float cx, float cy, float h, Color c, Color ring);

/// What the screens tell the views for one frame.
struct DrawState {
  bool inline_decode = true;  // harness and tests: decode inside the draw call. The device loop: `pending`, on the job thread
  std::string pending;        // the next picture to request (covers::request), the first one this frame missed
  uint32_t pending_w = 0, pending_h = 0;
  std::string system_theme;   // `<theme>` of the system of the game list
  std::function<std::string(size_t)> row_image;  // picture of a row of the game list ("" = none yet)
  float wave_t = 0.0f;        // XMB: where the waves of the backdrop are, in seconds
  float list_cam = -1.0f;     // the game list: the row the view is on while it slides to the cursor (< 0: on the cursor)
  float row_room = 1.0f;      // Zenith Next: 1 the row of games to resume keeps its room, 0 the card sits in the middle of the screen
  size_t rows_shown = 0;      // the game list: how many rows the last view drew (L / R page by it); 0 = not known
};
DrawState& state();

/// The file exists, asked once per path: a theme's logos and a game's pictures are looked at every frame and never stat'ed twice. The answers
/// are forgotten when the games change (`games_generation`: a scrape brings pictures) and the table never grows past `kExistsCache` paths.
bool exists_cached(const std::string& path);
size_t exists_cache_size();
constexpr size_t kExistsCache = 4096;

/// Key of a game's picture drawn whole in a box with rounded corners of radius `radius` (the flags `decode_fit` knows): never cropped, so a
/// portrait cover keeps its top and bottom.
std::string cover_key(const std::string& image, int radius);

/// The systems the XMB home draws around the chosen one `mid`: [first, last], as indices of an endless strip (`first` may be negative).
/// Never more of them than there are systems, or a short list would show one twice.
void xmb_span(int n, int mid, int& first, int& last);

/// Zenith Next's count under the chosen tile: "7 games", or the bare number for the systems that hold no games (tools, pictures,
/// music, books). Collections hold games.
std::string zen_count(const std::string& theme_name, uint32_t games);

/// The rows of a list of `n`, `rows` on screen, while the view slides (`list_cam`): `start` is the row at the top, fractional on the way, [first, end) the ones to draw,
/// `sel` where the selector is, counted from the top, and `lit` the row under it.
struct RowWindow {
  float start = 0.0f, sel = 0.0f;
  size_t first = 0, end = 0, lit = 0;
  bool moving = false;
};
RowWindow row_window(size_t n, size_t cursor, size_t rows);

/// Tiles of the "continue playing" row on screen (XMB only; the others have no room for it).
std::vector<size_t> recent_tiles(const Theme& th, float w, float h, const SystemItem& cur, const RecentRow& r);

void system_view(const Theme& th, Assets& a, float w, float h, const std::vector<SystemItem>& items, int cursor_abs, float cam, const Status& status, RecentRow* recent,
                 const InfoFade& fade, std::vector<Op>& ops);
void list_view(const Theme& th, Assets& a, float w, float h, const std::string& system_name, const std::vector<std::string>& names, size_t cursor, const Status& status,
               const DetailedData* detail, std::vector<Op>& ops);
/// The status bar of a remade theme: its own icons for WiFi, Bluetooth and battery, its own clock.
void status(const Theme& th, Assets& a, float w, float h, const Status& st, std::vector<Op>& ops);
/// Behind the rows of a menu: the theme's backdrop, over the flat colour Menu has drawn.
void menu_backdrop(const Theme& th, Assets& a, float w, float h, std::vector<Op>& ops);
/// The help row of the views, for the screens that are not views: same place, same size, the colours of the menus.
void menu_help(const Theme& th, Assets& a, float w, float h, const std::vector<std::pair<const char*, const char*>>& prompts, std::vector<Op>& ops);
/// Iconic: the colour of the little tile behind a menu icon; alpha 0 = no tile.
Color menu_icon_tile(const Theme& th, const std::string& icon);

}  // namespace zf::remake
