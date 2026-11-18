#include "addgames.hpp"  // the "Add games" card's line under the carousel
#include "remake.hpp"

#include "ui_prefs.hpp"

#include <algorithm>
#include <functional>
#include <ctime>
#include <memory>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "covers.hpp"
#include "gamemeta.hpp"
#include "i18n.hpp"
#include "motion.hpp"
#include "scan.hpp"
#include "scene.hpp"
#include "services.hpp"
#include "ui_style.hpp"
#include "util.hpp"
#include "video.hpp"
#include "vpreview.hpp"

namespace zf {
Color system_color(const std::string& t, const ui::Style& st);  // scene.cpp
const char* system_icon_label(const std::string& t);            // scene.cpp
}

namespace zf::remake {

namespace {

DrawState g_state;
const Color kWhite = {255, 255, 255, 255};
ui::Shapes g_shapes;
std::vector<Box>* g_audit = nullptr;
std::vector<size_t>* g_status_bg = nullptr;
/// What the status bar drew from `from` on is its background, not an icon.
void status_bg(const std::vector<Op>& ops, size_t from) {
  if (g_status_bg)
    for (size_t i = from; i < ops.size(); ++i) g_status_bg->push_back(i);
}
bool g_audit_quiet = false;  // inside the status bar and the help: they report themselves, as one box
int g_free = 0;  // inside a card or a row of chips: those flow from their own box, not from a column
struct Free {
  Free() { ++g_free; }
  ~Free() { --g_free; }
};
void rec(float x, float y, float w, float h, std::string tag, char anchor = 'L', bool grid = false, bool cell = false) {
  if (g_audit && !g_audit_quiet) g_audit->push_back(Box{x, y, w, h, std::move(tag), anchor, grid && g_free == 0, cell && g_free == 0});
}

Color rgb(uint32_t v, uint8_t a = 255) { return Color{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), a}; }
Color alpha(Color c, int a) { return Color{c[0], c[1], c[2], uint8_t(std::clamp(a, 0, 255))}; }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
int rem_e(int a, int n) {
  const int r = a % n;
  return r < 0 ? r + n : r;
}

std::unordered_map<std::string, bool>& exists_table() {
  static std::unordered_map<std::string, bool> seen;
  return seen;
}
bool exists_once(const std::string& p) {
  std::unordered_map<std::string, bool>& seen = exists_table();
  static unsigned gen = games_generation();
  if (gen != games_generation()) {  // games were scraped, edited or removed: a picture that was not there may be now
    gen = games_generation();
    seen.clear();
  }
  auto it = seen.find(p);
  if (it != seen.end()) return it->second;
  const bool e = file_exists(p);
  if (seen.size() >= kExistsCache) seen.clear();  // bounded: the oldest answers are asked again, nothing is cached for ever
  seen.emplace(p, e);
  return e;
}

/// A file of the ES theme. A system name never leaves its folder.
std::string es_file(const Theme& th, const std::string& rel) { return std::string(th.str("esDir", "")) + "/" + rel; }
std::string sys_file(const Theme& th, const char* folder, const std::string& sys, const char* ext) {
  if (sys.empty() || sys.find('/') != std::string::npos || sys[0] == '.') return {};
  std::string p = es_file(th, std::string(folder) + "/" + sys + ext);
  return exists_once(p) ? p : std::string();
}
std::string baked(const std::string& p, const char* flags) { return p + kBakeMark + flags; }

/// A picture at a fixed box. `later`: on the job thread while the loop runs; the frame goes on without it and the loop redraws when it lands.
bool pic(Assets& a, const std::string& path, float bw, float bh, bool later, ImgId& id) {
  const uint32_t mw = uint32_t(std::max(1L, std::lround(bw))), mh = uint32_t(std::max(1L, std::lround(bh)));
  if (covers::failed(path)) return false;
  if (!later || g_state.inline_decode) {
    if (a.file_fit(path, mw, mh, id)) return true;
    covers::mark_failed(path);
    return false;
  }
  if (covers::ready(a, path, mw, mh, id)) {
    a.touch(id);
    return true;
  }
  if (g_state.pending.empty()) {
    g_state.pending = path;
    g_state.pending_w = mw;
    g_state.pending_h = mh;
  }
  return false;
}

void fit_size(const Img& im, float bw, float bh, float& ow, float& oh) {
  const float k = std::min(bw / float(im.w), bh / float(im.h));
  ow = float(im.w) * k;
  oh = float(im.h) * k;
}

/// Contained in the box, centred by `ha`, `va` (0..1).
void put_fit(Assets& a, std::vector<Op>& ops, ImgId id, float x, float y, float bw, float bh, Color tint = kWhite, float ha = 0.5f, float va = 0.5f) {
  float ow, oh;
  fit_size(a.img(id), bw, bh, ow, oh);
  ops.push_back(Op::image(id, std::round(x + (bw - ow) * ha), std::round(y + (bh - oh) * va), ow, oh, tint));
  if (ha == 0.0f) rec(x, std::round(y + (bh - oh) * va), ow, oh, "logo", 'L', true);
}

// ---- soft pieces, made once as tiny images and stretched: no blur at run time --------------------------------------------------------------

template <typename Fn>
ImgId texture(Assets& a, const char* key, uint32_t tw, uint32_t th, Fn alpha_at) {
  ImgId id;
  if (a.find(key, id)) return id;
  Img im;
  im.w = tw;
  im.h = th;
  im.rgba.assign(size_t(tw) * th * 4, 255);
  for (uint32_t y = 0; y < th; ++y)
    for (uint32_t x = 0; x < tw; ++x) im.rgba[(size_t(y) * tw + x) * 4 + 3] = uint8_t(std::clamp(alpha_at((float(x) + 0.5f) / float(tw), (float(y) + 0.5f) / float(th)), 0.0f, 1.0f) * 255.0f);
  return a.add(key, std::move(im));
}

/// Texts are measured in a colour nothing draws: those pictures are never uploaded, so the renderer never releases their pixels.
constexpr Color kMeasure{1, 2, 3, 255};  // opaque: the alpha of the text is its coverage times this one
const Img& text_px(Assets& a, std::string_view s, int px) {
  ImgId id = a.text(s, float(px), kMeasure);
  if (!a.has_pixels(id)) {  // evicted for room: made again
    a.forget(id);
    id = a.text(s, float(px), kMeasure);
  }
  return a.img(id);
}
/// A picture that is measured: if the renderer released its pixels (it does with everything it uploaded) it is made again, and it
/// is pinned so it keeps them. The status bar measures its icons each time it is built again (every minute): see test_status_ink.
template <typename Make>
ImgId measurable(Assets& a, Make make) {
  ImgId id = make();
  if (!a.has_pixels(id)) {
    a.forget(id);
    id = make();
  }
  a.pin(id);
  return id;
}

/// A soft shadow or glow: box (x, y, w, h), spreading `b` px out of it.
void soft(Assets& a, std::vector<Op>& ops, float x, float y, float w, float h, float b, Color c) {
  ImgId id = texture(a, "rm:soft", 64, 64, [](float u, float v) {
    const float e = 0.22f;  // the box starts here, in the texture
    const float dx = std::max({e - u, u - (1 - e), 0.0f}) / e, dy = std::max({e - v, v - (1 - e), 0.0f}) / e;
    const float d = std::min(std::sqrt(dx * dx + dy * dy), 1.0f);
    return (1 - d) * (1 - d) * (3 - 2 * (1 - d)) * 1.0f;
  });
  ops.push_back(Op::image(id, x - b, y - b, w + 2 * b, h + 2 * b, c));
}

void disc(Assets& a, std::vector<Op>& ops, float cx, float cy, float r, Color c) {
  ImgId id = texture(a, "rm:disc", 64, 64, [](float u, float v) {
    const float d = std::min(std::hypot(u - 0.5f, v - 0.5f) * 2.0f, 1.0f);
    return (1 - d) * (1 - d);
  });
  ops.push_back(Op::image(id, cx - r, cy - r, 2 * r, 2 * r, c));
}

void beam(Assets& a, std::vector<Op>& ops, float x, float y, float w, float h, int al) {
  ImgId id = texture(a, "rm:beam", 128, 16, [](float u, float v) {
    const float vv = 1 - std::fabs(v - 0.5f) * 2;
    return std::pow(vv, 0.8f) * std::pow(1 - u, 1.6f) * std::min(1.0f, u * 12.0f + 0.1f);
  });
  ops.push_back(Op::image(id, x, y, w, h, Color{255, 255, 255, uint8_t(al)}));
}

void line_fade(Assets& a, std::vector<Op>& ops, float x, float y, float w, float h, Color c) {
  ImgId id = texture(a, "rm:line", 128, 1, [](float u, float) { return 1 - std::pow(u, 2.5f); });
  ops.push_back(Op::image(id, x, y, w, h, c));
}

/// Light from a corner of the screen, for Iconic's page.
void corner_light(Assets& a, std::vector<Op>& ops, float w, float h, Color c) {
  ImgId id = texture(a, "rm:corner", 48, 48, [](float u, float v) { return std::max(0.0f, 1 - (u * u + v * v)); });
  ops.push_back(Op::image(id, 0, 0, w, h, c));
}

/// Vertical fade, for the top of Art Book's strips.
void top_fade(Assets& a, std::vector<Op>& ops, float w, float h, Color c) {
  ImgId id = texture(a, "rm:top", 1, 32, [](float, float v) { return 1 - v; });
  ops.push_back(Op::image(id, 0, 0, w, h, c));
}

void bottom_fade(Assets& a, std::vector<Op>& ops, float w, float screen_h, float h, Color c) {
  ImgId id = texture(a, "rm:bottom", 1, 32, [](float, float v) { return v; });
  ops.push_back(Op::image(id, 0, screen_h - h, w, h, c));
}

void scanlines(Assets& a, std::vector<Op>& ops, float x, float y, float w, float h, float screen_h) {
  char key[32];
  std::snprintf(key, sizeof key, "rm:scan%d", int(screen_h));
  ImgId id;
  if (!a.find(key, id)) {
    Img im;
    im.w = 1;
    im.h = uint32_t(screen_h);
    im.rgba.assign(size_t(im.h) * 4, 0);
    for (uint32_t r = 0; r < im.h; r += 2) im.rgba[size_t(r) * 4 + 3] = 40;
    id = a.add(key, std::move(im));
  }
  ops.push_back(Op::clip(x, y, w, h));
  ops.push_back(Op::image(id, x, 0, w, screen_h));
  ops.push_back(Op::unclip());
}

// ---- text ----------------------------------------------------------------------------------------------------------------------------------

enum Anchor { L, C, R };
void put(Assets& a, std::vector<Op>& ops, ImgId id, float x, float cy, Anchor an) {
  const float tw = float(a.img(id).w), thh = float(a.img(id).h);
  const float ox = an == L ? x : an == C ? x - tw / 2.0f : x - tw;
  ops.push_back(Op::image(id, std::round(ox), std::round(cy - thh / 2.0f), tw, thh));
}
void text(Assets& a, std::vector<Op>& ops, std::string_view s, float x, float cy, int px, Color c, float max_w = 0, Anchor an = L) {
  if (s.empty()) return;
  ImgId id = max_w > 0 ? a.text_ellipsis(s, float(px), c, max_w) : a.text(s, float(px), c);
  put(a, ops, id, x, cy, an);
  if (g_audit && c[3] > 40) {
    const float tw = float(a.img(id).w), thh = float(a.img(id).h);
    rec(an == L ? x : an == C ? x - tw / 2.0f : x - tw, cy - thh / 2.0f, tw, thh, "\"" + std::string(s.substr(0, 18)) + "\"", an == L ? 'L' : an == C ? 'C' : 'R', an != C);
  }
}
/// LIST TEXT CASE (theme variable `listCase`, APPEARANCE): "" keeps the case of each name, "upper" / "lower" force it on the names of a list. `buf` keeps the
/// result alive; nothing is made when the user has not chosen.
std::string_view list_cased(const Theme& th, const std::string& s, std::string& buf) {
  const std::string_view mode = th.str("listCase", "");
  if (mode == "lower") return buf = utf8_lower(s);
  if (mode == "upper") return buf = utf8_upper(s);
  return s;
}
/// The theme's second face (titles).
/// A system name in place of a missing logo: one line if it fits in `max_w`, else two, cut at the space nearest the middle (each line is
/// still clipped with an ellipsis by `text` if a word alone is too long). Centred on (cx, cy).
void name_lines(Assets& a, std::vector<Op>& ops, const std::string& s, float cx, float cy, int px, Color c, float max_w) {
  if (a.measure(s, float(px)) <= max_w || s.find(' ') == std::string::npos) {
    text(a, ops, s, cx, cy, px, c, max_w, C);
    return;
  }
  size_t cut = std::string::npos;
  for (size_t i = s.find(' '); i != std::string::npos; i = s.find(' ', i + 1))
    if (cut == std::string::npos || std::abs(int(i) - int(s.size() / 2)) < std::abs(int(cut) - int(s.size() / 2))) cut = i;
  const float lh = a.line_height(float(px));
  text(a, ops, s.substr(0, cut), cx, cy - lh / 2.0f, px, c, max_w, C);
  text(a, ops, s.substr(cut + 1), cx, cy + lh / 2.0f, px, c, max_w, C);
}

void text_alt(Assets& a, std::vector<Op>& ops, std::string_view s, float x, float cy, int px, Color c, float max_w = 0, Anchor an = L) {
  a.use_alt_face(true);
  text(a, ops, s, x, cy, px, c, max_w, an);
  a.use_alt_face(false);
}

std::string games_of(uint32_t n) {
  std::string t = i18n::ntr("%i GAME", "%i GAMES", n);
  size_t p = t.find("%i");
  if (p == std::string::npos) p = t.find("%d");
  if (p != std::string::npos) t.replace(p, 2, std::to_string(n));
  return t;
}
std::string games_low(uint32_t n) { return i18n::sentence_case(games_of(n)); }

std::string meta_of(const DetailedData& d) {
  std::string m;
  auto add = [&](std::string_view v) {
    if (v.empty()) return;
    if (!m.empty()) m += "  \xC2\xB7  ";
    m.append(v);
  };
  char y[16] = "";
  if (d.year >= 0) std::snprintf(y, sizeof y, "%d", int(d.year));
  add(y);
  add(trim(d.genre));
  add(trim(d.players));
  return m;
}
bool has_data(const DetailedData& d) { return d.year >= 0 || !trim(d.genre).empty() || !trim(d.players).empty() || !trim(d.desc).empty(); }

void stars(const Theme& th, Assets& a, std::vector<Op>& ops, float x, float y, int size, float rating, Color on, Color off, int gap) {
  ImgId empty = 0, full = 0;
  if (!a.svg(th.path("./assets/icons/icon-star-unfilled.svg"), uint32_t(size), empty) || !a.svg(th.path("./assets/icons/icon-star-filled.svg"), uint32_t(size), full)) return;
  const float sw = float(a.img(full).w), sh = float(a.img(full).h);
  const float value = std::round(std::clamp(rating, 0.0f, 1.0f) * 10.0f) / 2.0f;
  for (int i = 0; i < 5; ++i) {
    const float sx = x + float(i) * (sw + float(gap));
    const float fill = std::clamp(value - float(i), 0.0f, 1.0f);
    if (fill >= 1.0f) {
      ops.push_back(Op::image(full, sx, y, sw, sh, on));
    } else {
      ops.push_back(Op::image(empty, sx, y, sw, sh, off));
      if (fill > 0.0f) {
        ops.push_back(Op::clip(sx, y, sw * fill, sh));
        ops.push_back(Op::image(full, sx, y, sw, sh, on));
        ops.push_back(Op::unclip());
      }
    }
  }
}

/// The description: a window over the wrapped text that scrolls like ES (`desc_scroll_offset`).
void desc_block(Assets& a, std::vector<Op>& ops, const DetailedData& d, float x, float y, float w, float h, int px, Color c) {
  d.desc_max_scroll = 0;
  if (trim(d.desc).empty() || h < float(px)) return;
  const float spacing = 1.3f;
  ImgId id = a.text_wrap(d.desc, float(px), c, std::floor(w) - 1.0f / 64.0f, 128, spacing);
  const float lh = std::max(std::round(a.line_height(float(px)) * spacing), 1.0f);
  const float win = std::floor(h / lh) * lh;  // whole lines only: no line cut in half at rest
  if (win < lh) return;
  const int max_off = std::max(int(a.img(id).h) - int(win), 0);
  d.desc_max_scroll = max_off;
  rec(x, y, w, win, "descripcion", 'L', true);
  const int off = d.desc_time < 0 ? 0 : desc_scroll_offset(d.desc_time, max_off);
  ops.push_back(Op::clip(x, y, w, win));
  ops.push_back(Op::image(id, x, y - float(off), float(a.img(id).w), float(a.img(id).h)));
  ops.push_back(Op::unclip());
}

// ---- bars ------------------------------------------------------------------------------------------------------------------------------------

struct HelpLook {
  Color fg = kWhite, accent = {0, 0, 0, 0}, pill = {0, 0, 0, 0};
  float cy = 0, cx = 0, right = 0;  // right > 0: right aligned there; else centred on cx
  float scale = 1.0f;               // menus with a larger text size (ui::access): icon, text and gaps grow, what does not fit is left out from the end
};
void help(const Theme& th, Assets& a, const Grid& g, const HelpPrompts& prompts, const HelpLook& look, std::vector<Op>& ops) {
  // Text and row come from the Grid (level Help, TEXT SIZE included); `look.scale` only widens the gaps and decides what is left out.
  const float k = std::max(look.scale, std::max(ui::access().scale(), 1.0f));
  const int px = g.L(ui::Level::Help);
  const int ih = std::min(g.help_h(), std::max(2, int(std::lround(1.2f * float(px) / float(g.u)))) * g.u), pad = int(std::lround(float(g.P(6)) * k));
  int gap = g.P(16);
  struct Item {
    ImgId icon = 0, label = 0;
    float iw = 0, lw = 0;
    bool main = false;
  };
  std::vector<Item> items;
  float total = 0;
  for (const auto& [icon, label] : prompts) {
    Item it;
    if (!a.file(th.path(std::string("./assets/help-") + icon + ".png"), it.icon)) continue;
    it.iw = float(a.img(it.icon).w) * float(ih) / float(a.img(it.icon).h);
    it.label = a.text(label, float(px), look.fg);
    it.lw = float(a.img(it.label).w);
    it.main = std::strcmp(icon, "A") == 0;
    total += it.iw + float(pad) + it.lw;
    items.push_back(it);
  }
  if (items.empty()) return;
  const float room = g.w - 2.0f * float(g.margin());
  while (items.size() > 1 && total + float(g.P(6)) * float(items.size() - 1) > room) {  // never past the margin: the last prompts go
    total -= items.back().iw + float(pad) + items.back().lw;
    items.pop_back();
  }
  const float bare = total;
  while (gap > g.P(6) && bare + float(gap) * float(items.size() - 1) > room) --gap;
  total = bare + float(gap) * float(items.size() - 1);
  float x = look.right > 0 ? look.right - total : look.cx - total / 2.0f;
  x = std::max(x, float(g.margin()));
  rec(x, look.cy - float(std::max(ih, px)) / 2.0f, total, float(std::max(ih, px)), "ayuda", look.right > 0 ? 'R' : 'C', false);
  if (look.pill[3]) g_shapes.pill(a, ops, x - float(g.P(14)), look.cy - float(g.P(14)), total + float(g.P(28)), float(g.P(28)), look.pill);
  for (const Item& it : items) {
    ops.push_back(Op::image(it.icon, std::round(x), std::round(look.cy - float(ih) / 2.0f), it.iw, float(ih), it.main && look.accent[3] ? look.accent : look.fg));
    put(a, ops, it.label, x + it.iw + float(pad), look.cy, L);
    x += it.iw + float(pad) + it.lw + float(gap);
  }
}

/// The status bar on a picture: each theme now brings its own background for it.
void status_pill(const Theme& th, Assets& a, const Grid& g, const Status& st, Color, std::vector<Op>& ops) { status_bar(th, a, g.w, g.h, st, ops); }

bool at_rest(float cam) { return std::fabs(cam - std::round(cam)) < 0.001f; }

std::vector<size_t> window(size_t n, size_t cursor, size_t rows) {
  std::vector<size_t> out;
  if (n == 0 || rows == 0) return out;
  const size_t st = n > rows ? std::min(cursor > rows / 2 ? cursor - rows / 2 : 0, n - rows) : 0;
  for (size_t i = st; i < std::min(n, st + rows); ++i) out.push_back(i);
  return out;
}

/// The camera seen as a pair of systems: `lo` and `lo + 1` with `frac` between them. At rest (and within 0.001 of it) the camera is snapped
/// to the whole system, so `frac` is exactly 0 and only `lo` is drawn: the resting picture does not depend on the motion code.
struct CamPair {
  int lo = 0;
  float frac = 0.0f;
  bool moving = false;
  explicit CamPair(float cam) {
    moving = !at_rest(cam);
    const float c = moving ? cam : std::round(cam);
    lo = int(std::floor(c));
    frac = c - float(lo);
  }
  /// 0 = still at `lo`, 1 = already at `lo + 1`; `a` (0..0.2) delays the start, so a second text can follow the first a beat later.
  /// It reaches 0 and 1 without a jump, and going backwards the same value runs the other way.
  float phase(float a) const { return std::clamp((frac - a) / (1.0f - a), 0.0f, 1.0f); }
};

/// A rectangle that cuts what rolls inside it while the camera moves (h <= 0: no cut).
struct RollBand {
  float x = 0, y = 0, w = 0, h = 0;
};

/// Draws the text (or whatever) of the two systems the camera is between, as a roll: `draw(item, index, s)` is called for each one with
/// `s` = -d for the system the camera leaves and 1 - d for the one it reaches, in units of the roll's travel (so the old one goes up and out
/// and the new one comes up from below; backwards the same numbers give the opposite way). With `dim` the old one fades out and the new one
/// in (`d` and `1 - d`), without it they are only cut by the band, like the digits of an odometer. What moves is not measured by the audit.
/// Only the position and the opacity change per frame, never a colour or a size: nothing new is made for the text cache while it moves.
template <class F>
void draw_pair(std::vector<Op>& ops, const std::vector<SystemItem>& items, const CamPair& cp, float a, const RollBand& band, bool dim, F&& draw) {
  const float d = cp.phase(a);
  const int n = int(items.size());
  const bool quiet = g_audit_quiet;
  const bool cut = cp.moving && band.h > 0.0f;
  if (cut) ops.push_back(Op::clip(band.x, band.y, band.w, band.h));
  for (int k = 0; k < (cp.moving ? 2 : 1); ++k) {
    const float wk = k == 0 ? 1.0f - d : d;
    if (wk <= 0.004f) continue;
    const size_t from = ops.size();
    if (cp.moving) g_audit_quiet = true;
    draw(items[size_t(rem_e(cp.lo + k, n))], rem_e(cp.lo + k, n), k == 0 ? -d : 1.0f - d);
    g_audit_quiet = quiet;
    if (dim) motion::fade(ops, from, wk);
  }
  if (cut) ops.push_back(Op::unclip());
}

/// Box of a 4:3 picture `w` wide that fits under `max_h`.
void box43(float w, float max_h, float& ow, float& oh) {
  oh = std::floor(w * 0.75f);
  ow = w;
  if (oh > max_h) {
    oh = std::max(std::floor(max_h), 1.0f);
    ow = std::floor(oh / 0.75f);
  }
}

// The picture of the game, rounded and cropped to its box, or `logo` on a surface of the same size: the screen does not jump between games.
void cover_box(Assets& a, std::vector<Op>& ops, const std::string& image, float x, float y, float w, float h, int r, const std::string& logo, Color surface, Color frame, Color shadow,
               const std::string& video = std::string()) {
  rec(x, y, w, h, "caratula", 'L', false);
  video::box_offer(x, y, w, h);  // where the list preview plays
  if (vpreview::hides(video, x, y, w, h)) {  // the game's video plays here: only the card, nothing to show at its sides
    if (frame[3]) g_shapes.rrect(a, ops, x - 1, y - 1, w + 2, h + 2, r + 1, frame);
    if (surface[3]) g_shapes.rrect(a, ops, x, y, w, h, r, surface);
    return;
  }
  ImgId id;  // the whole cover, whatever its shape: a portrait box must not lose its top and bottom
  const bool have = !image.empty() && exists_once(image) && pic(a, cover_key(image, r), w, h, true, id);
  if (have) {
    float ow, oh;
    fit_size(a.img(id), w, h, ow, oh);
    const float ox = std::round(x + (w - ow) / 2.0f), oy = std::round(y + (h - oh) / 2.0f);
    if (shadow[3]) soft(a, ops, ox - 2, oy + 2, ow + 4, oh + 4, float(std::max(6, r * 2)), shadow);
    if (frame[3]) g_shapes.rrect(a, ops, ox - 1, oy - 1, ow + 2, oh + 2, r + 1, frame);
    ops.push_back(Op::image(id, ox, oy, ow, oh));
    return;
  }
  if (frame[3]) g_shapes.rrect(a, ops, x - 1, y - 1, w + 2, h + 2, r + 1, frame);
  if (surface[3]) g_shapes.rrect(a, ops, x, y, w, h, r, surface);
  ImgId lg;
  if (!logo.empty() && pic(a, logo, w * 0.6f, h * 0.5f, true, lg)) put_fit(a, ops, lg, x + w * 0.2f, y + h * 0.25f, w * 0.6f, h * 0.5f, Color{255, 255, 255, 170});
}

// ---- XMB -------------------------------------------------------------------------------------------------------------------------------------

/// The backdrop, made here at the size of the screen: the picture of the ES theme is 640x480 and goes soft when stretched.
ImgId xmb_backdrop(Assets& a, const Grid& g) {
  const uint32_t W = uint32_t(g.w), H = uint32_t(g.h);
  char key[40];
  std::snprintf(key, sizeof key, "rm:xbg%ux%u", W, H);
  ImgId id;
  if (a.find(key, id)) return id;
  // Sheets of light, not lines: a thin bright edge above, a long fade below.
  struct Band {
    float c, amp, freq, phase, edge, fall, gain;
  };
  static const Band bands[] = {
      {0.58f, 0.050f, 1.0f, 0.4f, 0.120f, 0.120f, 0.07f},  // the glow behind
      {0.53f, 0.042f, 1.4f, 2.2f, 0.0040f, 0.100f, 0.15f},
      {0.61f, 0.062f, 0.8f, 4.1f, 0.0050f, 0.150f, 0.12f},
      {0.57f, 0.034f, 1.9f, 1.0f, 0.0030f, 0.060f, 0.10f},
  };
  constexpr int kBands = int(sizeof bands / sizeof bands[0]);
  std::vector<float> centre(size_t(W) * kBands);  // the curves once per column: this runs on a slow core, once
  for (uint32_t x = 0; x < W; ++x)
    for (int b = 0; b < kBands; ++b) {
      const Band& d = bands[b];
      centre[size_t(x) * kBands + size_t(b)] = d.c + d.amp * std::sin(6.2832f * d.freq * float(x) / float(W) + d.phase);
    }
  Img im;
  im.w = W;
  im.h = H;
  im.rgba.resize(size_t(W) * H * 4);
  static const uint8_t kDither[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};  // or the slow gradient shows steps
  for (uint32_t y = 0; y < H; ++y) {
    const float v = (float(y) + 0.5f) / float(H);
    const float t = v < 0.55f ? v / 0.55f : (1.0f - v) / 0.45f;  // darkest at both ends
    const float base_r = 6.0f + 20.0f * t, base_g = 12.0f + 52.0f * t, base_b = 52.0f + 128.0f * t;
    uint8_t* px = im.rgba.data() + size_t(y) * W * 4;
    for (uint32_t x = 0; x < W; ++x, px += 4) {
      float light = 0.0f;
      for (int b = 0; b < kBands; ++b) {
        const float dv = v - centre[size_t(x) * kBands + size_t(b)];
        const float dy = dv / (dv < 0 ? bands[b].edge : bands[b].fall);
        if (std::fabs(dy) < 3.0f) light += bands[b].gain * std::exp(-dy * dy);
      }
      light = std::min(light, 1.0f);
      const float n = float(kDither[((y & 3) << 2) | (x & 3)]) / 16.0f;
      px[0] = uint8_t(std::min(255.0f, base_r + (225.0f - base_r) * light + n));
      px[1] = uint8_t(std::min(255.0f, base_g + (235.0f - base_g) * light + n));
      px[2] = uint8_t(std::min(255.0f, base_b + (255.0f - base_b) * light + n));
      px[3] = 255;
    }
  }
  return a.add(key, std::move(im));
}

void xmb_back(const Theme& th, Assets& a, const Grid& g, std::vector<Op>& ops) {
  ops.push_back(Op::rect(0, 0, g.w, g.h, th.color("bg")));
  ops.push_back(Op::image(xmb_backdrop(a, g), 0, 0, g.w, g.h));
  // two thin ribbons of light drifting over it; each texture is one seamless period, twice the width of the screen
  ImgId r1 = texture(a, "rm:xwave1", 1024, 192, [](float u, float v) {
    const float dv = v - 0.35f - 0.20f * std::sin(6.2832f * u * 2.0f);
    const float dy = dv / (dv < 0 ? 0.012f : 0.26f);
    return 0.42f * std::exp(-dy * dy);
  });
  ImgId r2 = texture(a, "rm:xwave2", 1024, 192, [](float u, float v) {
    const float dv = v - 0.40f + 0.18f * std::sin(6.2832f * u * 3.0f + 2.0f);
    const float dy = dv / (dv < 0 ? 0.016f : 0.20f);
    return 0.34f * std::exp(-dy * dy);
  });
  const auto ribbon = [&](ImgId id, float y, float hh, float secs_per_period, uint8_t al) {
    const float W = g.w * 2.0f, off = std::fmod(state().wave_t / secs_per_period, 1.0f) * W;
    ops.push_back(Op::image(id, -off, y, W, hh, Color{255, 255, 255, al}));
    ops.push_back(Op::image(id, W - off, y, W, hh, Color{255, 255, 255, al}));
  };
  ribbon(r1, g.h * 0.42f, g.h * 0.40f, 70.0f, 62);
  ribbon(r2, g.h * 0.46f, g.h * 0.36f, 110.0f, 48);
}

// Text that stays readable over the moving light: a thin shadow under what matters; the faint rows go without one.
void xtext(Assets& a, std::vector<Op>& ops, std::string_view s, float x, float cy, int px, Color c, float max_w = 0) {
  if (c[3] >= 150) {
    g_audit_quiet = true;  // the shadow is part of the text, not another box
    text(a, ops, s, x + 1, cy + 1, px, Color{4, 8, 40, uint8_t(c[3] * 0.6f)}, max_w);
    g_audit_quiet = false;
  }
  text(a, ops, s, x, cy, px, c, max_w);
}

// The cross on the grid: everything hangs from the second column, where the chosen icon starts. Heights go in whole cells: a text lives in a
// band of cells a little taller than its line (the band, not the text, is what sits on the lines).
struct XHome {
  float x, icon, icon_y, cy, name_y, count_y, end;
  int name_px, count_px;
};
int text_cells(const Grid& g, int px) { return std::max(1, int(std::ceil(float(px) * 1.25f / float(g.u)))); }
XHome xmb_home(const Grid& g) {
  XHome o;
  o.x = g.col(1);
  o.icon = std::min(float(g.snap(float(g.P(84)))), g.span(2));
  const int first = (g.content_top() - g.top()) / g.u;  // the first line under the bar
  o.icon_y = float(g.row(std::max(first, int(std::lround((std::max(g.h * 0.27f - o.icon / 2.0f, 0.0f) - float(g.top())) / float(g.u))))));
  o.cy = o.icon_y + o.icon / 2.0f;
  o.name_px = g.F(26);
  o.count_px = g.F(15);
  const float nb = float(g.U(text_cells(g, o.name_px))), cb = float(g.U(text_cells(g, o.count_px)));
  const float under = o.icon_y + o.icon + float(g.u);
  o.name_y = under + nb / 2.0f;
  o.count_y = under + nb + cb / 2.0f;
  o.end = under + nb + cb;
  return o;
}
struct XRow {
  float x = 0, text_x = 0, label_y = 0, top = 0, tile_w = 0, tile_h = 0, pitch = 0;
  int label_px = 0, fit = 0;
};
XRow xmb_row(const Grid& g) {
  const XHome o = xmb_home(g);
  XRow r;
  r.x = o.x;
  r.text_x = g.col(2);
  r.tile_w = g.colw();
  const int tc = std::max(1, int(g.colw() * 0.75f) / g.u);
  r.tile_h = float(g.U(tc));
  r.label_px = g.F(14);
  const float lb = float(g.U(text_cells(g, r.label_px)));
  r.label_y = o.end + float(g.u) + lb / 2.0f;
  r.top = o.end + float(g.u) + lb + float(g.u);
  int pc = std::max(tc + 2, (g.F(16, 13) + g.F(12, 11) + g.P(10) + g.u - 1) / g.u);
  if ((pc - tc) & 1) ++pc;  // the tile is centred in its row and keeps both edges on the lines
  r.pitch = float(g.U(pc));
  r.fit = std::clamp(int((float(g.help_top()) - r.top) / r.pitch), 0, 3);  // what does not fit goes, it is not squeezed
  return r;
}

void xmb_system(const Theme& th, Assets& a, const Grid& g, const std::vector<SystemItem>& items, int cursor, const SystemItem& cur, float cam, const Status& status, RecentRow* recent, const InfoFade& fade, std::vector<Op>& ops) {
  xmb_back(th, a, g, ops);
  const int n = int(items.size());
  const XHome o = xmb_home(g);
  const float pitch = g.span(2) + g.gutter(), cx = o.x + g.span(2) / 2.0f, cy0 = o.cy;
  const int mid = int(std::floor(cam));
  int first_i = 0, last_i = 0;
  xmb_span(n, mid, first_i, last_i);
  for (int i = first_i; i <= last_i; ++i) {
    const float d = float(i) - cam, ad = std::fabs(d);
    const float k = std::max(0.0f, 1.0f - ad);
    const float size = lerp(o.icon * 0.52f, o.icon, k);
    const float x = cx + d * pitch;
    if (x + size / 2.0f < 0.0f || x - size / 2.0f > g.w) continue;
    const float dim = d < 0 ? lerp(255, 70, std::min(ad, 1.0f)) : ad <= 1 ? lerp(255, 175, ad) : lerp(175, 110, std::min(ad - 1, 3.0f) / 3.0f);
    const SystemItem& it = items[size_t(rem_e(i, n))];
    if (k > 0.3f) {
      disc(a, ops, x, cy0, size * 1.5f, Color{170, 200, 255, uint8_t(60.0f * k)});
      disc(a, ops, x, cy0, size * 0.85f, Color{255, 255, 255, uint8_t(95.0f * k)});
    }
    ImgId id;
    std::string p = sys_file(th, "_inc/systems/controller", it.theme_name, ".png");
    if (p.empty()) {
      p = th.path("./assets/logos/" + it.theme_name + ".webp");
      if (!exists_once(p)) p.clear();
    }
    if (!p.empty() && pic(a, p, o.icon, o.icon, true, id)) put_fit(a, ops, id, x - size / 2.0f, cy0 - size / 2.0f, size, size, Color{255, 255, 255, uint8_t(dim)});
  }
  rec(cx - o.icon / 2.0f, cy0 - o.icon / 2.0f, o.icon, o.icon, "icono", 'C', false, true);
  const size_t info = ops.size();
  if (cur.games != kCountPending || cur.sys == kItemAddGames) {  // the "Add games" card: its name, never a count
    const float room = g.w - float(g.margin()) - o.x;
    int px = o.name_px;
    while (px > g.F(18) && a.measure(cur.full_name, float(px)) > room) --px;
    xtext(a, ops, cur.full_name, o.x, o.name_y, px, kWhite, room);
    if (cur.games != kCountPending) xtext(a, ops, games_low(cur.games), o.x, o.count_y, o.count_px, Color{255, 255, 255, 205});
  }
  motion::fade(ops, info, fade.alpha);
  // continue playing, hanging from the icon
  if (recent && !recent->items.empty() && cur.games != kCountPending) {
    const XRow r = xmb_row(g);
    if (r.fit > 0) {
      recent->box_w = uint32_t(r.tile_w);
      recent->box_h = uint32_t(r.tile_h);
      text(a, ops, i18n::sentence_case(tr("CONTINUE PLAYING")), r.x, r.label_y, r.label_px, Color{255, 255, 255, 170});  // no plate, no shadow: the text's own opacity gives the contrast
      int k = 0;
      for (size_t idx = 0; idx < recent->items.size() && k < r.fit; ++idx) {
        const RecentItem& it = recent->items[idx];
        if (covers::failed(it.shot_key()) && !g_audit) continue;
        const float cy = r.top + float(k) * r.pitch + r.pitch / 2.0f;
        const bool foc = int(idx) == recent->focus && recent->focus_a > 0.0f;
        if (foc) {  // the row with the keys must show at a glance: a plate from the column to the margin
          beam(a, ops, 0, cy - r.pitch / 2.0f, g.w, r.pitch, 80);
          const float pad = g.gutter() / 2.0f;
          g_shapes.rrect(a, ops, r.x - pad, cy - r.pitch / 2.0f + 1.0f, g.w - float(g.margin()) - r.x + pad, r.pitch - 2.0f, g.P(6), Color{255, 255, 255, 46});
        }
        g_shapes.rrect(a, ops, r.x - 1, cy - r.tile_h / 2.0f - 1, r.tile_w + 2, r.tile_h + 2, g.P(4), foc ? kWhite : Color{255, 255, 255, 50});
        ImgId id;
        const std::string key = it.shot_key();
        const bool have = recent->decode_inline ? a.file_fit(key, recent->box_w, recent->box_h, id) : covers::ready(a, key, recent->box_w, recent->box_h, id);
        if (!have && recent->decode_inline) covers::mark_failed(key);
        if (have) put_fit(a, ops, id, r.x, cy - r.tile_h / 2.0f, r.tile_w, r.tile_h);
        rec(r.x, cy - r.tile_h / 2.0f, r.tile_w, r.tile_h, "miniatura", 'L', true, true);
        const float room = g.w - r.text_x - float(g.margin());
        const int al = foc ? 255 : 200 - 40 * k, tpx = g.F(16, 13), apx = g.F(12, 11);
        if (it.ago.empty()) {
          text(a, ops, short_title(it.name), r.text_x, cy, tpx, Color{255, 255, 255, uint8_t(al)}, room);
        } else {
          text(a, ops, short_title(it.name), r.text_x, cy - float(apx) / 2.0f - 1.0f, tpx, Color{255, 255, 255, uint8_t(al)}, room);
          text(a, ops, it.ago, r.text_x, cy + float(tpx) / 2.0f + 1.0f, apx, Color{255, 255, 255, uint8_t(al * 3 / 4)}, room);
        }
        ++k;
      }
    }
  }
  (void)cursor;
  status_bar(th, a, g.w, g.h, status, ops);
  HelpLook hl;
  hl.fg = Color{255, 255, 255, 165};
  hl.cy = float(g.help_y());
  hl.right = g.w - float(g.margin());
  help(th, a, g, recent && recent->focus_help ? recent_help() : system_help(), hl, ops);
}

void xmb_list(const Theme& th, Assets& a, const Grid& g, const std::string& system_name, const std::vector<std::string>& names, size_t cursor, const Status& status, const DetailedData* d, std::vector<Op>& ops) {
  xmb_back(th, a, g, ops);
  const float x0 = g.col(1), tx = g.col(2), colx = g.col(8), colw = g.span(4), textw = g.span(6);
  const float u = float(g.u);
  const float isz = float(g.snap(g.colw())), icy = float(g.top()) + std::max(isz, float(g.bar_h())) / 2.0f;  // a column, on whole cells
  const std::string ctrl = sys_file(th, "_inc/systems/controller", g_state.system_theme, ".png");
  ImgId id;
  disc(a, ops, x0 + isz / 2.0f, icy, isz, Color{170, 200, 255, 45});
  disc(a, ops, x0 + isz / 2.0f, icy, isz * 0.6f, Color{255, 255, 255, 60});
  if (!ctrl.empty() && pic(a, ctrl, g.colw(), g.colw(), true, id)) put_fit(a, ops, id, x0, icy - isz / 2.0f, isz, isz);
  rec(x0, icy - isz / 2.0f, isz, isz, "icono", 'L', true, true);
  std::string cased_sys;
  xtext(a, ops, list_cased(th, system_name, cased_sys), tx, icy, g.F(17), Color{255, 255, 255, 225}, textw);
  const float icon_bottom = icy + isz / 2.0f;
  // Every row is a band of whole cells with its tile centred in it, so tile and row edges sit on the lines; the chosen row is the tall one
  // (its title is bigger), its data under it.
  const int title_px = g.F(22, 15), meta_px = g.F(14), fs = g.F(18, 14);
  const float tile_w = g.colw();
  int tc = std::max(1, int(g.colw() * 0.75f) / g.u);
  const int hbc = std::max(tc, text_cells(g, title_px));
  if ((hbc - tc) & 1) tc = tc + 1 <= hbc ? tc + 1 : tc - 1;
  const float tile_h = float(g.U(tc)), hb = float(g.U(hbc)), mb = float(g.U(text_cells(g, meta_px)));
  int pc = std::max(tc + 2, (fs + g.P(8) + g.u - 1) / g.u);
  if ((pc - tc) & 1) ++pc;
  const float pitch = float(g.U(pc));
  // the chosen row crosses the screen here, with its data under it; the other rows above and below
  const float ct = std::max(icon_bottom + g.gutter(), float(g.top()) + std::round((g.h * 0.45f - hb / 2.0f - float(g.top())) / u) * u);  // top of the chosen band
  const float cy0 = ct + hb / 2.0f;
  const float meta_y = ct + hb + mb / 2.0f;
  const float slide = g_state.list_cam >= 0.0f ? (float(cursor) - g_state.list_cam) * pitch : 0.0f;
  // the cover on the right: its box on the cells, its bottom on the bottom of the chosen tile
  float cw = 0, ch = 0;
  if (d) {
    box43(colw, cy0 + tile_h / 2.0f - float(g.content_top()), cw, ch);
    ch = float(g.U(std::max(1, int(ch) / g.u)));
    cw = std::min(colw, float(g.U(std::max(1, int(ch / 0.75f) / g.u))));
  }
  // The game's own pictures ask first: only the newest request of a frame counts and the first picture a frame misses is the one asked for,
  // so the thumbnails of the rows (drawn before them) must not take the slot of the cover the user is waiting for.
  if (d && !d->image.empty() && exists_once(d->image)) {
    ImgId first;
    pic(a, cover_key(d->image, g.P(4)), tile_w, tile_h, true, first);
    pic(a, cover_key(d->image, g.P(6)), cw, ch, true, first);
  }
  size_t shown = 1;  // the chosen row and the others drawn: what L / R page by
  auto row = [&](size_t k, float y, int al) {
    y += slide;
    ++shown;
    const std::string im = g_state.row_image ? g_state.row_image(k) : std::string();
    ImgId t;
    // the whole picture inside the tile (it used to be cut to 4:3: a portrait cover lost its top and bottom)
    if (!im.empty() && exists_once(im) && pic(a, cover_key(im, 3), tile_w, tile_h, true, t)) put_fit(a, ops, t, x0, y - tile_h / 2.0f, tile_w, tile_h, Color{255, 255, 255, uint8_t(al)});
    else g_shapes.rrect(a, ops, x0, y - tile_h / 2.0f, tile_w, tile_h, g.P(4), Color{255, 255, 255, uint8_t(al / 5)});
    rec(x0, y - tile_h / 2.0f, tile_w, tile_h, "miniatura", 'L', true, true);
    std::string cb;
    xtext(a, ops, list_cased(th, names[k], cb), tx, y, fs, Color{255, 255, 255, uint8_t(al)}, textw);
  };
  if (!names.empty()) {
    for (int k = 1; k <= 6; ++k) {
      const float y = ct - pitch / 2.0f - float(k - 1) * pitch;
      if (y - pitch / 2.0f < icon_bottom || size_t(k) > cursor) break;
      row(cursor - size_t(k), y, std::max(30, 195 - (k - 1) * 65));
    }
    const float below = ct + hb + (d ? mb : 0.0f);  // where the rows under the chosen one start
    for (int k = 1; k <= 12; ++k) {
      const float y = below + pitch / 2.0f + float(k - 1) * pitch;
      if (y + pitch / 2.0f > float(g.help_top()) || cursor + size_t(k) >= names.size()) break;
      row(cursor + size_t(k), y, std::max(26, 195 - (k - 1) * 60));
    }
    g_state.rows_shown = shown;
    beam(a, ops, 0, ct - float(g.P(2)), g.w, hb + float(g.P(4)), 80);
    cover_box(a, ops, d ? d->image : std::string(), x0, cy0 + slide - tile_h / 2.0f, tile_w, tile_h, g.P(4), ctrl, Color{255, 255, 255, 40}, Color{255, 255, 255, 170}, Color{0, 0, 0, 0});
    std::string cb_cur;
    xtext(a, ops, list_cased(th, names[cursor], cb_cur), tx, cy0 + slide, title_px, kWhite, textw);
    if (d) {
      xtext(a, ops, meta_of(*d), tx, meta_y, meta_px, Color{255, 255, 255, 205}, textw);
      const float cover_bottom = cy0 + tile_h / 2.0f;
      cover_box(a, ops, d->image, colx, cover_bottom - ch, cw, ch, g.P(6), ctrl, Color{255, 255, 255, 36}, Color{255, 255, 255, 70}, Color{0, 0, 0, 120}, d->video);
      float yd = cover_bottom + g.gutter();
      if (d->show_rating) {
        stars(th, a, ops, colx, yd, g.P(16), d->rating, kWhite, Color{255, 255, 255, 80}, g.P(3));
        yd += float(g.P(16)) + g.gutter();
      }
      desc_block(a, ops, *d, colx, yd, colw, float(g.help_top()) - yd, g.F(14), Color{255, 255, 255, 210});
    }
  }
  status_bar(th, a, g.w, g.h, status, ops);
  HelpLook hl;
  hl.fg = Color{255, 255, 255, 165};
  hl.cy = float(g.help_y());
  hl.right = g.w - float(g.margin());
  help(th, a, g, list_help(), hl, ops);
}

// ---- Art Book --------------------------------------------------------------------------------------------------------------------------------

Color readable_on_dark(Color c) {
  auto lum = [](Color x) {
    const auto f = [](float u) { return u <= 0.03928f ? u / 12.92f : std::pow((u + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * f(float(x[0]) / 255) + 0.7152f * f(float(x[1]) / 255) + 0.0722f * f(float(x[2]) / 255);
  };
  for (int i = 0; i < 24 && (lum(c) + 0.05f) / 0.053f < 4.5f; ++i) c = motion::mix(c, kWhite, 0.08f);
  return c;
}
Color readable_on_light(Color c);
/// A fill that carries white text (3:1 at least). The accent of a system without an illustration is the theme's own, which on a dark skin is light
/// (Swiss: white): then the fill is taken towards black until the white name reads on it.
Color holds_white(Color c) {
  const auto f = [](float u) { return u <= 0.03928f ? u / 12.92f : std::pow((u + 0.055f) / 1.055f, 2.4f); };
  const float l = 0.2126f * f(float(c[0]) / 255) + 0.7152f * f(float(c[1]) / 255) + 0.0722f * f(float(c[2]) / 255);
  return 1.05f / (l + 0.05f) >= 3.0f ? c : readable_on_light(c);
}
Color readable_on_light(Color c) {
  auto lum = [](Color x) {
    const auto f = [](float u) { return u <= 0.03928f ? u / 12.92f : std::pow((u + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * f(float(x[0]) / 255) + 0.7152f * f(float(x[1]) / 255) + 0.0722f * f(float(x[2]) / 255);
  };
  for (int i = 0; i < 24 && 1.05f / (lum(c) + 0.05f) < 4.5f; ++i) c = motion::mix(c, Color{0, 0, 0, 255}, 0.08f);
  return c;
}

/// `fg` as it is when it already reads on `page` (contrast 4.5:1 or more); otherwise pulled towards white (dark page) or black (light page)
/// until it does. The ink of a theme is made for its own page: a skin with a page of another colour (Swiss dark is blue) needs this.
Color legible_on(Color fg, Color page) {
  auto lum = [](Color x) {
    const auto f = [](float u) { return u <= 0.03928f ? u / 12.92f : std::pow((u + 0.055f) / 1.055f, 2.4f); };
    return 0.2126f * f(float(x[0]) / 255) + 0.7152f * f(float(x[1]) / 255) + 0.0722f * f(float(x[2]) / 255);
  };
  const auto ratio = [&](Color a, Color b) {
    const float la = lum(a), lb = lum(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
  };
  const Color toward = lum(page) < 0.4f ? kWhite : Color{0, 0, 0, 255};
  for (int i = 0; i < 24 && ratio(fg, page) < 4.5f; ++i) fg = motion::mix(fg, toward, 0.1f);
  return fg;
}

// ---- on the cells (Art Book, Iconic, TechDweeb) ----

/// Box of a 4:3 picture `w` wide that fits under `max_h`, on whole cells: its height and, when `max_h` is what limits it, its width too.
void box43c(const Grid& g, float w, float max_h, float& ow, float& oh) {
  const float u = float(g.u);
  oh = std::max(1.0f, std::floor(w * 0.75f / u)) * u;
  ow = w;
  if (oh > max_h) {
    oh = std::max(1.0f, std::floor(max_h / u)) * u;
    ow = std::max(1.0f, std::floor(oh / 0.75f / u)) * u;
  }
}
/// The line of the cells at or over `v`, counted down from `top` (a line): never over `top` itself.
float down_to_line(const Grid& g, float top, float v) { return top + std::floor(std::max(0.0f, v - top) / float(g.u)) * float(g.u); }
/// A length of the old design as whole cells (at least `min`).
float cells_of(const Grid& g, float design_px, int min = 1) { return float(g.snap(float(g.P(design_px)), min)); }
/// The width of a pill around text `text_w` wide with `pad` px at each side: whole cells (an even number with `even`, to centre it on a line).
float pill_w(const Grid& g, float text_w, float pad, bool even = false) {
  int n = std::max(1, int(std::ceil((text_w + 2.0f * pad) / float(g.u) - 0.02f)));
  if (even && n % 2) ++n;
  return float(n * g.u);
}
/// The chips under a title (year, genre, players): pills of whole cells side by side from `x`, tops on `top` (a line), `ph` tall, their text a
/// `pad` in and centred on them; the next one that would pass `limit` is left out.
void chips(Assets& a, std::vector<Op>& ops, const DetailedData& d, const Grid& g, float x, float top, float ph, float limit, int px, float pad, Color fill, Color ink) {
  char yr[16] = "";
  if (d.year >= 0) std::snprintf(yr, sizeof yr, "%d", int(d.year));
  float cx = x;
  for (std::string_view s : {std::string_view(yr), trim(d.genre), trim(d.players)}) {
    if (s.empty()) continue;
    const float pw = pill_w(g, a.measure(s, float(px)), pad);
    if (cx + pw > limit) break;
    rec(cx, top, pw, ph, "tarjeta", 'B', false, true);
    g_shapes.pill(a, ops, cx, top, pw, ph, fill);
    const Free in_pill;
    text(a, ops, s, cx + pad, top + ph / 2.0f, px, ink);
    cx += pw + float(g.u);
  }
}

/// The light of a picture: very small and blurred, stretched. Also leaves its strongest colour in `accent`.
bool ambient_baked(Assets& a, const std::string& b, ImgId& id, Color& accent) {
  if (!pic(a, b, 32, 24, true, id)) return false;
  Assets::accent_of(b, accent);
  return true;
}
bool ambient_of(Assets& a, const std::string& path, ImgId& id, Color& accent) { return ambient_baked(a, baked(path, "a"), id, accent); }

std::string g_art_system;  // the system the menu of this theme takes its colour from

/// Behind the menu: the picture of the system, only as a dark haze of its colours.
void art_menu_back(const Theme& th, Assets& a, const Grid& g, std::vector<Op>& ops) {
  if (g_art_system.empty()) return;
  // the path (with the existence of its file) is made when the theme folder, the system or the games change: a frame at rest builds no string
  static struct {
    std::string dir, sys, baked;
    unsigned gen = ~0u;
  } c;
  const std::string_view dir = th.str("esDir", "");
  if (c.gen != games_generation() || c.sys != g_art_system || c.dir != dir) {
    c.gen = games_generation();
    c.sys = g_art_system;
    c.dir.assign(dir);
    const std::string art = sys_file(th, "_inc/systems/artwork-default", g_art_system, ".png");
    c.baked = art.empty() ? std::string() : baked(art, "a");
  }
  ImgId am;
  Color got = kWhite;
  if (c.baked.empty() || !ambient_baked(a, c.baked, am, got)) return;
  ops.push_back(Op::image(am, 0, 0, g.w, g.h));
  ops.push_back(Op::rect(0, 0, g.w, g.h, Color{10, 10, 14, 195}));
}

void art_system(const Theme& th, Assets& a, const Grid& g, const std::vector<SystemItem>& items, const SystemItem& cur, float cam, const Status& status, const InfoFade& fade, std::vector<Op>& ops) {
  g_art_system = cur.theme_name;
  ops.push_back(Op::rect(0, 0, g.w, g.h, Color{0, 0, 0, 255}));
  const int n = int(items.size());
  const float sh = float(g.bottom() - g.snap(float(g.P(104) - g.top()), 6)), band = g.h - sh;  // the band starts on a line of the cells
  const float sw = std::round(sh * 454.0f / 1080.0f), big = std::round(sh * 1.165f), bw = std::round(big * 454.0f / 1080.0f);
  const float pitch = std::floor(sw * 0.85f);
  const int mid = int(std::floor(cam));
  const int reach = int(g.w / pitch) / 2 + 2;
  std::vector<int> order;
  for (int k = reach + 1; k >= 0; --k) {
    order.push_back(mid - k);
    if (k >= 0) order.push_back(mid + 1 + k);
  }
  std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return std::fabs(float(x) - cam) > std::fabs(float(y) - cam); });
  for (int i : order) {
    const float d = float(i) - cam, ad = std::fabs(d);
    const float k = std::max(0.0f, 1.0f - ad);
    const SystemItem& it = items[size_t(rem_e(i, n))];
    const std::string p = sys_file(th, "_inc/systems/artwork-default", it.theme_name, ".png");
    const float cx = g.w / 2.0f + d * pitch + (d > 0 ? 1.0f : -1.0f) * std::min(ad, 1.0f) * sw * 0.15f;
    if (cx + bw < 0 || cx - bw > g.w) continue;
    if (p.empty()) {
      if (k > 0.5f) ops.push_back(Op::rect(cx - sw / 2.0f, 0, sw, sh, th.color("cyanSoft")));
      continue;
    }
    ImgId id;
    if (k < 1.0f && pic(a, baked(p, "g"), sw, sh, true, id)) {
      const float ow = float(a.img(id).w) * sh / float(a.img(id).h);
      ops.push_back(Op::image(id, cx - ow / 2.0f, 0, ow, sh, Color{110, 110, 110, uint8_t(160.0f * (1 - k * k))}));
    }
    if (k > 0.0f && pic(a, p, bw, big, true, id)) {
      const float hh = lerp(sh, big, k), ww = hh * float(a.img(id).w) / float(a.img(id).h);
      if (k > 0.5f) soft(a, ops, cx - ww / 2.0f, 0, ww, sh, float(g.P(14)), Color{0, 0, 0, uint8_t(220.0f * k)});
      ops.push_back(Op::image(id, cx - ww / 2.0f, 0, ww, hh, Color{255, 255, 255, uint8_t(255.0f * std::min(1.0f, k * 1.6f))}));
    }
  }
  if (at_rest(cam)) {  // on the job thread, one by one: the neighbours are ready before the move, and the logo of the list before it opens
    for (int dd : {1, -1, 2, -2}) {
      const std::string np = sys_file(th, "_inc/systems/artwork-default", items[size_t(rem_e(int(std::round(cam)) + dd, n))].theme_name, ".png");
      ImgId nid;
      if (!np.empty()) (void)(pic(a, np, bw, big, true, nid) && pic(a, baked(np, "g"), sw, sh, true, nid));
    }
    const std::string ll = sys_file(th, "_inc/systems/logos", cur.theme_name, ".svg");
    ImgId nid;
    if (!ll.empty()) pic(a, baked(ll, "w"), float(g.P(180)), float(g.P(32)), true, nid);
  }
  top_fade(a, ops, g.w, float(g.P(60)), Color{0, 0, 0, 200});
  ops.push_back(Op::rect(0, sh, g.w, band, rgb(0x08080A)));
  const std::string art = sys_file(th, "_inc/systems/artwork-default", cur.theme_name, ".png");
  Color acc = th.color("dim");
  if (!art.empty()) {
    ImgId am;
    Color c = acc;
    if (ambient_of(a, art, am, c)) acc = readable_on_dark(c);
  }
  ops.push_back(Op::rect(0, sh, g.w, float(std::max(2, g.P(2))), acc));
  const size_t info = ops.size();
  const int m = g.margin();
  const std::string logo = sys_file(th, "_inc/systems/logos", cur.theme_name, ".svg");
  ImgId lg;
  if (!logo.empty() && pic(a, baked(logo, "w"), g.w * 0.39f, float(g.P(44)), true, lg)) {
    put_fit(a, ops, lg, float(m), sh + float(g.U(1)), g.w * 0.39f, cells_of(g, 44), kWhite, 0.0f);
  } else if (logo.empty() || covers::failed(baked(logo, "w"))) {
    text_alt(a, ops, cur.full_name, float(m), sh + float(g.P(32)), g.F(22, 15), kWhite, g.w * 0.45f);
  }
  if (cur.games != kCountPending) {
    text_alt(a, ops, cur.full_name, g.w - float(m), sh + float(g.P(20)), g.F(16), rgb(0xF0F0F0), g.w * 0.48f, R);
    text(a, ops, games_low(cur.games), g.w - float(m), sh + float(g.P(42)), g.F(14), acc, 0, R);
  }
  motion::fade(ops, info, fade.alpha);
  status_bar(th, a, g.w, g.h, status, ops);
  HelpLook hl;
  hl.fg = rgb(0xA5A5AC);
  hl.cy = float(g.help_y());
  hl.right = g.w - float(g.margin());
  help(th, a, g, system_help(), hl, ops);
}

void art_list(const Theme& th, Assets& a, const Grid& g, const std::vector<std::string>& names, size_t cursor, const Status& status, const DetailedData* d, std::vector<Op>& ops) {
  g_art_system = g_state.system_theme;
  ops.push_back(Op::rect(0, 0, g.w, g.h, rgb(0x08080A)));
  const float pw = g.col(5) - g.gutter();
  const int m = g.margin();
  Color acc = kWhite;
  const std::string art = sys_file(th, "_inc/systems/artwork-default", g_state.system_theme, ".png");
  const std::string image = d && !d->image.empty() && exists_once(d->image) ? d->image : std::string();
  ImgId am;
  Color got = acc;
  if (!image.empty() && ambient_of(a, image, am, got)) {
    ops.push_back(Op::image(am, pw, 0, g.w - pw, g.h));
    ops.push_back(Op::rect(pw, 0, g.w - pw, g.h, Color{10, 10, 14, 150}));
    acc = readable_on_dark(got);
  } else if (image.empty() && !art.empty() && ambient_of(a, art, am, got)) {
    ops.push_back(Op::image(am, pw, 0, g.w - pw, g.h));
    ops.push_back(Op::rect(pw, 0, g.w - pw, g.h, Color{10, 10, 14, 215}));
    acc = readable_on_dark(got);
  }
  const std::string logo = sys_file(th, "_inc/systems/logos", g_state.system_theme, ".svg");
  ImgId lg;
  if (!logo.empty() && pic(a, baked(logo, "w"), float(g.P(180)), float(g.P(32)), true, lg)) put_fit(a, ops, lg, float(m), float(g.top()), cells_of(g, 180), float(g.bar_h()), kWhite, 0.0f);
  const int y0 = g.content_top(), pitch = g.snap(float(g.P(38)), 3), fs = g.F(17, 14);
  const size_t rows = size_t(std::max(1, (g.help_top() - y0) / pitch));
  const RowWindow rw = row_window(names.size(), cursor, rows);
  if (rw.moving) ops.push_back(Op::clip(0, float(y0), pw, float(rows) * float(pitch)));
  if (!names.empty()) {  // the selected row: a band of whole cells, from the edge of the screen to the column
    const float ys = float(y0) + rw.sel * float(pitch);
    rec(float(m), ys, pw - float(m), float(pitch), "tarjeta", 'C', false, true);  // the band bleeds to the edge of the screen: its row is what the margins hold
    ops.push_back(Op::rect(0, ys, pw, float(pitch), acc == kWhite ? Color{255, 255, 255, 40} : alpha(acc, 92)));
    ops.push_back(Op::rect(0, ys, float(std::max(3, g.P(6))), float(pitch), acc));
  }
  const float tw_max = pw - float(m + g.U(2));
  std::string cased_row;
  for (size_t k = rw.first; k < rw.end; ++k) {
    const float y = float(y0) + (float(k) - rw.start) * float(pitch) + float(pitch) / 2.0f;
    if (k == rw.lit) {
      text_alt(a, ops, list_cased(th, names[k], cased_row), float(m), y, fs, kWhite, tw_max);
    } else {
      const size_t dist = k > cursor ? k - cursor : cursor - k;
      const int v = std::max(92, 175 - int(dist) * 16);
      text(a, ops, list_cased(th, names[k], cased_row), float(m), y, fs, Color{uint8_t(v), uint8_t(v), uint8_t(v + 4), 255}, tw_max);
    }
  }
  if (rw.moving) ops.push_back(Op::unclip());
  if (d) {
    const float x = pw + g.gutter(), w = g.w - x - float(m);
    const bool has = has_data(*d);
    const int title = g.F(24, 15), body = g.F(14), lh = g.F(20, 16);
    const float reserve = float(g.U(3) + (has ? g.U(8) + 2 * lh : g.U(5)));
    float top = float(g.content_top()), cw, ch;
    box43c(g, w, float(g.help_top()) - top - reserve, cw, ch);
    if (!has) top = down_to_line(g, float(g.content_top()), (float(g.help_top() + g.content_top()) - ch - reserve) / 2.0f);
    cover_box(a, ops, image, x, top, cw, ch, g.P(8), logo.empty() ? std::string() : baked(logo, "w"), Color{255, 255, 255, 18}, Color{0, 0, 0, 0}, Color{0, 0, 0, 170}, d->video);
    const float yt = top + ch + float(g.U(3));
    std::string cased_name;
    text_alt(a, ops, list_cased(th, d->name, cased_name), x, yt, title, kWhite, w);
    if (has) {
      const int cf = g.F(13, 11), star = g.P(14);
      const float ph = cells_of(g, 24), star_w = float(5 * (star + g.P(3)));
      const float row = yt + float(g.U(3));
      chips(a, ops, *d, g, x, row, ph, x + w - star_w, cf, cells_of(g, 10), Color{255, 255, 255, 30}, rgb(0xEBEBF0));
      if (d->show_rating) stars(th, a, ops, x + w - star_w, row + (ph - float(star)) / 2.0f, star, d->rating, acc, Color{255, 255, 255, 50}, g.P(3));
      const float yd = yt + float(g.U(8));
      desc_block(a, ops, *d, x, yd, w, float(g.help_top()) - yd, body, rgb(0xD7D7DC));
    } else if (d->show_rating) {
      stars(th, a, ops, x, yt + float(g.U(2)), g.P(16), d->rating, acc, Color{255, 255, 255, 60}, g.P(3));
    }
  }
  status_bar(th, a, g.w, g.h, status, ops);
  HelpLook hl;
  hl.fg = rgb(0xCDCDD2);
  hl.cy = float(g.help_y());
  hl.right = g.w - float(g.margin());
  help(th, a, g, list_help(), hl, ops);
}

// ---- Iconic ----------------------------------------------------------------------------------------------------------------------------------

const Color kInk = rgb(0x1C1E26), kDim = rgb(0x5C5F6C), kBody = rgb(0x464953);

/// What the user's choices of the theme (COLOR SCHEME, PLAY VIDEO PREVIEWS, SHOW DETAILED GAME INFO) change in Iconic's pages: the generator
/// writes them as the `ic*` variables of theme.xml (es_theme_map.cpp, `iconic_look`). Without them (a theme folder of an older run) it is the light page.
struct IconicInk {
  Color ink = kInk, body = kBody, dim = kDim, card = kWhite, star = rgb(0xCDD0DC);
  bool dark = false, video = true, detail = true;
};
IconicInk iconic_ink(const Theme& th) {
  IconicInk s;
  if (!th.has("icInk")) return s;
  s.ink = th.color("icInk");
  s.body = th.color("icBody");
  s.dim = th.color("icDim");
  s.card = th.color("icCard");
  s.star = th.color("icStar");
  s.dark = th.str("icDark", "0") == "1";
  s.video = th.str("icVideo", "1") != "0";
  s.detail = th.str("icDetail", "1") != "0";
  return s;
}
/// The accent as text on the page: darkened on the light page, lightened on the dark one.
Color iconic_text(const IconicInk& sc, Color acc) { return sc.dark ? readable_on_dark(acc) : acc; }

/// The colour of the system, taken from its illustration; until it is known, the theme's own.
Color iconic_accent(const Theme& th, Assets& a, const std::string& sys) {
  Color acc = th.color("cyan");
  const std::string p = sys_file(th, "_inc/backgrounds", sys, ".webp");
  if (p.empty()) return acc;
  ImgId id;
  Color c = acc;
  if (ambient_of(a, p, id, c)) {
    // the strongest colour of a dark illustration can be almost black: keep its hue, with life enough to be a colour
    const float mx = float(std::max({c[0], c[1], c[2]})), mn = float(std::min({c[0], c[1], c[2]}));
    if (mx > 0) {
      const float v = std::clamp(mx / 255.0f, 0.55f, 0.8f), s = std::max((mx - mn) / mx, 0.5f);
      for (int k = 0; k < 3; ++k) c[size_t(k)] = uint8_t(std::clamp(v * 255.0f * (1.0f - s * (mx - float(c[size_t(k)])) / std::max(mx - mn, 1.0f)), 0.0f, 255.0f));
    }
    acc = readable_on_light(c);
  }
  return acc;
}

void iconic_page(const Theme& th, Assets& a, const Grid& g, Color acc, std::vector<Op>& ops) {
  ops.push_back(Op::rect(0, 0, g.w, g.h, th.color("bg")));
  corner_light(a, ops, g.w, g.h, alpha(acc, 34));
}

// ---- Aurora: glass cards over the light of the picture --------------------------------------------------------------------------------------
// The page is the colour of the chosen system's illustration, blurred (it is a tiny picture stretched) with three soft blobs of colour over it.
// The system is a card of frosted glass in the middle; its neighbours are smaller, fainter cards that peek out at the sides. Under the card:
// the name, light and large, and a row of dots. Night: white on a deep violet; day: ink on a pale lilac. Nothing is blurred at run time.

void aurora_system(const Theme& th, Assets& a, const Grid& g, const std::vector<SystemItem>& items, int cursor, const SystemItem& cur, float cam, const Status& status, const InfoFade& fade, std::vector<Op>& ops) {
  (void)cursor;  // the dots follow the camera
  (void)cur;     // the name follows the camera too (and with it, the fade of the caller is not needed)
  (void)fade;
  const IconicInk sc = iconic_ink(th);
  const bool dk = sc.dark;
  const Color base = dk ? rgb(0x13113A) : rgb(0xEEF0FF), ink = dk ? kWhite : rgb(0x1D1B3A), dim = dk ? Color{255, 255, 255, 170} : rgb(0x5A5790), acc = dk ? kWhite : rgb(0x3B2FA0);
  const int n = int(items.size());
  const float ctop = float(g.content_top()), u = float(g.u), bot = float(g.help_top()) - float(g.U(1));
  const int lo = int(std::floor(cam));
  ImgId id;
  // The page is always the same (the colour of the theme and its three soft blobs, as in the menus): it does not change with the system
  // under the cursor, so turning the carousel never makes the background flicker or shift.
  ops.push_back(Op::rect(0, 0, g.w, g.h, base));
  disc(a, ops, g.w * 0.10f, g.h * 0.86f, g.w * 0.50f, alpha(dk ? rgb(0x2EE6C8) : rgb(0x7FE8D6), dk ? 190 : 230));
  disc(a, ops, g.w * 0.82f, g.h * 0.08f, g.w * 0.44f, alpha(dk ? rgb(0xFF5FA8) : rgb(0xFFB3D1), dk ? 190 : 235));
  disc(a, ops, g.w * 0.50f, g.h * 0.40f, g.w * 0.40f, alpha(dk ? rgb(0x7A5CFF) : rgb(0xB9A8FF), dk ? 170 : 200));
  ops.push_back(Op::rect(0, 0, g.w, g.h, alpha(base, dk ? 50 : 70)));
  // the cards
  using ui::Level;
  const int title_px = g.L(Level::Title), small_px = g.L(Level::Small), foot_px = g.L(Level::Foot);
  // the name's band holds a line of the smallest name size (Title level) on whole cells
  const float name_h = std::max(cells_of(g, 56, 3), float(g.cells_over(1.25f * float(title_px)) * g.u)), dot = float(std::max(4, g.P(8)));
  const float dots_y = bot - dot, name_cy = dots_y - float(g.U(2)) - name_h / 2.0f;
  const float top = ctop + float(g.U(1));
  const float ch = std::max(u * 8.0f, std::floor((name_cy - name_h / 2.0f - float(g.U(1)) - top) / u) * u);
  const float cw = g.span(8), x0 = g.col(2), cx = x0 + cw / 2.0f;
  const int radius = std::max(6, g.P(30));
  const float ref_pw = std::round(cw * 0.34f), ref_ph = std::round(ref_pw * 0.45f), photo_w = cw * 0.6f, photo_h = ch * 0.5f;
  rec(x0, top, cw, ch, "tarjeta", 'B', true, true);
  g_audit_quiet = true;  // the cards slide: only the one in the middle is measured
  std::vector<int> idx;
  for (int i = lo - 2; i <= lo + 3; ++i) idx.push_back(i);
  std::sort(idx.begin(), idx.end(), [&](int l, int r) { return std::fabs(float(l) - cam) > std::fabs(float(r) - cam); });
  for (int i : idx) {
    const float d = float(i) - cam, ad = std::fabs(d);
    const float k = std::min(ad, 1.0f), k2 = std::clamp(ad - 1.0f, 0.0f, 1.0f), vis = 1.0f - k2;
    if (vis <= 0.004f) continue;  // only a card with no alpha left is skipped: nothing visible pops out
    const float bw = cw * (1.0f - 0.4f * k), bh = ch * (1.0f - 0.12f * k);
    const float off = (d < 0 ? -1.0f : 1.0f) * (k * cw * 0.8f + k2 * cw * 0.3f);
    const float bx = cx + off - bw / 2.0f, by = top + (ch - bh) / 2.0f;
    if (bx + bw < 0.0f || bx > g.w) continue;
    const SystemItem& it = items[size_t(rem_e(i, n))];
    const float cc = (1.0f - k) * vis;  // what only the middle card shows
    soft(a, ops, bx, by + float(g.P(12)), bw, bh, float(g.P(28)), alpha(dk ? rgb(0x0A0832) : rgb(0x3C328C), int((dk ? 90.0f : 44.0f) * vis)));
    g_shapes.rrect(a, ops, bx, by, bw, bh, int(float(radius) * (1.0f - 0.1f * k)), alpha(kWhite, int((dk ? 40.0f : 158.0f) * (1.0f - 0.35f * k) * vis)));
    if (cc > 0.004f) {  // the sheen of the glass
      // Eight rounded layers of the card itself, each a little shorter than the last: a soft light at the top that fades down and can never
      // leave the rounded corners (a rectangular clip let a square corner of light out).
      const int rr = int(float(radius) * (1.0f - 0.1f * k));
      for (int s = 0; s < 8; ++s) {
        const float lh = bh * (1.0f - 0.09f * float(s));
        g_shapes.rrect(a, ops, bx, by, bw, lh, std::min(rr, int(lh / 2.0f)), alpha(kWhite, int((dk ? 7.0f : 19.0f) * cc)));
      }
    }
    std::string ctrl = sys_file(th, "_inc/controllers", it.theme_name, ".webp");
    if (ctrl.empty()) ctrl = es_file(th, "_inc/controllers/_default.webp");
    if (cc > 0.004f && exists_once(ctrl) && pic(a, ctrl, photo_w, photo_h, true, id)) {
      float ow, oh;
      fit_size(a.img(id), photo_w, photo_h, ow, oh);
      const float s = bw / cw, py = by + bh * 0.07f;
      ow *= s;
      oh *= s;
      // Aurora is depth: parallax. The controller is the farthest layer of the card, it travels at 0.7 of the card's own displacement (the plate and
      // the texts, at 1.0, stay with the card), so it lags toward the middle; never further than what keeps it inside the card, clear of the corners.
      const float room = std::max(0.0f, (bw - ow) / 2.0f - float(g.U(1))), par = std::clamp(-0.3f * off, -room, room);
      soft(a, ops, bx + bw / 2.0f - ow * 0.4f + par, py + oh * 0.92f, ow * 0.8f, std::max(1.0f, oh * 0.06f), float(g.P(14)), alpha(dk ? rgb(0x0A0832) : rgb(0x3C328C), int(110.0f * cc)));
      ops.push_back(Op::image(id, std::round(bx + bw / 2.0f - ow / 2.0f + par), std::round(py + (photo_h * s - oh) / 2.0f), ow, oh, Color{255, 255, 255, uint8_t(255.0f * cc)}));
    }
    // the logo on its plate: below the picture on the middle card, in the middle of the card on the others
    // (a side card is half under the middle one: its plate sits in the part that shows)
    const float shown = d < 0 ? x0 - std::max(bx, 0.0f) : std::min(bx + bw, g.w) - (x0 + cw);
    const float pw = std::round(std::min(bw * lerp(0.34f, 0.62f, k), lerp(bw, std::max(shown - float(g.U(3)), ref_pw * 0.5f), k))), ph = std::round(pw * 0.45f);
    const float pcx = k <= 0.0f ? bx + bw / 2.0f : lerp(bx + bw / 2.0f, d < 0 ? std::max(bx, 0.0f) + shown / 2.0f : x0 + cw + shown / 2.0f, k);
    // at rest the plate sits at 62% of the card, but never over the games label under it (a line of Small, two cells from the foot, and a cell of air)
    const float rest_py = std::max(by + bh * 0.45f, std::min(by + bh * 0.62f, by + bh - float(g.U(3)) - 1.25f * float(small_px) - ph));
    const float py = lerp(rest_py, by + (bh - ph) / 2.0f, k), plx = pcx - pw / 2.0f;
    g_shapes.rrect(a, ops, plx, py, pw, ph, int(ph * 0.34f), alpha(kWhite, int(242.0f * vis)));
    const std::string lp = sys_file(th, "_inc/logos", it.theme_name, ".webp");
    const float in_ = std::max(2.0f, ph * 0.14f);
    if (!lp.empty() && pic(a, lp, ref_pw - 2.0f * in_, ref_ph - 2.0f * in_, true, id))
      put_fit(a, ops, id, plx + in_, py + in_, pw - 2.0f * in_, ph - 2.0f * in_, Color{255, 255, 255, uint8_t(255.0f * vis)});
    else if (lp.empty())
      text(a, ops, it.full_name, pcx, py + ph / 2.0f, small_px, alpha(rgb(0x1D1B3A), int(255.0f * vis)), pw - 2.0f * in_, C);
    if (cc > 0.004f) {
      char b[32];
      std::snprintf(b, sizeof b, "%02d / %02d", rem_e(i, n) + 1, n);
      text(a, ops, b, bx + float(g.U(2)), by + float(g.U(2)) + 0.65f * float(foot_px), foot_px, alpha(dim, int(float(dim[3]) * cc)));
      if (it.games != kCountPending) text(a, ops, games_low(it.games), bx + bw / 2.0f, by + bh - float(g.U(2)) - 0.65f * float(small_px), small_px, alpha(ink, int(255.0f * cc)), bw - float(g.U(4)), C);
    }
  }
  g_audit_quiet = false;
  if (at_rest(cam))  // the logos and pictures about to come in, ready before the move
    for (int dd : {-2, -1, 1, 2, 3}) {
      const SystemItem& it = items[size_t(rem_e(int(std::round(cam)) + dd, n))];
      ImgId nid;
      const float in_ = std::max(2.0f, ref_ph * 0.14f);
      const std::string lp = sys_file(th, "_inc/logos", it.theme_name, ".webp");
      if (!lp.empty()) pic(a, lp, ref_pw - 2.0f * in_, ref_ph - 2.0f * in_, true, nid);
      const std::string cp = sys_file(th, "_inc/controllers", it.theme_name, ".webp");
      if (!cp.empty() && std::abs(dd) == 1) pic(a, cp, photo_w, photo_h, true, nid);
    }
  // the name and the dots
  const int min_tpx = std::max(g.F(24, 14), title_px);  // a system name is never smaller than a title
  const int disp_tpx = std::max(g.F(40, 20), min_tpx);  // display size
  const auto name_px = [&](const std::string& s) {
    int p = disp_tpx;
    while (p > min_tpx && a.measure(s, float(p)) > g.span(10)) --p;
    return p;
  };
  const CamPair pair(cam);
  // The name is a cross-fade driven by the camera (not by the cursor): the old one drifts away a couple of cells in the direction of the move
  // while it fades, the new one drifts in from the opposite side. A slow, soft glide, no bounce; at rest only the name under the camera.
  draw_pair(ops, items, pair, 0.0f, RollBand{}, true, [&](const SystemItem& it, int, float s) { text_alt(a, ops, it.full_name, g.w / 2.0f + s * 2.0f * u, name_cy, name_px(it.full_name), ink, g.span(10), C); });
  if (!pair.moving)  // the names of the neighbours, drawn once now, so the move does not have to make a large title in its first frame
    for (int dd : {-2, -1, 1, 2}) {
      const std::string& nm = items[size_t(rem_e(pair.lo + dd, n))].full_name;
      const int p = name_px(nm);  // measured with the main face, as the real draw does, before the title face is on
      a.use_alt_face(true);
      (void)a.text_ellipsis(nm, float(p), ink, g.span(10));
      a.use_alt_face(false);
    }
  {
    // The dots follow the camera, not the cursor: the strip slides one dot as the camera crosses a system and the lit dot widens and
    // narrows continuously. With more systems than dots the window is circular and always centred on the camera (the lit dot in the
    // middle), so it slides through the wrap-around too (j runs past 0 / n-1 and the dot drawn is rem_e(j, n)); with few, the row is fixed.
    const int shown = std::min(n, 9);
    const bool circ = n > shown;
    const float cm = cam - float(n) * std::floor(cam / float(n));  // the camera wrapped into [0, n)
    const float sf = circ ? cm - float(shown / 2) : 0.0f;  // where the strip starts (may be under 0: the window wraps)
    const int j0 = int(std::floor(sf));
    const float gap = float(std::max(4, g.P(12)));
    const float total = float(shown - 1) * (dot + gap) + dot * 3.0f;
    float x = std::round((g.w - total) / 2.0f) - (sf - float(j0)) * (dot + gap);
    const Color off_c = alpha(dk ? kWhite : rgb(0x1D1B3A), dk ? 90 : 64);
    for (int j = j0; j <= j0 + shown && (circ || j < n); ++j) {
      const float rel = float(j) - sf, edge = std::clamp(std::min(rel + 1.0f, float(shown) - rel), 0.0f, 1.0f);  // the two ends fade in / out
      // distance to the camera along the unwrapped line (the window is only `shown` wide, so it is also the circular one when circ)
      const float dj = std::fabs(float(j) - cm), on = std::clamp(1.0f - (circ ? dj : std::min(dj, float(n) - dj)), 0.0f, 1.0f);
      const float w = lerp(dot, dot * 3.0f, on);
      if (edge > 0.004f) {
        const Color c = motion::mix(off_c, acc, on);
        g_shapes.rrect(a, ops, x, dots_y, w, dot, int(dot / 2.0f), alpha(c, int(float(c[3]) * edge)));
      }
      x += w + gap;
    }
  }
  status_pill(th, a, g, status, Color{255, 255, 255, 240}, ops);
  HelpLook hl;
  hl.fg = dk ? Color{255, 255, 255, 230} : rgb(0x3A3768);
  hl.accent = acc;
  hl.cy = float(g.help_y());
  hl.right = g.w - float(g.margin());
  help(th, a, g, system_help(), hl, ops);
}

// ---- Swiss: flat colour, a huge name and a list ----------------------------------------------------------------------------------------------
// Dark mode is the blue page, light mode the paper one. The name of the system is the headline (two lines when it is long), a rule, the count,
// and a list of the neighbours whose chosen row is a solid bar; the controller stands in a disc at the foot of the right side.

void swiss_system(const Theme& th, Assets& a, const Grid& g, const std::vector<SystemItem>& items, int cursor, const SystemItem& cur, float cam, const Status& status, const InfoFade& fade, std::vector<Op>& ops) {
  (void)cursor;  // everything that changes with the system follows the camera, not the cursor
  (void)cur;
  (void)fade;
  const IconicInk sc = iconic_ink(th);
  const bool dk = sc.dark;
  const Color bg = dk ? rgb(0x2540FF) : rgb(0xF3F1EA), ink = dk ? kWhite : rgb(0x12163A), dim = dk ? Color{255, 255, 255, 180} : Color{0x12, 0x16, 0x3A, 160};
  const Color bar = dk ? kWhite : rgb(0x2540FF), bar_ink = dk ? rgb(0x2540FF) : kWhite, round_c = dk ? rgb(0x4660FF) : rgb(0xDFE3FF);
  const int n = int(items.size());
  const float mg = float(g.margin()), ctop = float(g.content_top()), u = float(g.u), bot = float(g.help_top()) - float(g.U(1));
  const int lo = int(std::floor(cam));
  const CamPair pair(cam);
  ImgId id;
  ops.push_back(Op::rect(0, 0, g.w, g.h, bg));
  // the disc and the controller at the foot of the right side
  const float cs = std::max(u * 6.0f, std::floor(std::min(g.span(6), (bot - ctop) * 0.62f) / u) * u);
  const float dcx = g.w - mg - cs / 2.0f, dcy = bot - cs / 2.0f;
  g_shapes.rrect(a, ops, dcx - cs / 2.0f, dcy - cs / 2.0f, cs, cs, int(cs / 2.0f), round_c);
  const float pw_ = cs * 1.08f, ph_ = cs * 0.62f;
  // Swiss moves on one axis, the vertical, and with no ornament: everything that changes with the camera ROLLS up and down (the old one goes
  // up and out, the new one comes up from below; backwards, the other way), by the same rule. The controller slides a cell, the old and the new
  // one at the same time but a cell apart (no ghost in place), without scale.
  g_audit_quiet = true;
  draw_pair(ops, items, pair, 0.0f, RollBand{}, true, [&](const SystemItem& it, int, float s) {
    std::string ctrl = sys_file(th, "_inc/controllers", it.theme_name, ".webp");
    if (ctrl.empty()) ctrl = es_file(th, "_inc/controllers/_default.webp");
    if (!exists_once(ctrl) || !pic(a, ctrl, pw_, ph_, true, id)) return;
    float ow, oh;
    fit_size(a.img(id), pw_, ph_, ow, oh);
    const float dy = s * float(g.U(1));
    soft(a, ops, dcx - ow / 2.0f + ow * 0.1f, dcy + cs * 0.12f + oh * 0.35f + dy, ow * 0.8f, oh * 0.18f, float(g.P(14)), alpha(dk ? rgb(0x0A1478) : rgb(0x2540FF), int(255.0f * (dk ? 0.5f : 0.3f))));
    ops.push_back(Op::image(id, std::round(dcx - ow / 2.0f), std::round(dcy + cs * 0.1f - oh / 2.0f + dy), ow, oh, Color{255, 255, 255, 255}));
  });
  g_audit_quiet = false;
  if (at_rest(cam))
    for (int dd : {-1, 1}) {
      std::string cp = sys_file(th, "_inc/controllers", items[size_t(rem_e(int(std::round(cam)) + dd, n))].theme_name, ".webp");
      ImgId nid;
      if (!cp.empty()) pic(a, cp, pw_, ph_, true, nid);
    }
  // the counter, top left, in the row of the status bar
  using ui::Level;
  const int title_px = g.L(Level::Title), row_px = g.L(Level::Row), small_px = g.L(Level::Small), foot_px = g.L(Level::Foot);
  const float row_cy = float(g.status_y());
  // The index, the headline and the count belong to the system the camera shows and roll together (index and headline at once, the count a beat
  // later: it follows the headline with a small delay). Each rolls inside its own band: the old text goes up and out, the new one comes up from
  // below, with the opacity going the other way; the rule under the headline is structure and does not move.
  {
    a.use_alt_face(true);
    const float ih = a.line_height(float(small_px));
    a.use_alt_face(false);
    draw_pair(ops, items, pair, 0.0f, RollBand{0.0f, row_cy - ih / 2.0f, g.w * 0.5f, ih}, true, [&](const SystemItem&, int idx, float s) {
      char b[32];
      std::snprintf(b, sizeof b, "%02d / %02d", idx + 1, n);
      text_alt(a, ops, b, mg, row_cy + s * 0.6f * ih, small_px, ink);
    });
  }
  // the headline
  const float avail = g.w - 2.0f * mg;
  auto meas = [&](const std::string& s, int px) {
    a.use_alt_face(true);
    const float w = a.measure(s, float(px));
    a.use_alt_face(false);
    return w;
  };
  const float rh =std::max(cells_of(g, 36, 2), float(g.cells_over(1.6f * float(row_px)) * g.u)), list_w = g.span(5);  // a row holds its text at 1.6 lines
  const float idx_w = std::max(float(g.U(4)), float(g.cells_over(a.measure("00", float(foot_px)) + u) * g.u));  // the index column: its two digits and a cell
  // the height of a headline line of `p` px, on whole cells (the lines of the headline must not touch)
  const auto lh_of = [&](int p) {
    a.use_alt_face(true);
    const float v = std::ceil(std::max(float(p), a.line_height(float(p))) / u) * u;
    a.use_alt_face(false);
    return v;
  };
  // The count's band is always reserved (also while it is still being counted) and so is a headline of TWO lines: everything under the headline
  // (rule, count, list) is where it is for every system, so nothing below moves when the cursor changes to a name of one line or of two.
  const float count_h = std::ceil(float(g.F(34, 16)) * 1.15f / u) * u + float(g.U(2));
  const int min_px = std::max(g.F(36, 18), title_px);  // a system name is never smaller than a title
  const int disp_px = std::max(g.F(84, 28), min_px);  // display size
  // the box is two lines of the largest size that still leaves three rows of the list: the headline gives way before the list does
  int box_px = disp_px;
  while (box_px > min_px && ctop + float(g.U(1)) + 2.0f * lh_of(box_px) + float(g.U(5)) + count_h + 3.0f * rh > bot) --box_px;
  const float box_h = 2.0f * lh_of(box_px);
  // the layout of the headline of a name: the size as large as fits in the width and in the box, and its one or two lines
  struct Head {
    std::string l1, l2;
    int px = 0;
    float lh = 0.0f;
  };
  const auto head_of = [&](const std::string& name) {
    Head h;
    h.px = disp_px;
    for (;;) {
      h.l1 = name;
      h.l2.clear();
      if (meas(h.l1, h.px) > avail) {  // two lines, split at the space nearest the middle
        size_t best = std::string::npos;
        for (size_t i = 0; i < name.size(); ++i)
          if (name[i] == ' ' && (best == std::string::npos || std::llabs(long(i) - long(name.size() / 2)) < std::llabs(long(best) - long(name.size() / 2)))) best = i;
        if (best != std::string::npos) {
          h.l1 = name.substr(0, best);
          h.l2 = name.substr(best + 1);
        }
      }
      if ((meas(h.l1, h.px) <= avail && (h.l2.empty() || meas(h.l2, h.px) <= avail) && float(h.l2.empty() ? 1 : 2) * lh_of(h.px) <= box_h) || h.px <= min_px) break;
      --h.px;
    }
    h.lh = lh_of(h.px);
    return h;
  };
  // the lines sit at the foot of the box, on the rule: a name of one line leaves the room above it empty, the second line (if any) goes above
  const float head_top = ctop + float(g.U(1));
  float y = head_top + box_h;
  // The headline rolls inside its box (a roll of 0.6 of its own line height, cut to the box: the square cut suits Swiss).
  draw_pair(ops, items, pair, 0.0f, RollBand{0.0f, head_top, g.w, box_h}, true, [&](const SystemItem& it, int, float s) {
    const Head h = head_of(it.full_name);
    const float ly = y - (h.l2.empty() ? 1.0f : 2.0f) * h.lh + s * 0.6f * h.lh;
    text_alt(a, ops, h.l1, mg, ly + h.lh / 2.0f, h.px, ink, avail);
    if (!h.l2.empty()) text_alt(a, ops, h.l2, mg, ly + h.lh + h.lh / 2.0f, h.px, ink, avail);
  });
  y += float(g.U(2));
  ops.push_back(Op::rect(mg, y, g.w - 2.0f * mg, float(std::max(2, g.P(3))), ink));  // the rule: structure, it neither moves nor fades
  y += float(g.U(3));
  // the count: a big number and its word (its band is there even before the count is known); it rolls a beat after the headline
  const int bp = g.F(34, 16);
  const float bh = std::ceil(float(bp) * 1.15f / u) * u;
  const auto count_parts = [&](const SystemItem& it, std::string& num, std::string& word) {
    const std::string t = games_low(it.games);
    const size_t sp = t.find(' ');
    num = sp == std::string::npos ? t : t.substr(0, sp);
    word = sp == std::string::npos ? std::string() : t.substr(sp + 1);
  };
  draw_pair(ops, items, pair, 0.15f, RollBand{0.0f, y, g.w, bh}, true, [&](const SystemItem& it, int, float s) {
    if (it.games == kCountPending) return;
    std::string num, word;
    count_parts(it, num, word);
    const float cy = y + bh / 2.0f + s * 0.6f * bh;
    text_alt(a, ops, num, mg, cy, bp, ink);
    float wx = g.col(11);  // the word starts on the first column line after the number
    for (int c = 1; c < 12; ++c)
      if (g.col(c) >= mg + a.measure(num, float(bp)) + float(g.U(2))) {
        wx = g.col(c);
        break;
      }
    text(a, ops, word, wx, cy + float(g.P(3)), small_px, dim);
  });
  if (!pair.moving)  // the texts of the neighbours, made now (at rest), so the first frame of a move does not have to rasterise a headline of 84 px
    for (int dd : {-2, -1, 1, 2}) {
      const SystemItem& nb = items[size_t(rem_e(pair.lo + dd, n))];
      const Head h = head_of(nb.full_name);
      a.use_alt_face(true);
      (void)a.text_ellipsis(h.l1, float(h.px), ink, avail);
      if (!h.l2.empty()) (void)a.text_ellipsis(h.l2, float(h.px), ink, avail);
      char b[32];
      std::snprintf(b, sizeof b, "%02d / %02d", rem_e(pair.lo + dd, n) + 1, n);
      (void)a.text(b, float(small_px), ink);
      if (nb.games != kCountPending) {
        std::string num, word;
        count_parts(nb, num, word);
        (void)a.text(num, float(bp), ink);
        a.use_alt_face(false);
        if (!word.empty()) (void)a.text(word, float(small_px), dim);
      }
      a.use_alt_face(false);
    }
  y += bh + float(g.U(2));
  // the list of neighbours: the chosen row is a bar
  const float list_top = down_to_line(g, ctop, y);
  int slots = std::min(5, int(std::floor((bot - list_top) / rh)));
  if (slots % 2 == 0) --slots;  // odd: the chosen row in the middle; never one more row than there is room for
  const int mid_slot = slots / 2;
  if (slots >= 3) {
    const float bx = mg, in_ = float(g.U(1));
    rec(bx, list_top + float(mid_slot) * rh, list_w, rh, "tarjeta", 'B', true, true);
    g_shapes.rrect(a, ops, bx, list_top + float(mid_slot) * rh, list_w, rh, 0, bar);
    g_audit_quiet = true;  // the rows slide: only the bar is measured
    ops.push_back(Op::clip(bx, list_top, list_w, float(slots) * rh));
    for (int i = lo - mid_slot - 1; i <= lo + mid_slot + 2; ++i) {
      const float d = float(i) - cam, ad = std::fabs(d);
      const float ry = list_top + (float(mid_slot) + d) * rh;
      if (ry + rh <= list_top || ry >= list_top + float(slots) * rh) continue;
      // The rows do not get cut in half at the top and the bottom of the window: they fade out as they leave it (the alpha follows how much of the
      // row is already outside, so it is 1 for every slot at rest and 0 when the row has gone). The square clip stays behind, only as a safety.
      const float over = std::max(list_top - ry, ry + rh - (list_top + float(slots) * rh));
      const float edge_a = std::clamp(1.0f - over / rh, 0.0f, 1.0f);
      if (edge_a <= 0.004f) continue;
      const SystemItem& it = items[size_t(rem_e(i, n))];
      // The colour changes continuously as the row crosses the bar: two layers of the text (the ink one and the bar one), each with the alpha
      // it has at this distance (a text picture is baked in its colour, so mixing the colour itself would bake a new picture every frame).
      const float on = std::clamp(1.0f - ad, 0.0f, 1.0f), row_a = 0.62f - 0.12f * std::min(ad, 3.0f);
      char b[8];
      std::snprintf(b, sizeof b, "%02d", rem_e(i, n) + 1);
      const std::string_view nm = it.full_name;
      for (int layer = 0; layer < 2; ++layer) {
        const float w_l = layer == 0 ? 1.0f - on : on;  // layer 0: ink, layer 1: the text on the bar
        if (w_l <= 0.004f) continue;
        const Color lc = layer == 0 ? ink : bar_ink;
        size_t f0 = ops.size();
        text(a, ops, b, mg + in_, ry + rh / 2.0f, foot_px, lc);
        motion::fade(ops, f0, w_l * edge_a * (layer == 0 ? 150.0f / 255.0f : 1.0f));
        f0 = ops.size();
        text_alt(a, ops, nm, mg + in_ + idx_w, ry + rh / 2.0f, row_px, lc, list_w - in_ - idx_w);
        motion::fade(ops, f0, w_l * edge_a * (layer == 0 ? row_a : 1.0f));
      }
    }
    ops.push_back(Op::unclip());
    g_audit_quiet = false;
  }
  status_pill(th, a, g, status, Color{255, 255, 255, 240}, ops);
  HelpLook hl;
  hl.fg = dk ? Color{255, 255, 255, 235} : rgb(0x12163A);
  hl.accent = dk ? kWhite : rgb(0x2540FF);
  hl.cy = float(g.help_y());
  hl.right = g.w - float(g.margin());
  help(th, a, g, system_help(), hl, ops);
}

// ---- Industrial: the panel of a device -------------------------------------------------------------------------------------------------------
// A dark screen with the illustration of the system in one colour (a grey picture tinted orange, with scan lines), a dial with the position, and
// a row of keys under them: the chosen one is orange and lit. Light: warm grey with cream keys; dark: graphite. The orange is the same in both.

void industrial_system(const Theme& th, Assets& a, const Grid& g, const std::vector<SystemItem>& items, int cursor, const SystemItem& cur, float cam, const Status& status, const InfoFade& fade, std::vector<Op>& ops) {
  (void)cursor;  // the dial's number follows the camera
  (void)cur;     // so do the plate, the name bar and the counts: none of them is drawn from the cursor or from the caller's fade
  (void)fade;
  const IconicInk sc = iconic_ink(th);
  const bool dk = sc.dark;
  const Color page = dk ? rgb(0x1D1C1A) : rgb(0xE8E6DF), panel = dk ? rgb(0x2B2A27) : rgb(0xF5F4EE), lip = dk ? rgb(0x0C0B0A) : rgb(0xCFCDC4), ink = dk ? rgb(0xF1EFE6) : rgb(0x1B1A17);
  const Color dim = dk ? rgb(0xA3A196) : rgb(0x6E6C63), bez = dk ? rgb(0x0B0B0C) : rgb(0x17161A), orange = rgb(0xFF5A1F), led_off = dk ? rgb(0x4A4842) : rgb(0xC4C2B8);
  const Color ring_c = dk ? rgb(0x35342F) : rgb(0xE1DFD6);
  const int n = int(items.size());
  const float mg = float(g.margin()), ctop = float(g.content_top()), u = float(g.u), bot = float(g.help_top()) - float(g.U(1));
  const int lo = int(std::floor(cam));
  const float frac = cam - float(lo);
  const CamPair pair(cam);  // the same camera as a pair of systems, for the texts that roll
  const float lipd = cells_of(g, 6, 1);  // a whole number of cells: the rows under the screen stay on the lines
  const int rad = std::max(4, g.P(24));
  ImgId id;
  ops.push_back(Op::rect(0, 0, g.w, g.h, page));
  const float gap = g.gutter();
  using ui::Level;
  const int title_px = g.L(Level::Title), small_px = g.L(Level::Small), foot_px = g.L(Level::Foot);
  // a key holds its lip, the index line, and room for the logo under it
  const float tile_h = std::max(cells_of(g, 92, 4), float(g.cells_over(lipd + u + 1.25f * float(foot_px) + 3.0f * u) * g.u)), led_h = float(g.U(2));
  const float strip_h = led_h + tile_h + lipd;
  const float top = ctop;
  const float sh = std::max(u * 8.0f, std::floor((bot - top - strip_h - gap - lipd) / u) * u);
  const float sx = g.col(0), sw = g.span(8), dx = g.col(8), dw = g.span(4);
  // the header, in the row of the status bar
  {
    const float cy = float(g.status_y()), r = float(std::max(4, g.P(6)));
    g_shapes.rrect(a, ops, mg, cy - r, 2.0f * r, 2.0f * r, int(r), orange);
    text(a, ops, "ZENITH  \xC2\xB7  SYS-SELECT", g.col(1), cy, small_px, ink, g.w * 0.5f);
  }
  // the screen
  rec(sx, top, sw, sh, "tarjeta", 'B', true, true);
  g_shapes.rrect(a, ops, sx, top + lipd, sw, sh, rad, lip);
  g_shapes.rrect(a, ops, sx, top, sw, sh, rad, bez);
  const float pad = float(g.U(1)), px0 = sx + pad, py0 = top + pad, pw_ = sw - 2.0f * pad, ph_ = sh - 2.0f * pad;
  const int prad = std::max(3, rad - g.P(10));
  const std::string flags = "gcr" + std::to_string(prad);
  g_audit_quiet = true;
  for (int k = 0; k < 2; ++k) {
    if (k == 1 && frac < 0.004f) break;
    const SystemItem& it = items[size_t(rem_e(lo + k, n))];
    const std::string art = sys_file(th, "_inc/backgrounds", it.theme_name, ".webp");
    const std::string art_any = art.empty() ? es_file(th, "_inc/backgrounds/_default.webp") : art;
    const int al = int(255.0f * (k == 0 ? 1.0f : frac));
    if (exists_once(art_any) && pic(a, baked(art_any, flags.c_str()), pw_, ph_, true, id)) put_fit(a, ops, id, px0, py0, pw_, ph_, Color{255, 110, 50, uint8_t(al)});
    else if (k == 0) g_shapes.rrect(a, ops, px0, py0, pw_, ph_, prad, alpha(orange, 90));
  }
  if (at_rest(cam))
    for (int dd : {-1, 1}) {
      const std::string art = sys_file(th, "_inc/backgrounds", items[size_t(rem_e(int(std::round(cam)) + dd, n))].theme_name, ".webp");
      ImgId nid;
      if (!art.empty()) pic(a, baked(art, flags.c_str()), pw_, ph_, true, nid);
    }
  scanlines(a, ops, px0, py0, pw_, ph_, g.h);
  // the logo on its plate, top left of the screen, and the bar with the name at its foot
  const float bar_h = std::max(cells_of(g, 56, 3), float(g.cells_over(1.25f * float(title_px) + 2.0f * u) * g.u));  // the name bar holds a Title line
  g_shapes.rrect(a, ops, px0, py0 + ph_ - bar_h, pw_, bar_h, std::max(3, prad / 2), bez);
  // Industrial is a device: what changes with the camera is MECHANICAL, a quick sweep cut to its own window with no fading (like the digits of an
  // odometer or the label of a panel): the old one goes up and out while the new one comes up from below, a whole window of travel; going back
  // it is the other way round. The plate of the logo and the name bar sweep together.
  const float plw = std::min(g.span(3), pw_ * 0.34f), plh = std::max(u * 3.0f, std::floor(plw * 0.4f / u) * u);
  draw_pair(ops, items, pair, 0.0f, RollBand{px0 + float(g.U(1)), py0 + float(g.U(1)), plw, plh}, false, [&](const SystemItem& it, int, float s) {
    const std::string lp = sys_file(th, "_inc/logos", it.theme_name, ".webp");
    const float dy = s * plh;
    g_shapes.rrect(a, ops, px0 + float(g.U(1)), py0 + float(g.U(1)) + dy, plw, plh, std::max(3, g.P(8)), kWhite);
    if (!lp.empty() && pic(a, lp, plw - float(g.U(2)), plh - float(g.U(1)), true, id)) put_fit(a, ops, id, px0 + float(g.U(1)) + float(g.U(1)) / 2.0f, py0 + float(g.U(1)) + float(g.U(1)) / 4.0f + dy, plw - float(g.U(1)), plh - float(g.U(1)) / 2.0f);
    else if (lp.empty())  // Iconic has no logo for this system (Music Player, Media Player, books...): its name, not an empty plate
      name_lines(a, ops, it.full_name, px0 + float(g.U(1)) + plw / 2.0f, py0 + float(g.U(1)) + dy + plh / 2.0f, small_px, rgb(0x1A1A1A), plw - float(g.U(2)));
  });
  g_audit_quiet = false;
  // the name of the system and its count, on the bar at the foot of the screen: the name's size depends on the room the count leaves
  const float bar_cy = py0 + ph_ - bar_h / 2.0f;
  const int fs_min = std::max(g.F(15, 10), title_px);  // a system name is never smaller than a title
  struct Bar {
    std::string t;
    float tw = 0.0f, max_w = 0.0f;
    int fs = 0;
  };
  const auto bar_of = [&](const SystemItem& it) {
    Bar b;
    b.t = it.games != kCountPending ? games_of(it.games) : std::string();
    b.tw = b.t.empty() ? 0.0f : a.measure(b.t, float(small_px)) + float(g.U(2));
    b.max_w = pw_ - b.tw - float(g.U(4));
    b.fs = std::max(g.F(22, 13), fs_min);  // display size
    while (b.fs > fs_min && a.measure(it.full_name, float(b.fs)) > b.max_w) --b.fs;
    return b;
  };
  draw_pair(ops, items, pair, 0.0f, RollBand{px0, py0 + ph_ - bar_h, pw_, bar_h}, false, [&](const SystemItem& it, int, float s) {
    const Bar b = bar_of(it);
    const float cy = bar_cy + s * bar_h;
    const bool quiet = g_audit_quiet;
    g_audit_quiet = true;  // inside the screen: the screen is what is measured
    text_alt(a, ops, it.full_name, px0 + float(g.U(2)), cy, b.fs, rgb(0xF4F2EA), b.max_w);
    if (!b.t.empty()) text(a, ops, b.t, px0 + pw_ - float(g.U(2)), cy, small_px, orange, 0, R);
    g_audit_quiet = quiet;
  });
  if (!pair.moving)  // the plates and names of the neighbours, ready before the move (at rest): a title is not rasterised in the first frame of a turn
    for (int dd : {-2, -1, 1, 2}) {
      const SystemItem& nb = items[size_t(rem_e(pair.lo + dd, n))];
      ImgId nid;
      const std::string lp = sys_file(th, "_inc/logos", nb.theme_name, ".webp");
      if (!lp.empty()) pic(a, lp, plw - float(g.U(2)), plh - float(g.U(1)), true, nid);
      const Bar b = bar_of(nb);
      a.use_alt_face(true);
      (void)a.text_ellipsis(nb.full_name, float(b.fs), rgb(0xF4F2EA), b.max_w);
      a.use_alt_face(false);
      if (!b.t.empty()) (void)a.text(b.t, float(small_px), orange);
    }
  // the dial: a ring, twelve marks and the position in the middle
  g_shapes.rrect(a, ops, dx, top + lipd, dw, sh, rad, lip);
  g_shapes.rrect(a, ops, dx, top, dw, sh, rad, panel);
  {
    const float dd = std::max(u * 4.0f, std::min(dw - float(g.U(3)), sh * 0.56f)), cx = dx + dw / 2.0f, cy = top + float(g.U(2)) + dd / 2.0f;
    const float bw2 = float(std::max(2, g.P(2)));
    g_shapes.rrect(a, ops, cx - dd / 2.0f, cy - dd / 2.0f, dd, dd, int(dd / 2.0f), ink);
    g_shapes.rrect(a, ops, cx - dd / 2.0f + bw2, cy - dd / 2.0f + bw2, dd - 2.0f * bw2, dd - 2.0f * bw2, int(dd / 2.0f), ring_c);
    const float d2 = dd * 0.78f;
    g_shapes.rrect(a, ops, cx - d2 / 2.0f, cy - d2 / 2.0f, d2, d2, int(d2 / 2.0f), ink);
    g_shapes.rrect(a, ops, cx - d2 / 2.0f + bw2, cy - d2 / 2.0f + bw2, d2 - 2.0f * bw2, d2 - 2.0f * bw2, int(d2 / 2.0f), panel);
    // The lit mark follows the camera: it slides from the mark of the system the camera leaves to the one it reaches (each is where the
    // system sits on the dial, as before), so at rest the same single mark is lit.
    const auto mark_of = [&](int i) { return int(std::lround(float(rem_e(i, n)) / float(n) * 12.0f)); };
    const int q_lo = mark_of(lo);
    int q_hi = mark_of(lo + 1);
    if (q_hi < q_lo) q_hi += 12;  // the carousel wraps round the dial
    const float pos = std::fmod(lerp(float(q_lo), float(q_hi), frac), 12.0f);
    for (int t = 0; t < 12; ++t) {
      const float ang = float(t) * 3.14159265f / 6.0f, r = dd * 0.455f;
      const float dt = std::fabs(float(t) - pos), lit = std::clamp(1.0f - std::min(dt, 12.0f - dt), 0.0f, 1.0f);
      const float m = lerp(float(std::max(3, g.P(4))), float(std::max(5, g.P(8))), lit);
      g_shapes.rrect(a, ops, cx + std::sin(ang) * r - m / 2.0f, cy - std::cos(ang) * r - m / 2.0f, m, m, int(m / 2.0f), motion::mix(ink, orange, lit));
    }
    char b[16];
    // The number is an ODOMETER, driven by the camera like the lit mark of the ring: the number of the system the camera leaves goes up and out
    // of its slot while the next one comes up from below, a line of travel, cut to the slot (the total, "/ NN", never changes). At rest it is
    // the number of the system under the camera. The count of games below follows it with the same roll.
    a.use_alt_face(true);
    const float nh = a.line_height(float(g.F(30, 14)));
    a.use_alt_face(false);
    const float sh2 = a.line_height(float(foot_px));
    const float y0 = cy - (nh + sh2) / 2.0f;  // the number and its total, one over the other, centred in the dial
    draw_pair(ops, items, pair, 0.0f, RollBand{cx - d2 / 2.0f + bw2, y0, d2 - 2.0f * bw2, nh}, false, [&](const SystemItem&, int idx, float s) {
      char nb[16];
      std::snprintf(nb, sizeof nb, "%02d", idx + 1);
      text_alt(a, ops, nb, cx, y0 + nh / 2.0f + s * nh, g.F(30, 14), ink, 0, C);
    });
    std::snprintf(b, sizeof b, "/ %02d", n);
    text(a, ops, b, cx, y0 + nh + sh2 / 2.0f, foot_px, dim, 0, C);
    // the games count sits two cells over the foot of the panel, and only if it clears the ring by a cell
    const float lab_cy = top + sh - float(g.U(2)) - 0.65f * float(small_px);
    if (cy + dd / 2.0f + u + 0.65f * float(small_px) <= lab_cy) {
      const float lbh = a.line_height(float(small_px));
      draw_pair(ops, items, pair, 0.0f, RollBand{dx, lab_cy - lbh / 2.0f, dw, lbh}, false, [&](const SystemItem& it, int, float s) {
        if (it.games != kCountPending) text(a, ops, games_of(it.games), cx, lab_cy + s * lbh, small_px, dim, dw - float(g.U(2)), C);
      });
    }
    if (!pair.moving)  // the numbers and counts of the neighbours, made now (at rest) for the first frame of a turn
      for (int dd2 : {-2, -1, 1, 2}) {
        const SystemItem& nb = items[size_t(rem_e(pair.lo + dd2, n))];
        std::snprintf(b, sizeof b, "%02d", rem_e(pair.lo + dd2, n) + 1);
        a.use_alt_face(true);
        (void)a.text(b, float(g.F(30, 14)), ink);
        a.use_alt_face(false);
        if (nb.games != kCountPending) (void)a.text_ellipsis(games_of(nb.games), float(small_px), dim, dw - float(g.U(2)));
      }
  }
  // the keys: five slide under the lit slot, the sixth only counts the rest
  const float pitch = g.col(2) - g.col(0), tw_ = g.span(2), ty = top + sh + gap + lipd;
  const float tl = ty + led_h;
  rec(g.col(4), tl, tw_, tile_h, "tarjeta", 'B', true, true);
  g_audit_quiet = true;
  ops.push_back(Op::clip(g.col(0), ty - float(g.U(1)), 5.0f * pitch - gap, strip_h + float(g.U(1))));
  for (int i = lo - 3; i <= lo + 4; ++i) {
    const float d = float(i) - cam, ad = std::fabs(d), x = g.col(0) + (2.0f + d) * pitch;
    if (x + tw_ <= g.col(0) || x >= g.col(0) + 5.0f * pitch) continue;
    const float on = std::clamp(1.0f - ad, 0.0f, 1.0f);
    const SystemItem& it = items[size_t(rem_e(i, n))];
    const float r = float(g.U(1)) / 2.0f;
    // Every part of a key goes from "resting" to "lit" with `on`, which follows the camera: nothing switches at a threshold.
    if (on > 0.008f) disc(a, ops, x + tw_ / 2.0f, ty + r, r * 4.0f, alpha(orange, int(160.0f * on)));  // (skipped only while it has no alpha)
    g_shapes.rrect(a, ops, x + tw_ / 2.0f - r, ty, 2.0f * r, 2.0f * r, int(r), motion::mix(led_off, orange, on));
    // The key that is being lit is PRESSED: its face sinks a pixel or two into its lip and comes back (sin(pi * on): 0 when the key rests, lit or
    // not, and the most in the middle of the move). The lip stays; the index and the logo travel with the face.
    const float press = (on > 0.0005f && on < 0.9995f ? std::sin(3.14159265f * on) : 0.0f) * std::min(float(std::max(1, g.P(2))), lipd * 0.5f);
    const float ky = tl + press;
    g_shapes.rrect(a, ops, x, tl + lipd, tw_, tile_h - lipd, int(g.P(12)), motion::mix(lip, rgb(0xB83A0D), on));
    g_shapes.rrect(a, ops, x, ky, tw_, tile_h - lipd, int(g.P(12)), motion::mix(panel, orange, on));
    char b[8];
    std::snprintf(b, sizeof b, "%02d", rem_e(i, n) + 1);
    // the index is two layers of the text, the resting colour and white, each with its alpha (a text picture is baked in its colour)
    for (int layer = 0; layer < 2; ++layer) {
      const float w_l = layer == 0 ? 1.0f - on : on;
      if (w_l <= 0.004f) continue;
      const size_t f0 = ops.size();
      text(a, ops, b, x + float(g.U(1)), ky + float(g.U(1)) + 0.6f * float(foot_px), foot_px, layer == 0 ? dim : kWhite);
      motion::fade(ops, f0, w_l);
    }
    const std::string lp = sys_file(th, "_inc/logos", it.theme_name, ".webp");
    const float lw = tw_ - float(g.U(3)), lh = (tile_h - lipd) * 0.5f;
    if (!lp.empty() && pic(a, lp, lw, lh, true, id)) {
      // the white plate of the logo: lit keys have it (in the dark theme all of them do), and it comes in with `on`
      const int plate_a = dk ? 255 : int(std::lround(255.0f * on));
      if (plate_a > 0) g_shapes.rrect(a, ops, x + float(g.U(1)), ky + (tile_h - lipd - lh - float(g.U(1))) / 2.0f + float(g.U(1)) / 2.0f, tw_ - float(g.U(2)), lh + float(g.U(1)), std::max(3, g.P(6)), alpha(kWhite, plate_a));
      put_fit(a, ops, id, x + (tw_ - lw) / 2.0f, ky + (tile_h - lipd - lh) / 2.0f + float(g.U(1)) / 2.0f, lw, lh);
    } else if (lp.empty()) {  // no logo in Iconic for this system: its name, in the two layers of the index (resting colour, white when lit)
      for (int layer = 0; layer < 2; ++layer) {
        const float w_l = layer == 0 ? 1.0f - on : on;
        if (w_l <= 0.004f) continue;
        const size_t f0 = ops.size();
        name_lines(a, ops, it.full_name, x + tw_ / 2.0f, ky + (tile_h - lipd) / 2.0f + float(g.U(1)) / 2.0f, foot_px, layer == 0 ? ink : kWhite, tw_ - float(g.U(2)));
        motion::fade(ops, f0, w_l);
      }
    }
  }
  ops.push_back(Op::unclip());
  g_audit_quiet = false;
  if (n > 5) {
    const float x = g.col(10), ky = tl;
    g_shapes.rrect(a, ops, x, ky, tw_, tile_h - lipd, g.P(12), dk ? rgb(0x262522) : rgb(0xDCDAD1));
    char b[12];
    std::snprintf(b, sizeof b, "+%d", n - 5);
    text_alt(a, ops, b, x + tw_ / 2.0f, ky + (tile_h - lipd) / 2.0f, small_px, dim, tw_, C);
  }
  if (at_rest(cam))
    for (int dd : {-4, -3, 4, 5}) {
      const std::string lp = sys_file(th, "_inc/logos", items[size_t(rem_e(int(std::round(cam)) + dd, n))].theme_name, ".webp");
      ImgId nid;
      if (!lp.empty()) pic(a, lp, tw_ - float(g.U(3)), (tile_h - lipd) * 0.5f, true, nid);
    }
  status_pill(th, a, g, status, Color{255, 255, 255, 240}, ops);
  HelpLook hl;
  hl.fg = ink;
  hl.accent = orange;
  hl.cy = float(g.help_y());
  hl.right = g.w - float(g.margin());
  help(th, a, g, system_help(), hl, ops);
}

void iconic_system(const Theme& th, Assets& a, const Grid& g, const std::vector<SystemItem>& items, int cursor, const SystemItem& cur, float cam, const Status& status, const InfoFade& fade, std::vector<Op>& ops) {
  if (th.str("esCarousel", "") == "aurora") return aurora_system(th, a, g, items, cursor, cur, cam, status, fade, ops);
  if (th.str("esCarousel", "") == "swiss") return swiss_system(th, a, g, items, cursor, cur, cam, status, fade, ops);
  if (th.str("esCarousel", "") == "industrial") return industrial_system(th, a, g, items, cursor, cur, cam, status, fade, ops);
  const IconicInk sc = iconic_ink(th);
  const Color acc = iconic_accent(th, a, cur.theme_name), soft_c = alpha(acc, sc.dark ? 56 : 30);  // the chips of the dark page need more body to show
  iconic_page(th, a, g, acc, ops);
  const int n = int(items.size());
  const float mg = float(g.margin()), ctop = float(g.content_top()), u = float(g.u);
  const float dock_h = cells_of(g, 62, 4), dock_y = float(g.help_top()) - dock_h;
  const size_t fade_from = ops.size();
  // the illustration fills the page, as in the theme's own view; the panel and the logos lie on it
  const float ih = dock_y - g.gutter() - ctop, panel_w = mg + g.span(4) + mg;
  const std::string art = sys_file(th, "_inc/backgrounds", cur.theme_name, ".webp");
  const std::string art_any = art.empty() ? es_file(th, "_inc/backgrounds/_default.webp") : art;
  ImgId id;
  bool art_on = false;
  if (exists_once(art_any)) art_on = pic(a, baked(art_any, "c"), g.w, g.h, true, id);
  if (art_on) ops.push_back(Op::image(id, 0, 0, float(a.img(id).w), float(a.img(id).h)));
  else ops.push_back(Op::rect(0, 0, g.w, g.h, soft_c));  // while it loads: the colour of the system, not an empty page
  if (at_rest(cam) && exists_once(art_any)) {  // on the job thread, one by one: the next illustrations are ready before the move
    for (int dd : {1, -1, 2, -2}) {
      const std::string np = sys_file(th, "_inc/backgrounds", items[size_t(rem_e(int(std::round(cam)) + dd, int(items.size())))].theme_name, ".webp");
      ImgId nid;
      if (!np.empty()) pic(a, baked(np, "c"), g.w, g.h, true, nid);
    }
  }
  ops.push_back(Op::rect(0, 0, panel_w, g.h, alpha(sc.card, 222)));  // the panel of the system, over the whole height
  ops.push_back(Op::rect(panel_w, 0, 1, g.h, alpha(sc.dim, 40)));
  // the panel: controller, name, count
  const float cxm = mg + g.span(4) / 2.0f;
  const float chh = std::max(u, std::floor(std::min(float(g.P(124)), std::floor(ih * 0.47f)) / u) * u);
  const float top = down_to_line(g, ctop, ctop + std::max(0.0f, (ih - chh - float(g.U(10))) / 2.0f));
  std::string ctrl = sys_file(th, "_inc/controllers", cur.theme_name, ".webp");
  if (ctrl.empty()) ctrl = es_file(th, "_inc/controllers/_default.webp");
  if (exists_once(ctrl) && pic(a, ctrl, g.span(4), chh, true, id)) {
    float ow, oh;
    fit_size(a.img(id), g.span(4), chh, ow, oh);
    soft(a, ops, cxm - ow / 2.0f, top + (chh - oh) / 2.0f + float(g.P(6)), ow, oh, float(g.P(10)), Color{20, 22, 40, 60});
    ops.push_back(Op::image(id, std::round(cxm - ow / 2.0f), std::round(top + (chh - oh) / 2.0f), ow, oh));
  }
  const float ty = top + chh + float(g.U(2));
  float pill_y;
  {
    const float maxw = g.span(4);
    int px = g.F(25, 16);
    const int min_px = g.F(19, 13);
    while (px > min_px && a.measure(cur.full_name, float(px)) > maxw) --px;
    if (a.measure(cur.full_name, float(px)) <= maxw) {
      text(a, ops, cur.full_name, cxm, ty + float(g.U(1)), px, sc.ink, maxw, C);
      pill_y = ty + float(g.U(4));
    } else {  // two lines, cut at the space nearest the middle
      const std::string& s = cur.full_name;
      size_t cut = s.size() / 2, best = std::string::npos;
      for (size_t k = 0; k < s.size(); ++k)
        if (s[k] == ' ' && (best == std::string::npos || (k > cut ? k - cut : cut - k) < (best > cut ? best - cut : cut - best))) best = k;
      if (best == std::string::npos) best = cut;
      text(a, ops, std::string_view(s).substr(0, best), cxm, ty, px, sc.ink, maxw, C);
      text(a, ops, trim(std::string_view(s).substr(best)), cxm, ty + float(g.F(24, 17)), px, sc.ink, maxw, C);
      pill_y = ty + float(g.snap(float(g.F(24, 17) + g.P(32)) - 1.5f * u, 1));
    }
  }
  if (cur.games != kCountPending) {
    const std::string t = games_low(cur.games);
    const int f = g.F(14);
    const float ph = cells_of(g, 26), tw = pill_w(g, a.measure(t, float(f)), float(g.u), true);
    rec(cxm - tw / 2.0f, pill_y, tw, ph, "tarjeta", 'B', false, true);
    g_shapes.pill(a, ops, cxm - tw / 2.0f, pill_y, tw, ph, soft_c);
    text(a, ops, t, cxm, pill_y + ph / 2.0f, f, iconic_text(sc, acc), 0, C);
  }
  motion::fade(ops, fade_from, fade.alpha);
  // the dock
  const float dock_w = g.w - 2 * mg, inset = float(g.U(1));
  for (int pass = 0; pass < 2; ++pass) bottom_fade(a, ops, g.w, g.h, g.h - dock_y + float(g.U(12)), alpha(sc.card, 236));  // the logos stand on the page, no card: two passes, or a grey logo is lost on the art
  const float selw = float(g.snap(std::floor(g.w * 0.3125f), 4)), sx = mg + inset;
  const float rest_x = sx + selw + inset, rest = g.w - mg - inset - rest_x;
  const int cells = rest > float(g.P(300)) ? 3 : 2;
  const float cell = rest / float(cells), pad_sel = float(g.U(4)), pad_cell = inset, pad_v = float(g.U(4));
  ops.push_back(Op::clip(mg, dock_y, dock_w, dock_h));
  rec(mg, dock_y, dock_w, dock_h, "dock", 'B', true, true);
  g_audit_quiet = true;  // what slides inside is cut to the dock
  const int mid = int(std::floor(cam));
  for (int i = mid - 1; i <= mid + cells + 2; ++i) {
    const float k = float(i) - cam;
    const SystemItem& it = items[size_t(rem_e(i, n))];
    float cx, bw, bh;
    int al;
    const float sel_c = sx + selw / 2.0f, rc0 = rest_x + cell / 2.0f;
    if (k <= 0) {
      cx = sel_c + k * selw;
      bw = selw - pad_sel;
      bh = dock_h - pad_v;
      al = 255;
    } else if (k < 1) {
      cx = lerp(sel_c, rc0, k);
      bw = lerp(selw - pad_sel, cell - pad_cell, k);
      bh = lerp(dock_h - pad_v, dock_h - pad_v - u, k);
      al = int(lerp(255, 150, k));
    } else {
      cx = rc0 + (k - 1) * cell;
      bw = cell - pad_cell;
      bh = dock_h - pad_v - u;
      al = 150;
    }
    if (cx + bw / 2.0f < mg || cx - bw / 2.0f > g.w - mg) continue;
    std::string p = sys_file(th, "_inc/logos", it.theme_name, ".webp");
    if (!p.empty() && pic(a, p, selw - pad_sel, dock_h - pad_v, true, id)) {
      put_fit(a, ops, id, cx - bw / 2.0f, dock_y + (dock_h - bh) / 2.0f, bw, bh, Color{255, 255, 255, uint8_t(al)});
    } else if (p.empty()) {
      text(a, ops, it.full_name, cx, dock_y + dock_h / 2.0f, g.F(13, 11), alpha(sc.body, al), bw, C);
    }
  }
  g_audit_quiet = false;
  ops.push_back(Op::unclip());
  (void)cursor;
  status_pill(th, a, g, status, Color{255, 255, 255, 240}, ops);
  // the bar sits inside the card, not on the page
  HelpLook hl;
  hl.fg = sc.dim;
  hl.accent = iconic_text(sc, acc);
  hl.cy = float(g.help_y());
  hl.right = g.w - float(g.margin());
  help(th, a, g, system_help(), hl, ops);
}

void iconic_list(const Theme& th, Assets& a, const Grid& g, const std::vector<std::string>& names, size_t cursor, const Status& status, const DetailedData* d, std::vector<Op>& ops) {
  const IconicInk sc = iconic_ink(th);
  const Color acc = iconic_accent(th, a, g_state.system_theme), soft_c = alpha(acc, sc.dark ? 56 : 30);  // the chips of the dark page need more body to show
  iconic_page(th, a, g, acc, ops);
  const int m = g.margin();
  const float x = float(m), w = g.span(5);
  const std::string logo = sys_file(th, "_inc/logos", g_state.system_theme, ".webp");
  if (d) {
    const bool has = sc.detail && has_data(*d);
    using ui::Level;
    const int title = g.L(Level::Title), row_px = g.L(Level::Row), small_px = g.L(Level::Small);
    // under the cover, on whole cells: the title line, the chips (Small at 1.6 lines), the stars and two lines of description
    const int title_h = g.cells_over(1.25f * float(title)) * g.u, chip_h = std::max(int(cells_of(g, 24)), g.cells_over(1.6f * float(small_px)) * g.u);
    const int star_h = d->show_rating ? g.cells_over(float(g.P(14))) * g.u : 0, lh = int(std::lround(1.3f * float(row_px)));
    const float reserve = float(2 * g.u + title_h + (has ? 3 * g.u + chip_h + star_h + 2 * lh : (star_h ? g.u + star_h : 0)));
    float top = float(g.content_top()), cw, ch;
    box43c(g, w, float(g.help_top()) - top - reserve, cw, ch);
    if (!has) top = down_to_line(g, float(g.content_top()), (float(g.help_top() + g.content_top()) - ch - reserve) / 2.0f);
    cover_box(a, ops, d->image, x, top, cw, ch, g.P(14), logo, sc.card, Color{0, 0, 0, 0}, Color{20, 22, 40, 70}, sc.video ? d->video : std::string());
    if (!sc.video) video::box_reset();  // PLAY VIDEO PREVIEWS: no (the list never offers its picture's box to the preview)
    const float y0 = top + ch + float(g.U(2)), yt = y0 + float(title_h) / 2.0f;  // the title's line, centred in its cells
    std::string cased;
    text(a, ops, list_cased(th, d->name, cased), x, yt, title, sc.ink, w);
    if (has) {
      const float yc = y0 + float(title_h) + float(g.u), ys = yc + float(chip_h) + float(g.u), yd = ys + float(star_h) + float(g.u);
      chips(a, ops, *d, g, x, yc, float(chip_h), x + w, small_px, cells_of(g, 10), soft_c, iconic_text(sc, acc));
      if (star_h) stars(th, a, ops, x, ys, g.P(14), d->rating, iconic_text(sc, acc), sc.star, g.P(3));
      desc_block(a, ops, *d, x, star_h ? yd : ys, w, float(g.help_top()) - (star_h ? yd : ys), row_px, sc.body);
    } else if (star_h) {
      stars(th, a, ops, x, y0 + float(title_h) + float(g.u), g.P(14), d->rating, iconic_text(sc, acc), sc.star, g.P(3));
    }
  }
  // the card of the list, on the columns from the sixth to the margin; the rows inside it, a cell in
  const float lx = g.col(6), ly = float(g.content_top()), lw = g.w - float(m) - lx, lh2 = float(g.help_top()) - ly, inset = float(g.U(1));
  soft(a, ops, lx, ly + float(g.P(3)), lw, lh2, float(g.P(10)), Color{20, 22, 40, 40});
  g_shapes.rrect(a, ops, lx, ly, lw, lh2, g.P(18), sc.card);
  rec(lx, ly, lw, lh2, "tarjeta", 'B', true, true);
  ImgId lg;
  if (!logo.empty() && pic(a, logo, float(g.P(150)), float(g.P(28)), true, lg)) put_fit(a, ops, lg, float(m), float(g.top()), cells_of(g, 150), float(g.bar_h()), kWhite, 0.0f);
  const int fs = g.L(ui::Level::Row);  // the game's name; a row holds it at 1.6 lines, on whole cells
  const int pitch = std::max(g.snap(float(g.P(36)), 3), g.cells_over(1.6f * float(fs)) * g.u);
  const size_t rows = size_t(std::max(1, int((lh2 - 2 * inset) / float(pitch))));
  const RowWindow rw = row_window(names.size(), cursor, rows);
  if (rw.moving) ops.push_back(Op::clip(lx, ly, lw, lh2));
  if (!names.empty()) {  // the selected row: a pill of whole cells inside the card
    const float ys = ly + inset + rw.sel * float(pitch);
    const Color pill_c = holds_white(acc);  // the name of the row is white
    soft(a, ops, lx + inset, ys + float(g.P(3)), lw - 2 * inset, float(pitch), float(g.P(6)), alpha(pill_c, 70));
    g_shapes.rrect(a, ops, lx + inset, ys, lw - 2 * inset, float(pitch), g.P(12), pill_c);
    rec(lx + inset, ys, lw - 2 * inset, float(pitch), "tarjeta", 'B', false, true);
  }
  const Free in_card;
  std::string cased_row;
  for (size_t k = rw.first; k < rw.end; ++k) {
    const float y = ly + inset + (float(k) - rw.start) * float(pitch) + float(pitch) / 2.0f;
    text(a, ops, list_cased(th, names[k], cased_row), lx + float(g.U(3)), y, fs, k == rw.lit ? kWhite : sc.body, lw - float(g.U(6)));
  }
  if (rw.moving) ops.push_back(Op::unclip());
  status_bar(th, a, g.w, g.h, status, ops);
  HelpLook hl;
  hl.fg = legible_on(sc.dim, th.color("bg"));
  hl.accent = iconic_text(sc, acc);
  hl.cy = float(g.help_y());
  hl.right = g.w - float(g.margin());
  help(th, a, g, list_help(), hl, ops);
}

// ---- TechDweeb -------------------------------------------------------------------------------------------------------------------------------

const Color kOrange = rgb(0xFF7A1A), kInkT = rgb(0x08080A), kPanelT = rgb(0x121214);

void pixel_frame(std::vector<Op>& ops, float x, float y, float w, float h, Color c, float t) {
  ops.push_back(Op::rect(x + t, y, w - 2 * t, t, c));
  ops.push_back(Op::rect(x + t, y + h - t, w - 2 * t, t, c));
  ops.push_back(Op::rect(x, y + t, t, h - 2 * t, c));
  ops.push_back(Op::rect(x + w - t, y + t, t, h - 2 * t, c));
}

int pixel_px(const Grid& g) { return g.s < 1.45f ? 16 : g.s < 2.2f ? 24 : 32; }  // the pixel face is sharp only at multiples of 8

void tech_scene(const Theme& th, Assets& a, const Grid& g, int al, std::vector<Op>& ops) {
  ImgId id;
  static struct {
    std::string dir, baked;
    unsigned gen = ~0u;
  } c;  // the path is made when the theme folder or the games change, not on every frame
  const std::string_view dir = th.str("esDir", "");
  if (c.gen != games_generation() || c.dir != dir) {
    c.gen = games_generation();
    c.dir.assign(dir);
    const std::string p = es_file(th, "_inc/images/system-view/orange.png");
    c.baked = exists_once(p) ? baked(p, "c") : std::string();
  }
  if (!c.baked.empty() && pic(a, c.baked, g.w, g.h, true, id)) ops.push_back(Op::image(id, 0, 0, g.w, g.h, Color{255, 255, 255, uint8_t(al)}));
}

void tech_system(const Theme& th, Assets& a, const Grid& g, const std::vector<SystemItem>& items, const SystemItem& cur, float cam, const Status& status, const InfoFade& fade, std::vector<Op>& ops) {
  ops.push_back(Op::rect(0, 0, g.w, g.h, kInkT));
  tech_scene(th, a, g, 255, ops);
  const int n = int(items.size());
  const float bw = std::round(0.7f * g.w), bh = std::round(0.6f * g.h);
  const float cy = g.h / 2.0f - float(g.P(12));
  const int mid = int(std::floor(cam));
  for (int i = mid; i <= mid + 1; ++i) {  // the theme's carousel: one logo at a time, it slides across the screen
    const float d = float(i) - cam;
    if (std::fabs(d) >= 1.0f && i != mid) continue;
    const SystemItem& it = items[size_t(rem_e(i, n))];
    const float cx = g.w / 2.0f + d * g.w;
    std::string p = sys_file(th, "_inc/systems/logos", it.theme_name, ".png");
    ImgId id;
    if (!p.empty() && pic(a, p, bw, bh, true, id)) {
      float ow, oh;
      fit_size(a.img(id), bw, bh, ow, oh);
      soft(a, ops, cx - ow / 2.0f, cy - oh / 2.0f, ow, oh, float(g.P(30)), Color{0, 0, 0, 215});  // the scene steps back a little behind the logo
      ops.push_back(Op::image(id, std::round(cx - ow / 2.0f), std::round(cy - oh / 2.0f), ow, oh));
    } else if (p.empty() || covers::failed(p)) {
      soft(a, ops, cx - bw / 2.0f, cy - float(g.P(30)), bw, float(g.P(60)), float(g.P(24)), Color{0, 0, 0, 215});
      text_alt(a, ops, it.full_name, cx, cy, pixel_px(g) * 2, kWhite, bw, C);
    }
  }
  if (at_rest(cam)) {  // on the job thread, one by one: the next logos are ready before they slide in, and the one of the list before it opens
    for (int dd : {1, -1, 2, -2}) {
      const std::string np = sys_file(th, "_inc/systems/logos", items[size_t(rem_e(mid + dd, n))].theme_name, ".png");
      ImgId nid;
      if (!np.empty()) pic(a, np, bw, bh, true, nid);
    }
    const std::string ll = sys_file(th, "_inc/systems/logos", cur.theme_name, ".png");
    ImgId nid;
    if (!ll.empty()) pic(a, ll, float(g.P(180)), float(g.P(40)), true, nid);
  }
  // the clock and the help must not fight with the picture: it steps back at both ends of the screen
  top_fade(a, ops, g.w, float(g.P(72)), alpha(kInkT, 235));
  bottom_fade(a, ops, g.w, g.h, float(g.P(116)), alpha(kInkT, 245));
  if (cur.games != kCountPending) {
    const size_t from = ops.size();
    std::string t = games_of(cur.games);
    for (char& c : t) c = char(std::toupper(static_cast<unsigned char>(c)));
    const int px = pixel_px(g);
    a.use_alt_face(true);
    const float tw = pill_w(g, a.measure(t, float(px)), float(g.U(2)), true), ph = float(g.U(4));
    a.use_alt_face(false);
    const float yy = float(g.help_top() - g.U(6)) + ph / 2.0f, bx = std::round(float(g.margin()) + (g.span(12) - tw) / 2.0f);
    const float st = float(std::max(2, g.P(2)));
    rec(bx, yy - ph / 2.0f, tw, ph, "tarjeta", 'B', false, true);
    ops.push_back(Op::rect(bx, yy - ph / 2.0f, tw, ph, kInkT));
    pixel_frame(ops, bx, yy - ph / 2.0f, tw, ph, kOrange, st);
    text_alt(a, ops, t, g.w / 2.0f, yy, px, kOrange, 0, C);
    motion::fade(ops, from, fade.alpha);
  }
  status_bar(th, a, g.w, g.h, status, ops);
  HelpLook hl;
  hl.fg = rgb(0xEBEBEE);
  hl.accent = kOrange;
  hl.pill = Color{8, 8, 10, 215};
  hl.cy = float(g.help_y());
  hl.right = g.w - float(g.margin());
  help(th, a, g, system_help(), hl, ops);
}

void tech_list(const Theme& th, Assets& a, const Grid& g, const std::vector<std::string>& names, size_t cursor, const Status& status, const DetailedData* d, std::vector<Op>& ops) {
  ops.push_back(Op::rect(0, 0, g.w, g.h, kInkT));
  const float pw = g.col(6) - g.gutter(), u = float(g.u);
  ops.push_back(Op::rect(0, 0, pw, g.h, kPanelT));
  const int m = g.margin();
  const std::string logo = sys_file(th, "_inc/systems/logos", g_state.system_theme, ".png");
  ImgId lg;
  if (!logo.empty() && pic(a, logo, float(g.P(180)), float(g.P(40)), true, lg)) put_fit(a, ops, lg, float(m), float(g.top()), cells_of(g, 180), float(g.bar_h()), kWhite, 0.0f);
  const int y0 = g.content_top(), pitch = g.snap(float(g.P(36)), 3), fs = g.F(17, 14), pn = pixel_px(g);
  const size_t rows = size_t(std::max(1, (g.help_top() - y0) / pitch));
  const RowWindow rw = row_window(names.size(), cursor, rows);
  if (rw.moving) ops.push_back(Op::clip(0, float(y0), pw, float(rows) * float(pitch)));
  if (!names.empty()) {  // the selected row: a band of whole cells, from the edge of the screen to the column
    const float ys = float(y0) + rw.sel * float(pitch);
    rec(float(m), ys, pw - float(m), float(pitch), "tarjeta", 'C', false, true);  // the band bleeds to the edge of the screen: its row is what the margins hold
    ops.push_back(Op::rect(0, ys, pw, float(pitch), kOrange));
  }
  for (size_t k = rw.first; k < rw.end; ++k) {
    const float y = float(y0) + (float(k) - rw.start) * float(pitch) + float(pitch) / 2.0f;
    const bool on = k == rw.lit;
    std::string cased_row;
    char num[16];
    std::snprintf(num, sizeof num, "%02zu", (k + 1) % 100);
    text_alt(a, ops, num, float(m), y, pn, on ? kInkT : rgb(0xB4631F));
    text(a, ops, list_cased(th, names[k], cased_row), g.col(1), y, fs, on ? kInkT : rgb(0xC3C3C8), pw - g.col(1) - float(g.U(1)));
  }
  if (rw.moving) ops.push_back(Op::unclip());
  const float x = pw + g.gutter(), w = g.w - x - float(m);
  if (d) {
    const bool has = has_data(*d);
    const int lh = g.F(20, 16);
    const float reserve = float(g.U(3) + (has ? g.U(8) + 2 * lh : g.U(5)));
    float y = float(g.content_top()), cw, ch;
    box43c(g, w, float(g.help_top()) - y - reserve, cw, ch);
    if (!has) y = down_to_line(g, float(g.content_top()), (float(g.help_top() + g.content_top()) - ch - reserve) / 2.0f);
    const float st = float(std::max(2, g.P(2)));
    const float ix = x + 2 * st, iy = y + 2 * st, iw0 = cw - 4 * st, ih0 = ch - 4 * st;  // what the frame holds
    video::box_offer(ix, iy, iw0, ih0);  // where the list preview plays
    ImgId id;
    const bool under = vpreview::hides(d->video, ix, iy, iw0, ih0);  // the game's video plays here: the frame only
    const bool have = !under && !d->image.empty() && exists_once(d->image) && pic(a, d->image, iw0, ih0, true, id);
    float ow = iw0, oh = ih0;  // the whole cover, whatever its shape
    if (have) fit_size(a.img(id), iw0, ih0, ow, oh);
    // the frame is a box of whole cells around it, centred in the room there is
    const float fw = std::min(cw, std::ceil((ow + 4 * st) / u) * u), fh = std::min(ch, std::ceil((oh + 4 * st) / u) * u);
    const float fx = x + std::floor((cw - fw) / 2.0f / u) * u, fy = y + std::floor((ch - fh) / 2.0f / u) * u;
    const float ox = fx + 2 * st, oy = fy + 2 * st, iw1 = fw - 4 * st, ih1 = fh - 4 * st;
    rec(fx, fy, fw, fh, "tarjeta", 'B', false, true);
    if (have) soft(a, ops, ox, oy, iw1, ih1, float(g.P(12)), alpha(kOrange, 110));
    pixel_frame(ops, fx, fy, fw, fh, kOrange, st);
    if (have) {
      ops.push_back(Op::image(id, ox, oy, iw1, ih1));
      scanlines(a, ops, ox, oy, iw1, ih1, g.h);
    } else if (!under && !logo.empty() && pic(a, logo, cw * 0.7f, ch * 0.5f, true, lg)) {
      put_fit(a, ops, lg, x + cw * 0.15f, y + ch * 0.25f, cw * 0.7f, ch * 0.5f, Color{255, 255, 255, 170});
    }
    const float yt = y + ch + float(g.U(3));
    a.use_alt_face(true);
    std::string cased_name;
    const std::string_view shown_name = list_cased(th, d->name, cased_name);
    const bool fits = a.measure(shown_name, float(pn)) <= w;
    a.use_alt_face(false);
    if (fits) text_alt(a, ops, shown_name, x, yt, pn, kOrange);
    else text(a, ops, shown_name, x, yt, g.F(18, 15), kOrange, w);  // a title too long for the pixel face stays whole in the readable one
    if (has) {
      text(a, ops, meta_of(*d), x, yt + float(g.U(4)), g.F(14), rgb(0xAAAAAF), w);
      if (d->show_rating) stars(th, a, ops, x, yt + float(g.U(5)), g.P(14), d->rating, kOrange, Color{255, 255, 255, 45}, g.P(3));
      const float yd = yt + float(g.U(8));
      desc_block(a, ops, *d, x, yd, w, float(g.help_top()) - yd, g.F(14), rgb(0xCDCDD2));
    } else if (d->show_rating) {
      stars(th, a, ops, x, yt + float(g.U(2)), g.P(16), d->rating, kOrange, Color{255, 255, 255, 45}, g.P(3));
    }
  }
  status_bar(th, a, g.w, g.h, status, ops);
  HelpLook hl;
  hl.fg = rgb(0xBEBEC3);
  hl.accent = kOrange;
  hl.cy = float(g.help_y());
  hl.right = g.w - float(g.margin());
  help(th, a, g, list_help(), hl, ops);
}

// ---- status bars -----------------------------------------------------------------------------------------------------------------------------
// Drawn here, for each theme: no picture of anyone's icons. Masks in a unit square, tinted when drawn.

float mask_edge(float d) { return std::clamp(d / 0.03f + 0.5f, 0.0f, 1.0f); }  // d > 0 inside
float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
  const float vx = bx - ax, vy = by - ay;
  const float h = std::clamp(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
  return std::hypot(px - ax - vx * h, py - ay - vy * h);
}
// The icons of the status bars, as functions of a point of their box (u, v in 0..1) and of the box's size in px. Every bar makes them at the
// exact size whose ink is as tall as the digits of its hour (fit_on), and no stroke is thinner than kMinHalf * 2 px, or the bar loses it.
constexpr float kMinHalf = 0.65f;
auto fn_wifi(float t) {
  return [t](float u, float v, float n) {
    const float tt = std::max(t, kMinHalf / n), dx = u - 0.5f, dy = 0.84f - v, r = std::hypot(dx, dy);
    float al = mask_edge(std::max(0.085f, tt * 1.4f) - r);
    if (dy > 0 && std::fabs(dx) <= dy * 1.05f)
      for (float ring : {0.30f, 0.54f, 0.78f}) al = std::max(al, mask_edge(tt - std::fabs(r - ring)));
    return al;
  };
}
auto fn_bt(float t) {
  return [t](float u, float v, float n) {
    static const float pts[][2] = {{0.28f, 0.30f}, {0.70f, 0.68f}, {0.50f, 0.90f}, {0.50f, 0.10f}, {0.70f, 0.32f}, {0.28f, 0.70f}};
    float d = 9.0f;
    for (int i = 0; i < 5; ++i) d = std::min(d, seg_dist(u, v, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1]));
    return mask_edge(std::max(t, kMinHalf / n) - d);
  };
}
/// The battery of put_batt, in a box of its own, no taller than its ink (n = the box's height): of a square of side s = n / 0.48 it is the part
/// 0.04..0.97 across and 0.26..0.74 down: body to 0.86, tip on the right.
constexpr float kBattAspect = 0.93f / 0.48f;
auto fn_batt(float t) {
  return [t](float u, float v, float n) {
    const float s = n / 0.48f, uu = 0.04f + u * 0.93f, vv = 0.26f + v * 0.48f;
    const float tt = std::max(t, kMinHalf / s), dx = std::fabs(uu - 0.45f) - 0.41f, dy = std::fabs(vv - 0.5f) - 0.24f;
    float al = mask_edge(tt - std::fabs(std::max(dx, dy)));
    if (uu > 0.89f && uu < 0.97f && std::fabs(vv - 0.5f) < 0.11f) al = 1.0f;  // its tip
    return al;
  };
}
float batt_stroke(float t, float s) { return std::max(t, kMinHalf / s); }
void put_bolt(Assets& a, std::vector<Op>& ops, float cx, float cy, float hmax, Color c, Color ring);
/// Battery icon in its box (x, y, the image's width, height `h`): outline, level and, while charging, the bolt in `bolt_c` ringed in `ring`.
/// Its parts are laid out as in the square of side s whose 0.04..0.97 x 0.26..0.74 part the box is.
void put_batt(Assets& a, std::vector<Op>& ops, ImgId outline, float t, const Battery& b, float bx0, float by0, float h, Color c, Color low, Color bolt_c, Color ring) {
  const float s = h / 0.48f, x = bx0 - 0.04f * s, y = by0 - 0.26f * s;
  ops.push_back(Op::image(outline, bx0, by0, float(a.img(outline).w), h, c));
  const float in = (batt_stroke(t, s) + 0.035f) * s;
  const float bx = x + 0.04f * s + in, by = y + 0.26f * s + in, bw = 0.82f * s - 2 * in, bh = 0.48f * s - 2 * in;
  const Color lv = b.percent <= 15 && !b.charging ? low : c;
  ops.push_back(Op::rect(std::round(bx), std::round(by), std::max(1.0f, std::round(bw * float(b.percent) / 100.0f)), std::max(1.0f, std::round(bh)), lv));
  if (b.charging) put_bolt(a, ops, x + 0.45f * s, y + 0.5f * s, 0.48f * s, bolt_c, ring);
}
/// XMB: the battery of the PSP era, three cells that light with the charge (red when low) instead of one level. While charging they
/// light one after another as on the PSP: `frame` = the cells of that step (`Status::charge_frame`; < 0: the level).
void put_batt_cells(Assets& a, std::vector<Op>& ops, ImgId outline, float t, const Battery& b, int frame, float bx0, float by0, float h, Color c, Color low) {
  const float s = h / 0.48f, x = bx0 - 0.04f * s, y = by0 - 0.26f * s;
  ops.push_back(Op::image(outline, bx0, by0, float(a.img(outline).w), h, c));
  const float in = (batt_stroke(t, s) + 0.035f) * s;
  const float bx = std::round(x + 0.04f * s + in), by = std::round(y + 0.26f * s + in), bh = std::max(1.0f, std::round(0.48f * s - 2 * in));
  const float bw = std::round(0.82f * s - 2 * in), gap = std::max(1.0f, std::round(s * 0.04f)), cw = std::floor((bw - 2 * gap) / 3.0f);
  const int p = b.percent, lit = b.charging && frame >= 0 ? frame : p > 66 ? 3 : p > 33 ? 2 : p > 5 ? 1 : 0;
  const Color lc = p <= 15 && !b.charging ? low : c;
  for (int i = 0; i < lit; ++i) ops.push_back(Op::rect(bx + float(i) * (cw + gap), by, cw, bh, lc));
  (void)a;
}
std::string pct_text(const Status& st) { return st.battery && ui_prefs::battery_text() ? std::to_string(st.battery->percent) + "%" : std::string(); }

/// A mask made at the exact size whose ink is as tall as the line, drawn 1:1 with its ink on the line: its image, size and top.
struct Fitted {
  ImgId id = 0;
  float w = 0, h = 0, y = 0;
};
template <typename Fn>
Fitted fit_on(Assets& a, const char* key, float aspect, Fn fn, const InkLine& ln) {
  const int want = std::max(1, int(std::lround(ln.h)));
  int size = want, err = 1 << 20;
  Fitted best;
  for (int i = 0; i < 10; ++i) {
    const std::string k = std::string(key) + ":" + std::to_string(size);
    const float n = float(size);
    const auto make = [&] { return texture(a, k.c_str(), uint32_t(std::max(1L, std::lround(n * aspect))), uint32_t(size), [&fn, n](float u, float v) { return fn(u, v, n); }); };
    const ImgId id = measurable(a, make);
    int t, b;
    if (!ink_rows(a.img(id), t, b)) {
      size *= 2;
      continue;
    }
    const int got = b - t + 1, e = got - want;
    if (std::abs(e) < err) err = std::abs(e), best = Fitted{id, float(a.img(id).w), n, ln.top - float(t)};
    if (e == 0) break;
    const int next = i == 0 ? std::max(2, int(std::lround(n * float(want) / float(got)))) : size - (e > 0 ? 1 : -1);
    if (next == size) break;
    size = next;
  }
  return best;
}
/// `s` in the size found for it whose ink is as tall as the line, its ink on the line.
struct OnLine {
  int px = 0;
  float w = 0, cy = 0;
};
OnLine text_fit(Assets& a, std::string_view s, int px, const InkLine& ln) {
  OnLine o;
  o.px = px_for_ink(a, s, int(std::lround(ln.h)), px);
  const Img& im = text_px(a, s, o.px);
  int t, b;
  if (!ink_rows(im, t, b)) t = 0;
  o.cy = ln.top - float(t) + float(im.h) / 2.0f;
  o.w = a.measure(s, float(o.px));
  return o;
}

/// XMB: a thin rounded box in the corner, as on the console: connections, date and hour, battery.
void status_xmb(Assets& a, const Grid& g, const Status& st, std::vector<Op>& ops) {
  const float gap = float(g.P(7)), cy = float(g.status_y());
  const int px = g.F(15, 12);
  const Color w{255, 255, 255, 235};
  std::string when = st.time;
  if (!when.empty() && !st.date.empty()) {
    when = st.date + "  " + when;
  } else if (!when.empty()) {
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
    char day[16];
    if (localtime_r(&now, &tmv) && std::strftime(day, sizeof day, "%d/%m", &tmv)) when = std::string(day) + "  " + when;
  }
  const InkLine ln = ink_line(a, when, px, cy);
  const std::string pct = pct_text(st);
  const Fitted fb = st.bluetooth ? fit_on(a, "rm:si:bx", 1.0f, fn_bt(0.055f), ln) : Fitted{};
  const Fitted fw = st.wifi ? fit_on(a, "rm:si:wx", 1.0f, fn_wifi(0.055f), ln) : Fitted{};
  const Fitted fa = st.battery ? fit_on(a, "rm:si:ax", kBattAspect, fn_batt(0.04f), ln) : Fitted{};
  const OnLine pt = pct.empty() ? OnLine{} : text_fit(a, pct, px, ln);
  const float tw = when.empty() ? 0.0f : a.measure(when, float(px));
  float total = 0.0f;
  const auto add = [&](float v) { total += (total > 0 ? gap : 0.0f) + v; };
  if (st.bluetooth) add(fb.w);
  if (st.wifi) add(fw.w);
  if (tw > 0) add(tw);
  if (st.battery) add(fa.w);
  if (pt.w > 0) add(pt.w);
  if (total <= 0) return;
  const float pad = float(g.P(11)), h = float(g.bar_h());
  const float bw = pill_w(g, total, pad), x0 = g.w - float(g.margin()) - bw;
  const size_t bg = ops.size();
  g_shapes.rrect(a, ops, x0, cy - h / 2.0f, bw, h, g.P(7), Color{255, 255, 255, 30});
  g_shapes.ring(a, ops, x0, cy - h / 2.0f, bw, h, g.P(7), std::max(1.0f, float(g.P(1))), Color{255, 255, 255, 120});
  status_bg(ops, bg);
  float x = std::round(x0 + (bw - total) / 2.0f);
  if (st.bluetooth) {
    ops.push_back(Op::image(fb.id, x, fb.y, fb.w, fb.h, w));
    x += fb.w + gap;
  }
  if (st.wifi) {
    ops.push_back(Op::image(fw.id, x, fw.y, fw.w, fw.h, w));
    x += fw.w + gap;
  }
  if (tw > 0) {
    text(a, ops, when, x, cy, px, w);
    x += tw + gap;
  }
  if (st.battery) {
    put_batt_cells(a, ops, fa.id, 0.04f, *st.battery, st.charge_frame, std::round(x), fa.y, fa.h, w, rgb(0xFF6A5A));
    x += fa.w + gap;
  }
  if (pt.w > 0) text(a, ops, pct, x, pt.cy, pt.px, w);
}

/// Art Book: nothing but thin strokes and a bold hour, like a caption on a page.
void status_art(Assets& a, const Grid& g, const Status& st, std::vector<Op>& ops) {
  const float gap = float(g.P(9)), cy = float(g.status_y());
  const int px = g.F(17, 13);
  a.use_alt_face(true);
  const InkLine ln = ink_line(a, st.time, px, cy);
  const float tw = st.time.empty() ? 0.0f : a.measure(st.time, float(px));
  const std::string pct = pct_text(st);
  const OnLine pt = pct.empty() ? OnLine{} : text_fit(a, pct, px, ln);  // the percent in the hour's face, weight and colour
  a.use_alt_face(false);
  float x = g.w - float(g.margin());
  if (tw > 0) {
    text_alt(a, ops, st.time, x, cy, px, kWhite, 0, R);
    x -= tw + gap * 1.4f;
  }
  if (pt.w > 0) {
    text_alt(a, ops, pct, x, pt.cy, pt.px, kWhite, 0, R);
    x -= pt.w + gap;
  }
  if (st.battery) {
    const Fitted f = fit_on(a, "rm:si:aa", kBattAspect, fn_batt(0.022f), ln);
    x = std::round(x - f.w);
    put_batt(a, ops, f.id, 0.022f, *st.battery, x, f.y, f.h, kWhite, rgb(0xFF6A5A), kWhite, Color{10, 10, 14, 255});
    x -= gap;
  }
  if (st.wifi) {
    const Fitted f = fit_on(a, "rm:si:wa", 1.0f, fn_wifi(0.028f), ln);
    x = std::round(x - f.w);
    ops.push_back(Op::image(f.id, x, f.y, f.w, f.h, kWhite));
    x -= gap;
  }
  if (st.bluetooth) {
    const Fitted f = fit_on(a, "rm:si:ba", 1.0f, fn_bt(0.028f), ln);
    x = std::round(x - f.w);
    ops.push_back(Op::image(f.id, x, f.y, f.w, f.h, kWhite));
  }
}

/// Iconic: a white pill with its icons in their colours, as tall as the digits of the hour, like the coloured icons of its menu.
void status_iconic(const Theme& th, Assets& a, const Grid& g, const Status& st, std::vector<Op>& ops) {
  const IconicInk sc = iconic_ink(th);
  const float gap = float(g.P(8)), cy = float(g.status_y()), pad = float(g.P(10)), h = float(g.bar_h());
  const int px = g.F(15, 12);
  const Color ink = sc.ink;
  const InkLine ln = ink_line(a, st.time, px, cy);
  const std::string pct = pct_text(st);
  const OnLine pt = pct.empty() ? OnLine{} : text_fit(a, pct, px, ln);
  const float tw = st.time.empty() ? 0.0f : a.measure(st.time, float(px));
  const Fitted fb = st.bluetooth ? fit_on(a, "rm:si:bi", 1.0f, fn_bt(0.07f), ln) : Fitted{};
  const Fitted fw = st.wifi ? fit_on(a, "rm:si:wi", 1.0f, fn_wifi(0.07f), ln) : Fitted{};
  const Fitted fa = st.battery ? fit_on(a, "rm:si:ai", kBattAspect, fn_batt(0.05f), ln) : Fitted{};
  float total = 0.0f;
  const auto add = [&](float v) { total += (total > 0 ? gap : 0.0f) + v; };
  if (st.bluetooth) add(fb.w);
  if (st.wifi) add(fw.w);
  if (st.battery) add(fa.w);
  if (pt.w > 0) add(pt.w);
  if (tw > 0) add(tw);
  if (total <= 0) return;
  const float bw = pill_w(g, total, pad), x0 = g.w - float(g.margin()) - bw;
  const size_t bg = ops.size();
  g_shapes.pill(a, ops, x0, cy - h / 2.0f, bw, h, alpha(sc.card, 240));
  status_bg(ops, bg);
  float x = std::round(x0 + (bw - total) / 2.0f);
  if (st.bluetooth) {
    ops.push_back(Op::image(fb.id, x, fb.y, fb.w, fb.h, iconic_text(sc, rgb(0x4B4FC4))));
    x += fb.w + gap;
  }
  if (st.wifi) {
    ops.push_back(Op::image(fw.id, x, fw.y, fw.w, fw.h, rgb(0x2F80ED)));
    x += fw.w + gap;
  }
  if (st.battery) {
    const int p = st.battery->percent;
    const Color c = st.battery->charging || p > 40 ? rgb(0x27AE60) : p > 15 ? rgb(0xE8A317) : rgb(0xE5484D);
    put_batt(a, ops, fa.id, 0.05f, *st.battery, x, fa.y, fa.h, c, c, ink, sc.card);
    x += fa.w + gap;
  }
  if (pt.w > 0) {
    text(a, ops, pct, x, pt.cy, pt.px, ink);
    x += pt.w + gap;
  }
  if (tw > 0) text(a, ops, st.time, x, cy, px, ink);
}

/// Aurora: one glass pill; the clock sits in a bubble of its own at its right end.
void status_aurora(const Theme& th, Assets& a, const Grid& g, const Status& st, std::vector<Op>& ops) {
  const IconicInk sc = iconic_ink(th);
  const bool dk = sc.dark;
  const int px = g.L(ui::Level::Small);  // hour and percent: the Small level (fits the 4-cell bar)
  // the bubble inside the bar keeps a line of Row plus a cell: its inset shrinks before the text would not fit
  const float gap = float(g.P(10)), cy = float(g.status_y()), h = float(g.bar_h()), inset = std::max(1.0f, std::min(float(std::max(2, g.P(4))), std::floor((float(g.bar_h()) - float(px + g.u)) / 2.0f)));
  const Color ink = dk ? kWhite : rgb(0x1D1B3A);
  const InkLine ln = ink_line(a, st.time, px, cy);
  const std::string pct = pct_text(st);
  const OnLine pt = pct.empty() ? OnLine{} : text_fit(a, pct, px, ln);
  const float tw = st.time.empty() ? 0.0f : a.measure(st.time, float(px));
  const Fitted fb = st.bluetooth ? fit_on(a, "rm:si:bi", 1.0f, fn_bt(0.07f), ln) : Fitted{};
  const Fitted fw = st.wifi ? fit_on(a, "rm:si:wi", 1.0f, fn_wifi(0.07f), ln) : Fitted{};
  const Fitted fa = st.battery ? fit_on(a, "rm:si:ai", kBattAspect, fn_batt(0.05f), ln) : Fitted{};
  float icons = 0.0f;
  const auto add = [&](float v) { icons += (icons > 0 ? gap : 0.0f) + v; };
  if (st.bluetooth) add(fb.w);
  if (st.wifi) add(fw.w);
  if (st.battery) add(fa.w);
  if (pt.w > 0) add(pt.w);
  const float lpad = float(g.P(14)), bub_w = tw > 0 ? tw + 2.0f * float(g.P(12)) : 0.0f, bub_h = h - 2.0f * inset;
  const float total = lpad + icons + (icons > 0 && bub_w > 0 ? gap : 0.0f) + bub_w + (bub_w > 0 ? inset : lpad);
  if (icons <= 0 && bub_w <= 0) return;
  const float bw = std::round(total), x0 = g.w - float(g.margin()) - bw;
  const size_t bg = ops.size();
  g_shapes.pill(a, ops, x0, cy - h / 2.0f, bw, h, alpha(kWhite, dk ? 40 : 168));
  if (bub_w > 0) g_shapes.pill(a, ops, x0 + bw - inset - bub_w, cy - bub_h / 2.0f, bub_w, bub_h, alpha(kWhite, dk ? 56 : 255));
  status_bg(ops, bg);
  float x = std::round(x0 + lpad);
  if (st.bluetooth) {
    ops.push_back(Op::image(fb.id, x, fb.y, fb.w, fb.h, dk ? rgb(0xB9A8FF) : rgb(0x3B2FA0)));
    x += fb.w + gap;
  }
  if (st.wifi) {
    ops.push_back(Op::image(fw.id, x, fw.y, fw.w, fw.h, ink));
    x += fw.w + gap;
  }
  if (st.battery) {
    const int p = st.battery->percent;
    const Color c = st.battery->charging || p > 40 ? (dk ? rgb(0x5EF2D4) : rgb(0x0FA88A)) : p > 15 ? rgb(0xE8A317) : rgb(0xE5484D);
    put_batt(a, ops, fa.id, 0.05f, *st.battery, x, fa.y, fa.h, c, c, ink, alpha(kWhite, 0));
    x += fa.w + gap;
  }
  if (pt.w > 0) {
    text(a, ops, pct, x, pt.cy, pt.px, ink);
    x += pt.w + gap;
  }
  if (tw > 0) text(a, ops, st.time, x0 + bw - inset - bub_w + (bub_w - tw) / 2.0f, cy, px, ink);
}

/// Swiss: no box. The marks and the numbers stand on the page in its ink, separated by thin vertical rules.
void status_swiss(const Theme& th, Assets& a, const Grid& g, const Status& st, std::vector<Op>& ops) {
  const IconicInk sc = iconic_ink(th);
  const bool dk = sc.dark;
  const Color ink = dk ? kWhite : rgb(0x12163A), page = dk ? rgb(0x2540FF) : rgb(0xF3F1EA);
  const float gap = float(g.P(12)), cy = float(g.status_y());
  const int px = g.L(ui::Level::Small);  // hour and percent: the Small level (fits the 4-cell bar)
  const InkLine ln = ink_line(a, st.time, px, cy);
  const std::string pct = pct_text(st);
  const OnLine pt = pct.empty() ? OnLine{} : text_fit(a, pct, px, ln);
  a.use_alt_face(true);
  const float tw = st.time.empty() ? 0.0f : a.measure(st.time, float(px));  // drawn in the second face
  a.use_alt_face(false);
  const Fitted fb = st.bluetooth ? fit_on(a, "rm:si:bi", 1.0f, fn_bt(0.07f), ln) : Fitted{};
  const Fitted fw = st.wifi ? fit_on(a, "rm:si:wi", 1.0f, fn_wifi(0.07f), ln) : Fitted{};
  const Fitted fa = st.battery ? fit_on(a, "rm:si:ai", kBattAspect, fn_batt(0.05f), ln) : Fitted{};
  const float rule_w = float(std::max(1, g.P(2))), rule_h = float(g.bar_h()) * 0.6f;
  const bool marks = st.bluetooth || st.wifi, power = st.battery.has_value() || pt.w > 0, clock = tw > 0;
  float total = 0.0f;
  const auto add = [&](float v) { total += (total > 0 ? gap : 0.0f) + v; };
  if (st.bluetooth) add(fb.w);
  if (st.wifi) add(fw.w);
  if (marks && (power || clock)) add(rule_w);
  if (st.battery) add(fa.w);
  if (pt.w > 0) add(pt.w);
  if (power && clock) add(rule_w);
  if (clock) add(tw);
  if (total <= 0) return;
  float x = std::round(g.w - float(g.margin()) - total);
  if (st.bluetooth) {
    ops.push_back(Op::image(fb.id, x, fb.y, fb.w, fb.h, ink));
    x += fb.w + gap;
  }
  if (st.wifi) {
    ops.push_back(Op::image(fw.id, x, fw.y, fw.w, fw.h, ink));
    x += fw.w + gap;
  }
  if (marks && (power || clock)) {
    ops.push_back(Op::rect(x, cy - rule_h / 2.0f, rule_w, rule_h, ink));
    x += rule_w + gap;
  }
  if (st.battery) {
    put_batt(a, ops, fa.id, 0.05f, *st.battery, x, fa.y, fa.h, ink, ink, ink, page);
    x += fa.w + gap;
  }
  if (pt.w > 0) {
    text(a, ops, pct, x, pt.cy, pt.px, ink);
    x += pt.w + gap;
  }
  if (power && clock) {
    ops.push_back(Op::rect(x, cy - rule_h / 2.0f, rule_w, rule_h, ink));
    x += rule_w + gap;
  }
  if (clock) text_alt(a, ops, st.time, x, cy, px, ink);
}

/// Industrial: the readout of an appliance, a window with a thick outline: wifi, battery, charge and time in it.
void status_industrial(const Theme& th, Assets& a, const Grid& g, const Status& st, std::vector<Op>& ops) {
  const IconicInk sc = iconic_ink(th);
  const bool dk = sc.dark;
  const Color ink = dk ? rgb(0xF1EFE6) : rgb(0x1B1A17), page = dk ? rgb(0x1D1C1A) : rgb(0xE8E6DF);
  const float gap = float(g.P(12)), cy = float(g.status_y()), pad = float(g.P(12));
  const int px = g.L(ui::Level::Small);  // hour and percent: the Small level (fits the 4-cell bar)
  const InkLine ln = ink_line(a, st.time, px, cy);
  const std::string pct = pct_text(st);
  a.use_alt_face(true);  // the readout is set in the second face
  const OnLine pt = pct.empty() ? OnLine{} : text_fit(a, pct, px, ln);
  const float tw = st.time.empty() ? 0.0f : a.measure(st.time, float(px));
  a.use_alt_face(false);
  const Fitted fw = st.wifi ? fit_on(a, "rm:si:wi", 1.0f, fn_wifi(0.07f), ln) : Fitted{};
  const Fitted fa = st.battery ? fit_on(a, "rm:si:ai", kBattAspect, fn_batt(0.05f), ln) : Fitted{};
  float total = 0.0f;
  const auto add = [&](float v) { total += (total > 0 ? gap : 0.0f) + v; };
  if (st.wifi) add(fw.w);
  if (st.battery) add(fa.w);
  if (pt.w > 0) add(pt.w);
  if (tw > 0) add(tw);
  if (total <= 0) return;
  const float bw = total + 2.0f * pad, bh = float(g.bar_h()), bx = std::round(g.w - float(g.margin()) - bw), by = std::round(cy - bh / 2.0f);
  g_shapes.ring(a, ops, bx, by, bw, bh, std::max(3, g.P(6)), float(std::max(2, g.P(2))), ink);
  float x = bx + pad;
  if (st.wifi) {
    ops.push_back(Op::image(fw.id, x, fw.y, fw.w, fw.h, ink));
    x += fw.w + gap;
  }
  if (st.battery) {
    put_batt(a, ops, fa.id, 0.05f, *st.battery, x, fa.y, fa.h, ink, ink, ink, page);
    x += fa.w + gap;
  }
  if (pt.w > 0) {
    text_alt(a, ops, pct, x, pt.cy, pt.px, ink);
    x += pt.w + gap;
  }
  if (tw > 0) text_alt(a, ops, st.time, x, cy, px, ink);
}

/// TechDweeb: a black plate with an orange pixel frame, icons drawn dot by dot, as many rows as the digits of the pixel face are tall.
void bits(std::vector<Op>& ops, const char* const* rows, int n, float x, float top, float b, Color c) {
  for (int r = 0; r < n; ++r)
    for (int i = 0; rows[r][i];) {
      if (rows[r][i] != '#') {
        ++i;
        continue;
      }
      int e = i;
      while (rows[r][e] == '#') ++e;
      ops.push_back(Op::rect(std::round(x) + float(i) * b, top + float(r) * b, float(e - i) * b, b, c));
      i = e;
    }
}
void status_tech(Assets& a, const Grid& g, const Status& st, std::vector<Op>& ops) {
  static const char* const kWifi[] = {".#######.", "#.......#", "..#####..", ".#.....#.", "...###...", "....#...."};
  // Bluetooth needs more rows than 6 to look like itself: 12 of half a dot, with a stroke of a whole one
  static const char* const kBt[] = {"...##...", "...###..", "...####.", "##.##.##", ".######.", "..####..", "..####..", ".######.", "##.##.##", "...####.", "...###..", "...##..."};
  static const char* const kBatt[] = {"###########..", "#.........#..", "#.........###", "#.........###", "#.........#..", "###########.."};
  constexpr int kRows = 6;
  const float cy = float(g.status_y()), gap = float(g.P(8));
  const int px = pixel_px(g);
  const std::string pct = pct_text(st);
  a.use_alt_face(true);
  const InkLine ln = ink_line(a, st.time, px, cy);
  const float tw = st.time.empty() ? 0.0f : a.measure(st.time, float(px));
  const OnLine pt = pct.empty() ? OnLine{} : text_fit(a, pct, px, ln);
  a.use_alt_face(false);
  const float b = std::max(1.0f, std::round(ln.h / float(kRows))), top = ln.top + std::round((ln.h - float(kRows) * b) / 2.0f);
  float total = 0.0f;
  const auto add = [&](float v) { total += (total > 0 ? gap : 0.0f) + v; };
  if (st.bluetooth) add(4 * b);
  if (st.wifi) add(9 * b);
  if (st.battery) add(13 * b);
  if (pt.w > 0) add(pt.w);
  if (tw > 0) add(tw);
  if (total <= 0) return;
  const float pad = float(g.P(8)), h = float(g.bar_h()), bw = pill_w(g, total, pad), x0 = g.w - float(g.margin()) - bw;
  const float t = std::max(2.0f, b);
  const size_t bg = ops.size();
  ops.push_back(Op::rect(x0, cy - h / 2.0f, bw, h, kInkT));
  pixel_frame(ops, x0, cy - h / 2.0f, bw, h, kOrange, t);
  status_bg(ops, bg);
  float x = x0 + (bw - total) / 2.0f;
  if (st.bluetooth) {
    bits(ops, kBt, 2 * kRows, x, top, b / 2.0f, kOrange);
    x += 4 * b + gap;
  }
  if (st.wifi) {
    bits(ops, kWifi, kRows, x, top, b, kOrange);
    x += 9 * b + gap;
  }
  if (st.battery) {
    bits(ops, kBatt, kRows, x, top, b, kOrange);
    const int lvl = std::clamp((int(st.battery->percent) * 9 + 50) / 100, 1, 9);
    const Color lc = st.battery->percent <= 15 && !st.battery->charging ? rgb(0xFF3B30) : kOrange;
    ops.push_back(Op::rect(std::round(x) + b, top + b, float(lvl) * b, 4 * b, lc));
    if (st.battery->charging) {  // a bolt of dots over the level: dark where the level is, orange past it
      static const char* const kBolt[] = {"..#", ".##", "##.", "#.."};
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
          if (kBolt[r][c] == '#') ops.push_back(Op::rect(std::round(x) + float(4 + c) * b, top + float(1 + r) * b, b, b, 4 + c <= lvl ? kInkT : kOrange));
    }
    x += 13 * b + gap;
  }
  if (pt.w > 0) {
    a.use_alt_face(true);
    text(a, ops, pct, x, pt.cy, pt.px, kOrange);
    a.use_alt_face(false);
    x += pt.w + gap;
  }
  if (tw > 0) text_alt(a, ops, st.time, x, cy, px, kOrange);
}

// ---- Zenith Next (theme variable zenStyle = next) ------------------------------------------------------------------------------------------

// ---- Zenith Next status bar: strokes as heavy as the digits next to them ----
float poly_d(float u, float v, const float (*p)[2], int n) {  // > 0 inside
  float d = 9.0f;
  bool in = false;
  for (int i = 0, j = n - 1; i < n; j = i++) {
    d = std::min(d, seg_dist(u, v, p[j][0], p[j][1], p[i][0], p[i][1]));
    if ((p[i][1] > v) != (p[j][1] > v) && u < (p[j][0] - p[i][0]) * (v - p[i][1]) / (p[j][1] - p[i][1]) + p[i][0]) in = !in;
  }
  return in ? d : -d;
}
float rbox_d(float u, float v, float cx, float cy, float hw, float hh, float r) {  // > 0 inside a rounded box
  const float qx = std::fabs(u - cx) - hw + r, qy = std::fabs(v - cy) - hh + r;
  return -(std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r);
}
float nx_wifi(float u, float v, float) {  // three filled bands of one 90 degree fan, inside its box
  const float dx = u - 0.5f, dy = 0.86f - v, r = std::hypot(dx, dy);
  const float fan = (dy - std::fabs(dx)) * 0.7071f;
  const float band = std::max({0.19f - r, std::min(r - 0.30f, 0.46f - r), std::min(r - 0.56f, 0.71f - r)});
  return mask_edge(std::min(fan, band));
}
float bolt_d(float u, float v) {
  static const float p[][2] = {{0.70f, 0.04f}, {0.12f, 0.56f}, {0.46f, 0.56f}, {0.30f, 0.96f}, {0.88f, 0.42f}, {0.54f, 0.42f}};
  return poly_d(u, v, p, 6) * 1.5f;
}
float nx_bolt(float u, float v, float) { return mask_edge(bolt_d(u, v)); }
/// A bolt with a ring of a pixel around it, centred on (cx, cy), the ring no taller than `hmax`: the ring, in the colour of the page, keeps
/// it apart from the level and the outline under it.
void put_bolt(Assets& a, std::vector<Op>& ops, float cx, float cy, float hmax, Color c, Color ring) {
  Fitted f;
  ImgId r = 0;
  int rt = 0, rb = -1;
  for (int tgt = int(hmax) - 2; tgt >= 3; --tgt) {
    f = fit_on(a, "rm:sb:bolt", 2.0f / 3.0f, nx_bolt, InkLine{0.0f, float(tgt)});
    const float rw = 1.5f / f.h;  // a pixel of ring, in the units of bolt_d
    const std::string k = "rm:sb:boltring:" + std::to_string(int(f.w)) + "x" + std::to_string(int(f.h));
    r = measurable(a, [&] { return texture(a, k.c_str(), uint32_t(f.w), uint32_t(f.h), [rw](float u, float v) { return mask_edge(bolt_d(u, v) + rw); }); });
    if (!ink_rows(a.img(r), rt, rb)) return;
    if (float(rb - rt + 1) <= hmax) break;
  }
  if (rb < 0) return;
  const float x = std::round(cx - f.w / 2.0f), y = std::round(cy - float(rb - rt + 1) / 2.0f) - float(rt);
  ops.push_back(Op::image(r, x, y, f.w, f.h, ring));
  ops.push_back(Op::image(f.id, x, y, f.w, f.h, c));
}
// Battery in a 2:1 box, in units of its height: body 0.06..1.78, tip after it.
float nx_batt_t(float n) { return std::max(0.13f, 2.0f * kMinHalf / n); }
float nx_batt(float u, float v, float n) {
  const float x = u * 2.0f, t = nx_batt_t(n), body = rbox_d(x, v, 0.92f, 0.5f, 0.86f, 0.40f, 0.18f);
  const float tip = rbox_d(x, v, 1.89f, 0.5f, 0.07f, 0.16f, 0.06f);
  return mask_edge(std::max(std::min(body, t - body), tip));
}

/// Right to left from the margin: Bluetooth, WiFi, the bolt while charging, battery, percent, hour. Low battery is red,
/// charging fills in the accent. Every icon is as tall as the digits of the hour and on their line, as on phones.
void status_zen(const Theme& th, Assets& a, const Grid& g, const Status& st, std::vector<Op>& ops) {
  const ui::Style sty = ui::Style::from(th);
  const float gap = float(g.P(10)), cy = float(g.status_y());
  const int px = g.F(16, 13);
  const InkLine ln = ink_line(a, st.time, px, cy);
  const bool low = st.battery && st.battery->percent <= 15 && !st.battery->charging;
  const bool chg = st.battery && st.battery->charging;
  const Color fg = sty.txt, lvl = low ? sty.danger : chg ? sty.accent() : fg;
  const std::string pct = pct_text(st);
  struct Item {
    float w, before;
    std::function<void(float)> draw;
  };
  std::vector<Item> items;
  const auto add = [&](float w, float before, std::function<void(float)> f) { items.push_back({w, items.empty() ? 0.0f : before, std::move(f)}); };
  if (st.bluetooth) {
    const Fitted f = fit_on(a, "rm:nx:bt", 1.0f, fn_bt(0.08f), ln);
    add(f.w, gap, [&, f](float x) { ops.push_back(Op::image(f.id, x, f.y, f.w, f.h, fg)); });
  }
  if (st.wifi) {
    const Fitted f = fit_on(a, "rm:nx:wifi", 1.0f, nx_wifi, ln);
    add(f.w, gap, [&, f](float x) { ops.push_back(Op::image(f.id, x, f.y, f.w, f.h, fg)); });
  }
  if (st.battery) {
    const Fitted f = fit_on(a, "rm:nx:batt", 2.0f, nx_batt, ln);
    add(f.w, gap, [&, f](float x) {
      ops.push_back(Op::image(f.id, x, f.y, f.w, f.h, low ? sty.danger : fg));
      const float bh = f.h, m = nx_batt_t(bh) + 0.08f, x0 = x + (0.06f + m) * bh, x1 = x + (1.78f - m) * bh, y0 = f.y + (0.10f + m) * bh, y1 = f.y + (0.90f - m) * bh;
      const float fw = std::max(1.0f, std::round((x1 - x0) * float(st.battery->percent) / 100.0f));
      g_shapes.rrect(a, ops, std::round(x0), std::round(y0), fw, std::round(y1 - y0), std::max(1, int((0.18f - m) * bh)), lvl);
      if (chg) put_bolt(a, ops, x + 0.92f * bh, f.y + 0.5f * bh, 0.8f * bh, fg, sty.bg);
    });
  }
  if (!pct.empty()) {
    const OnLine o = text_fit(a, pct, g.F(14, 12), ln);
    add(o.w, gap, [&, o](float x) { text(a, ops, pct, x, o.cy, o.px, low ? sty.danger : fg); });
  }
  if (!st.time.empty()) add(a.measure(st.time, float(px)), gap * 1.3f, [&](float x) { text(a, ops, st.time, x, cy, px, fg); });
  // from the margin leftwards, each one on a whole pixel inside the room the one on its right left it
  float right = g.w - float(g.margin());
  for (auto it = items.rbegin(); it != items.rend(); ++it) {
    const float x = std::floor(right - it->w);
    it->draw(x);
    right = x - it->before;
  }
}

/// The height of a line of text at `px`, in whole cells (at least one): the band a centred text lives in.
int line_cells(Assets& a, const Grid& g, int px) { return std::max(1, (int(a.img(a.text("Ag", float(px), kWhite)).h) + g.u - 1) / g.u); }

void zen_back(const Theme& th, Assets& a, const Grid& g, Color tint, std::vector<Op>& ops, float strength = 1.0f) {
  const ui::Style st = ui::Style::from(th);
  ops.push_back(Op::rect(0, 0, g.w, g.h, st.bg));
  const float r = std::max(g.w, g.h);
  disc(a, ops, g.w * 0.5f, g.h * 0.32f, r * 0.85f, alpha(tint, uint8_t(float(st.dark ? 120 : 80) * strength)));
  disc(a, ops, g.w, 0, r * 0.55f, alpha(st.cyan, st.dark ? 46 : 60));
}

void zen_avatar(const Theme& th, Assets& a, const Grid& g, std::vector<Op>& ops) {
  ImgId id;
  const float s = float(g.bar_h());
  if (!a.file(th.path("./assets/avatar.png"), id)) return;
  ops.push_back(Op::image(id, float(g.margin()), float(g.top()), s, s));
  rec(float(g.margin()), float(g.top()), s, s, "avatar", 'L', true, true);
}

/// One system: its logo on a card. `on` 0..1: how much of the selected one it is.
/// The card grows on every frame of a move: its pictures keep the sizes `rest_w`, `rest_h` (at rest, on the side) and `sel_w`, `sel_h`, and only their drawing scales.
void zen_card(const Theme& th, Assets& a, const Grid& g, const SystemItem& it, float cx, float cy, float w, float h, float on, float rest_w, float rest_h, float sel_w, float sel_h, std::vector<Op>& ops) {
  const ui::Style st = ui::Style::from(th);
  const float x = cx - w / 2.0f, y = cy - h / 2.0f;
  const int r = g.P(14);
  if (!st.dark) soft(a, ops, x, y + float(g.P(3)), w, h, float(g.P(10)), Color{20, 24, 40, uint8_t(26 + 30 * on)});
  // the fill closes first and the frame comes after, under a card that no longer lets it through: a mix of the two fills, or a frame seen through them, flashes
  const float fill = std::clamp(on / 0.4f, 0.0f, 1.0f), frame = std::clamp((on - 0.4f) / 0.6f, 0.0f, 1.0f);
  if (frame > 0.0f) {
    const float t = float(std::max(2, g.P(3)));
    soft(a, ops, x, y, w, h, float(g.P(14)), alpha(st.cyan, uint8_t(90 * frame)));
    g_shapes.rrect(a, ops, x - t, y - t, w + 2 * t, h + 2 * t, r + int(t), alpha(st.cyan, uint8_t(255 * frame)));
  }
  if (fill < 1.0f) g_shapes.rrect(a, ops, x, y, w, h, r, st.dark ? Color{255, 255, 255, 18} : Color{255, 255, 255, 200});
  if (fill > 0.0f) g_shapes.rrect(a, ops, x, y, w, h, r, alpha(st.dark ? Color{0x1D, 0x20, 0x26, 255} : Color{255, 255, 255, 255}, uint8_t(255 * fill)));
  const std::string logo = th.path("./assets/logos/" + it.theme_name + ".webp");
  ImgId id;
  const uint8_t al = uint8_t(150 + 105 * on);
  const float box_w = on > 0.0f ? sel_w : rest_w, box_h = on > 0.0f ? sel_h : rest_h, other_w = on > 0.0f ? rest_w : sel_w, other_h = on > 0.0f ? rest_h : sel_h;
  if (exists_once(logo)) {  // asked once, not a stat per card per frame
    const char* label = system_icon_label(it.theme_name);
    // on the job thread: until its size lands, the other size if there is one
    if (pic(a, logo, box_w * 0.76f, box_h * 0.58f, true, id) || covers::ready(a, logo, uint32_t(std::lround(other_w * 0.76f)), uint32_t(std::lround(other_h * 0.58f)), id)) {
      float ow, oh;
      fit_size(a.img(id), w * 0.76f, h * 0.58f, ow, oh);
      // the icons leave the bottom of their picture to their name: on the chosen card the name is below it, in big, and the icon takes the middle
      const float lift = label ? h * 0.07f * on : 0.0f;
      ops.push_back(Op::image(id, std::round(cx - ow / 2.0f), std::round(cy + lift - oh / 2.0f), ow, oh, Color{255, 255, 255, al}));
    }
    if (label && on < 1.0f) {
      const ImgId t = a.text(label, float(g.F(16, 12)), st.txt);
      const float k = h / sel_h, tw = float(a.img(t).w) * k, tth = float(a.img(t).h) * k;
      ops.push_back(Op::image(t, std::round(cx - tw / 2.0f), std::round(cy + h * 0.27f - tth / 2.0f), tw, tth, Color{255, 255, 255, uint8_t(float(al) * (1.0f - on))}));
    }
  } else {
    ImgId t = a.text_ellipsis(it.full_name, float(g.F(17, 13)), st.txt, box_w * 0.86f);
    ops.push_back(Op::image(t, std::round(cx - float(a.img(t).w) / 2.0f), std::round(cy - float(a.img(t).h) / 2.0f), float(a.img(t).w), float(a.img(t).h), Color{255, 255, 255, al}));
  }
}

/// "Continue playing": the screenshot with its corners, on the job thread.
std::string zen_shot(const Grid& g, const RecentItem& it) {
  char fl[48];
  std::snprintf(fl, sizeof fl, "cr%dm%lld", g.P(8), static_cast<long long>(it.mtime));  // the time it was written is part of the key (the picture of a state saved again)
  return baked(it.shot, fl);
}

/// The tiles of the row: the first three that did not fail to decode.
std::vector<size_t> zen_tiles(const Grid& g, const SystemItem& cur, const RecentRow& r) {
  std::vector<size_t> out;
  if (cur.games == kCountPending) return out;
  for (size_t i = 0; i < r.items.size() && out.size() < 3; ++i)
    if (!covers::failed(zen_shot(g, r.items[i])) || g_audit) out.push_back(i);
  return out;
}

void zen_recent_row(const Theme& th, Assets& a, const Grid& g, const RecentRow& row, const std::vector<size_t>& tiles, float x0, float tile_w, float step, float y, float tile_h, float lab_h, float cap_h, std::vector<Op>& ops) {
  const ui::Style st = ui::Style::from(th);
  const int lf = g.F(14, 12), cf = g.F(13, 11);
  // label band, a cell, the tiles, a cell, the caption band: every edge on a line of the cells
  text(a, ops, i18n::sentence_case(tr("CONTINUE PLAYING")), x0, y + lab_h / 2.0f, lf, st.dim, tile_w * 2.0f);
  const float ty = y + lab_h + float(g.u);
  for (size_t k = 0; k < tiles.size(); ++k) {
    const RecentItem& it = row.items[tiles[k]];
    const float x = x0 + float(k) * step;
    const bool foc = int(tiles[k]) == row.focus && row.focus_a > 0.0f;
    if (foc) {
      const float t = float(std::max(2, g.P(3)));
      g_shapes.rrect(a, ops, x - t, ty - t, tile_w + 2 * t, tile_h + 2 * t, g.P(8) + int(t), alpha(st.cyan, uint8_t(255 * row.focus_a)));
    }
    g_shapes.rrect(a, ops, x, ty, tile_w, tile_h, g.P(8), st.dark ? Color{0x1D, 0x20, 0x26, 255} : Color{255, 255, 255, 255});
    ImgId id;
    if (exists_once(it.shot) && pic(a, zen_shot(g, it), tile_w, tile_h, !row.decode_inline, id)) {
      float ow, oh;
      fit_size(a.img(id), tile_w, tile_h, ow, oh);
      ops.push_back(Op::image(id, std::round(x + (tile_w - ow) / 2.0f), std::round(ty + (tile_h - oh) / 2.0f), ow, oh));
    }
    rec(x, ty, tile_w, tile_h, "miniatura", 'L', true, true);
    // the name, and under it when it was played and on which console (cut with an ellipsis where the tile ends)
    const float name_h = float(g.U(line_cells(a, g, cf)));
    const float cap_top = ty + tile_h + float(g.u);
    text(a, ops, short_title(it.name), x, cap_top + name_h / 2.0f, cf, foc ? st.accent() : st.txt, tile_w);
    if (!it.ago.empty()) text(a, ops, it.ago, x, cap_top + name_h + (cap_h - name_h) / 2.0f, g.F(12, 10), st.dim, tile_w);
  }
}

void zen_system(const Theme& th, Assets& a, const Grid& g, const std::vector<SystemItem>& items, int cursor, const SystemItem& cur, float cam, const Status& status, RecentRow* recent, const InfoFade& fade, std::vector<Op>& ops) {
  (void)cursor;
  const ui::Style st = ui::Style::from(th);
  zen_back(th, a, g, system_color(cur.theme_name, st), ops);
  zen_avatar(th, a, g, ops);
  status_bar(th, a, g.w, g.h, status, ops);
  help_bar(th, a, g.w, g.h, recent && recent->focus_help ? recent_help() : system_help(), ops);
  const int n = int(items.size());
  const float top = float(g.content_top()), bottom = float(g.help_top());
  const std::vector<size_t> tiles = recent ? zen_tiles(g, cur, *recent) : std::vector<size_t>();
  const int name_px = g.F(28, 18), count_px = g.F(14, 12);
  const int lf = g.F(14, 12), cf = g.F(13, 11);
  // cards across the screen, the name and the count on the margins, the screenshots on four columns each.
  // The row keeps its room with or without games to resume, so the cards do not jump from one system to the next.
  // Every height is a whole number of cells: the cards, the tiles and the bands of the texts sit on the lines.
  const float sel_w = g.span(6), side_w = g.span(3);
  const float info_h = float(g.U(line_cells(a, g, name_px)));
  const float lab_h = float(g.U(line_cells(a, g, lf))), cap_h = float(g.U(line_cells(a, g, cf) + line_cells(a, g, g.F(12, 10))));  // name + 12 min ago - GBA
  const float tile_w = g.span(4);
  const float fixed = lab_h + 2.0f * float(g.u) + cap_h;  // the row without its tiles
  float tile_h = float(g.U(std::max(1, int(tile_w * 0.75f) / g.u)));
  float rh = fixed + tile_h;
  const float min_hero = float(g.snap(float(g.P(100))));
  if (bottom - top - info_h - rh - g.gutter() < min_hero) {  // short screens: smaller tiles first
    tile_h = float(std::max(g.snap(float(g.P(44))), g.U(std::max(1, int(std::max(0.0f, bottom - top - info_h - g.gutter() - min_hero - fixed)) / g.u))));
    rh = fixed + tile_h;
  }
  const float avail = bottom - top - info_h - rh - 2.0f * g.gutter();
  const float sel_h = float(g.U(std::max(1, int(std::max(0.0f, std::min(sel_w * 0.72f, avail))) / g.u)));
  const float side_h = float(g.snap(sel_h * 0.66f));
  const float room = std::clamp(g_state.row_room, 0.0f, 1.0f);
  const float hero_with = top + float(g.U(int(std::max(0.0f, avail - sel_h) / 2.0f) / g.u));
  const float hero_alone = top + float(g.U(int(std::max(0.0f, bottom - top - sel_h - g.gutter() - info_h) / 2.0f) / g.u));
  const float hero_y = std::round(hero_alone + (hero_with - hero_alone) * room);
  const float cy = hero_y + sel_h / 2.0f, ccx = g.w / 2.0f, first_d = sel_w / 2.0f + g.gutter() + side_w / 2.0f;
  auto place = [&](int i, bool chosen) {
    if (n == 1 && !chosen) return;
    const float dist = float(i) - cam, d = std::fabs(dist), t = std::clamp(1.0f - d, 0.0f, 1.0f), sg = dist < 0 ? -1.0f : 1.0f;
    const float x = ccx + sg * (d < 1.0f ? d * first_d : first_d + (d - 1.0f) * (side_w + g.gutter()));
    const float cw = side_w + (sel_w - side_w) * t, chh = side_h + (sel_h - side_h) * t;
    if (x + cw / 2.0f < 0 || x - cw / 2.0f > g.w) return;
    zen_card(th, a, g, items[size_t(rem_e(i, n))], x, cy, cw, chh, t, side_w, side_h, sel_w, sel_h, ops);
  };
  const int c0 = int(std::round(cam));
  for (int i = c0 - 4; i <= c0 + 4; ++i)
    if (i != c0) place(i, false);
  place(c0, true);
  if (at_rest(cam)) {  // on the job thread, one by one: the next logos are ready before the move
    for (int dd = 1; dd <= 5; ++dd)
      for (int sg : {1, -1}) {
        const std::string np = th.path("./assets/logos/" + items[size_t(rem_e(c0 + sg * dd, n))].theme_name + ".webp");
        if (!exists_once(np)) continue;
        ImgId nid;
        pic(a, np, side_w * 0.76f, side_h * 0.58f, true, nid);
        if (dd == 1) pic(a, np, sel_w * 0.76f, sel_h * 0.58f, true, nid);
      }
  }
  rec(ccx - sel_w / 2.0f, hero_y, sel_w, sel_h, "ficha", 'B', true, true);
  if (cur.games == kCountPending && cur.sys != kItemAddGames) return;
  const size_t info = ops.size();
  const float ny = hero_y + sel_h + g.gutter() + info_h / 2.0f;
  const std::string count = cur.sys == kItemAddGames ? addgames::card_hint() : zen_count(cur.theme_name, cur.games);  // the card: how games get here
  const float cw = a.measure(count, float(count_px));
  text(a, ops, cur.full_name, g.col(0), ny, name_px, st.txt, g.w - 2.0f * float(g.margin()) - cw - g.gutter());
  text(a, ops, count, g.w - float(g.margin()), ny, count_px, st.dim, 0, R);
  if (!tiles.empty()) {
    const size_t row_from = ops.size();
    zen_recent_row(th, a, g, *recent, tiles, g.col(0), tile_w, g.col(4) - g.col(0), bottom - rh, tile_h, lab_h, cap_h, ops);
    // it comes in with the room the card makes: from a third of the way it fades in and rises the last two cells
    const float k = std::clamp((room - 0.33f) / 0.67f, 0.0f, 1.0f), in = k * k * (3.0f - 2.0f * k);
    if (in < 1.0f) {
      const float dy = std::round((1.0f - in) * float(g.U(2)));
      for (size_t i = row_from; i < ops.size(); ++i) ops[i].y += dy;
      motion::fade(ops, row_from, in);
    }
  }
  motion::fade(ops, info, fade.alpha);
}

void zen_list(const Theme& th, Assets& a, const Grid& g, const std::string& system_name, const std::vector<std::string>& names, size_t cursor, const Status& status, const DetailedData* d, std::vector<Op>& ops) {
  const ui::Style st = ui::Style::from(th);
  // the colour of the chosen game's cover, as the home screen takes the colour of the system; while a cover is not ready (scrolling
  // fast, still decoding) the last one stays, so the page does not flash back to the system's colour
  static std::string tint_sys;
  static Color tint_game{};
  if (tint_sys != g_state.system_theme) tint_sys = g_state.system_theme, tint_game = Color{};
  if (d && !d->image.empty() && exists_once(d->image)) {
    ImgId am;
    Color c;
    if (ambient_of(a, d->image, am, c)) tint_game = c;
  }
  const Color tint = tint_game[3] ? tint_game : system_color(g_state.system_theme, st);
  zen_back(th, a, g, tint, ops, tint_game[3] ? 0.8f : 0.55f);  // a list is for reading; a game's colour, darker than a system's, a little stronger
  status_bar(th, a, g.w, g.h, status, ops);
  help_bar(th, a, g.w, g.h, list_help(), ops);
  const float top = float(g.content_top()), bottom = float(g.help_top());
  // the title in the bar's four cells; a line taller than them starts on the top margin instead of poking out above it
  const int title_px = g.F(20, 15);
  std::string cased_sys;
  const std::string_view shown_sys = list_cased(th, system_name, cased_sys);
  const float title_h = float(a.img(a.text(shown_sys, float(title_px), st.txt)).h);
  text(a, ops, shown_sys, g.col(0), std::max(float(g.status_y()), float(g.top()) + title_h / 2.0f), title_px, st.txt, g.span(7));
  const int list_cols = 7;
  const float lw = g.span(list_cols);
  const int fs = g.F(16, 13);
  const float pitch = float(g.snap(float(std::max(g.P(33), fs + g.P(11)))));  // a row: whole cells
  const float inset = float(g.U(2));
  const size_t rows = size_t(std::max(1.0f, (bottom - top) / pitch));
  {
    const RowWindow rw = row_window(names.size(), cursor, rows);
    // the chosen row is a card on the lines of the cells (at rest: while it slides it is on none)
    if (!names.empty() && !rw.moving) rec(g.col(0), top + rw.sel * pitch, lw, pitch, "tarjeta", 'L', true, true);
    const Free in_rows;
    if (rw.moving) ops.push_back(Op::clip(0, top, g.w, float(rows) * pitch));
    if (!names.empty()) {  // framed like the chosen card of the home screen and the chosen row of the menus
      const float sy = top + rw.sel * pitch;
      if (st.sel_style == ui::SelStyle::Fill) {
        g_shapes.rrect(a, ops, g.col(0), sy, lw, pitch, g.P(10), st.cyan_soft);
        g_shapes.ring(a, ops, g.col(0), sy, lw, pitch, g.P(10), std::max(2.0f, float(g.P(2))), st.accent());
      } else {  // SELECTOR STYLE chosen by the user: the shape over the same row, with the card's corner and stroke
        g_shapes.selector(a, st, ops, g.col(0), sy, lw, pitch, st.cyan_soft, st.accent(), g.P(10), int(std::max(2.0f, float(g.P(2)))));
      }
    }
    std::string cased_row;
    for (size_t k = rw.first; k < rw.end; ++k) {
      const float cy = top + (float(k) - rw.start) * pitch + pitch / 2.0f;
      text(a, ops, list_cased(th, names[k], cased_row), g.col(0) + inset, cy, fs, k == rw.lit ? st.sel_ink : st.txt, lw - 2.0f * inset);
    }
    if (rw.moving) ops.push_back(Op::unclip());
  }
  if (!d) return;
  const float rx = g.col(list_cols), rw = g.w - float(g.margin()) - rx, ry = top;
  const bool wants_desc = has_data(*d);
  float cw, ch;
  box43(rw, (bottom - ry) * (wants_desc ? 0.5f : 0.62f), cw, ch);
  ch = float(g.U(std::max(1, int(ch) / g.u)));  // the cover's box on the cells
  cw = std::min(rw, float(g.U(std::max(1, int(ch / 0.75f) / g.u))));
  const std::string logo = th.path("./assets/logos/" + g_state.system_theme + ".webp");
  video::box_offer(rx, ry, rw, ch);  // where the list preview plays
  // the whole cover, whatever its shape: a portrait box must not lose its top and bottom
  ImgId cov;
  if (!vpreview::hides(d->video, rx, ry, rw, ch) && !d->image.empty() && exists_once(d->image) && pic(a, d->image, rw, ch, true, cov)) {
    rec(rx, ry, rw, ch, "caratula", 'L', false, true);
    float ow, oh;
    fit_size(a.img(cov), rw, ch, ow, oh);
    const float ox = std::round(rx + (rw - ow) / 2.0f), oy = std::round(ry + (ch - oh) / 2.0f);
    soft(a, ops, ox - 1.0f, oy + 2.0f, ow + 2.0f, oh + 2.0f, float(g.P(10)), Color{0, 0, 0, uint8_t(st.dark ? 90 : 36)});
    ops.push_back(Op::image(cov, ox, oy, ow, oh));
  } else {
    cover_box(a, ops, std::string(), rx + float(g.U(int(rw - cw) / 2 / g.u)), ry, cw, ch, g.P(10), logo, st.dark ? Color{255, 255, 255, 14} : Color{255, 255, 255, 220}, Color{0, 0, 0, 0}, Color{0, 0, 0, 0},
              d->video);
  }
  const int tp = g.F(19, 14), mp = g.F(13, 11);
  float y = ry + ch + float(g.snap(float(g.P(10)))) + float(tp) / 2.0f;
  std::string cased_name;
  text(a, ops, list_cased(th, d->name, cased_name), rx, y, tp, st.txt, rw);
  y += float(tp) / 2.0f + float(g.P(4));
  std::string meta;
  char yr[16] = "";
  if (d->year >= 0) std::snprintf(yr, sizeof yr, "%d", int(d->year));
  for (std::string_view s : {std::string_view(yr), trim(d->genre), trim(d->players)}) {
    if (s.empty()) continue;
    if (!meta.empty()) meta += " \xC2\xB7 ";
    meta += std::string(s);
  }
  if (!meta.empty()) {
    y += float(mp) / 2.0f;
    text(a, ops, meta, rx, y, mp, st.dim, rw);
    y += float(mp) / 2.0f + float(g.P(4));
  }
  if (d->show_rating && y + float(g.P(14)) <= bottom) {
    stars(th, a, ops, rx, y + float(g.P(7)), g.P(13), d->rating, st.accent(), alpha(st.dim, 160), g.P(3));
    y += float(g.P(14)) + float(g.P(6));
  }
  if (wants_desc && !d->desc.empty() && bottom - y > float(g.F(14, 12))) desc_block(a, ops, *d, rx, y, rw, bottom - y, g.F(14, 12), st.dim);
}

}  // namespace

std::string zen_count(const std::string& theme_name, uint32_t games) {
  const bool collection = theme_name.rfind("auto-", 0) == 0 || theme_name.rfind("custom-", 0) == 0;
  return system_icon_label(theme_name) && !collection ? std::to_string(games) : games_low(games);  // tools and pictures are not games
}

DrawState& state() { return g_state; }

bool exists_cached(const std::string& path) { return exists_once(path); }
size_t exists_cache_size() { return exists_table().size(); }

std::string cover_key(const std::string& image, int radius) {
  char fl[16];
  std::snprintf(fl, sizeof fl, "r%d", radius);  // no `c`: the whole cover, whatever its shape
  return baked(image, fl);
}

void xmb_span(int n, int mid, int& first, int& last) {
  const int before = std::min(1, (n - 1) / 3), after = std::max(0, std::min(8, n - 1 - before));  // few systems: none of them twice
  first = mid - before;
  last = std::min(mid + after + 1, first + std::max(n, 1) - 1);
}

RowWindow row_window(size_t n, size_t cursor, size_t rows) {
  RowWindow r;
  g_state.rows_shown = rows;  // the list that asks for a window draws that many rows
  if (n == 0 || rows == 0) return r;
  const float c = g_state.list_cam >= 0.0f ? std::clamp(g_state.list_cam, 0.0f, float(n - 1)) : float(cursor);
  r.start = n > rows ? std::clamp(c - float(rows / 2), 0.0f, float(n - rows)) : 0.0f;
  r.first = size_t(std::floor(r.start));
  r.end = std::min(n, size_t(std::ceil(r.start)) + rows);
  r.sel = c - r.start;
  r.lit = std::min(n - 1, size_t(std::lround(c)));
  r.moving = c != float(cursor);
  return r;
}

Kind kind_of(const Theme& th) {
  const std::string_view s = th.str("esStyle", "");
  if (s.empty()) return th.str("zenStyle", "") == "next" ? Kind::Zen : Kind::None;
  if (s == "xmb") return Kind::Xmb;
  if (s == "artbook") return Kind::Art;
  if (s == "iconic") return Kind::Iconic;
  if (s == "techdweeb") return Kind::Tech;
  return Kind::None;
}

std::vector<size_t> recent_tiles(const Theme& th, float w, float h, const SystemItem& cur, const RecentRow& r) {
  if (kind_of(th) == Kind::Zen) return zen_tiles(Grid(w, h), cur, r);
  std::vector<size_t> out;
  if (kind_of(th) != Kind::Xmb || r.items.empty() || cur.games == kCountPending) return out;
  const Grid g(w, h);
  const XRow x = xmb_row(g);
  for (size_t i = 0; i < r.items.size() && int(out.size()) < x.fit; ++i)
    if (!covers::failed(r.items[i].shot_key())) out.push_back(i);
  return out;
}

static void grid_overlay(const Grid& g, std::vector<Op>& ops);

void system_view(const Theme& th, Assets& a, float w, float h, const std::vector<SystemItem>& items, int cursor_abs, float cam, const Status& status, RecentRow* recent, const InfoFade& fade, std::vector<Op>& ops) {
  const Grid g(w, h);
  if (recent) recent->box_w = recent->box_h = 0;
  if (items.empty()) {
    ops.push_back(Op::rect(0, 0, w, h, th.color("bg")));
    status_bar(th, a, w, h, status, ops);
    help_bar(th, a, w, h, system_help(), ops);
    return;
  }
  const int n = int(items.size());
  const int cursor = rem_e(cursor_abs, n);
  const SystemItem& cur = items[size_t(fade.item >= 0 && fade.item < n ? fade.item : cursor)];
  switch (kind_of(th)) {
    case Kind::Xmb: xmb_system(th, a, g, items, cursor, cur, cam, status, recent, fade, ops); break;
    case Kind::Art: art_system(th, a, g, items, cur, cam, status, fade, ops); break;
    case Kind::Iconic: iconic_system(th, a, g, items, cursor, cur, cam, status, fade, ops); break;
    case Kind::Tech: tech_system(th, a, g, items, cur, cam, status, fade, ops); break;
    case Kind::Zen: zen_system(th, a, g, items, cursor, cur, cam, status, recent, fade, ops); break;
    case Kind::None: break;
  }
  grid_overlay(g, ops);
}

void list_view(const Theme& th, Assets& a, float w, float h, const std::string& system_name, const std::vector<std::string>& names, size_t cursor, const Status& status, const DetailedData* detail, std::vector<Op>& ops) {
  const Grid g(w, h);
  if (detail) detail->cover_missing = false;
  switch (kind_of(th)) {
    case Kind::Xmb: xmb_list(th, a, g, system_name, names, cursor, status, detail, ops); break;
    case Kind::Art: art_list(th, a, g, names, cursor, status, detail, ops); break;
    case Kind::Iconic: iconic_list(th, a, g, names, cursor, status, detail, ops); break;
    case Kind::Tech: tech_list(th, a, g, names, cursor, status, detail, ops); break;
    case Kind::Zen: zen_list(th, a, g, system_name, names, cursor, status, detail, ops); break;
    case Kind::None: break;
  }
  grid_overlay(g, ops);
}

bool ink_rows(const Img& im, int& t, int& b) {
  t = b = -1;
  if (im.rgba.size() < size_t(im.w) * im.h * 4) return false;  // released by the renderer or evicted: nothing to measure
  for (uint32_t y = 0; y < im.h; ++y)
    for (uint32_t x = 0; x < im.w; ++x)
      if (im.rgba[(size_t(y) * im.w + x) * 4 + 3] > 127) {
        if (t < 0) t = int(y);
        b = int(y);
        break;
      }
  return t >= 0;
}
bool text_ink(Assets& a, std::string_view s, int px, int& t, int& b) { return ink_rows(text_px(a, s, px), t, b); }
InkLine ink_line(Assets& a, std::string_view s, int px, float cy) {
  const Img& im = text_px(a, s.empty() ? std::string_view("0") : s, px);
  int t, b;
  if (!ink_rows(im, t, b)) t = 0, b = int(im.h) - 1;
  return {std::round(cy - float(im.h) / 2.0f) + float(t), float(b - t + 1)};
}
int px_for_ink(Assets& a, std::string_view s, int h, int px) {
  int best = px, err = 1 << 20;
  for (int i = 0; i < 12 && px > 1; ++i) {
    int t, b;
    if (!text_ink(a, s, px, t, b)) break;
    const int e = (b - t + 1) - h;
    if (std::abs(e) < err) err = std::abs(e), best = px;
    if (e == 0) break;
    px -= e > 0 ? 1 : -1;
  }
  return best;
}
void status_bolt(Assets& a, std::vector<Op>& ops, float cx, float cy, float h, Color c, Color ring) { put_bolt(a, ops, cx, cy, h, c, ring); }
std::vector<Box>*& audit_sink() { return g_audit; }
std::vector<size_t>*& status_bg_sink() { return g_status_bg; }
void audit(float x, float y, float w, float h, std::string tag, char anchor, bool grid, bool cell) { rec(x, y, w, h, std::move(tag), anchor, grid, cell); }

/// ZF_GRID=1: the margins and the columns over the screen, to check a layout by eye too.
static void grid_overlay(const Grid& g, std::vector<Op>& ops) {
  static const bool on = std::getenv("ZF_GRID") != nullptr;
  if (!on) return;
  const float m = float(g.margin());
  for (int i = 0; i < 12; ++i) ops.push_back(Op::rect(g.col(i), 0, g.colw(), g.h, Color{255, 40, 90, 34}));
  const Color line{255, 40, 90, 200};
  ops.push_back(Op::rect(0, m, g.w, 1, line));
  ops.push_back(Op::rect(0, g.h - m, g.w, 1, line));
  ops.push_back(Op::rect(m, 0, 1, g.h, line));
  ops.push_back(Op::rect(g.w - m, 0, 1, g.h, line));
  ops.push_back(Op::rect(0, float(g.content_top()), g.w, 1, Color{60, 200, 255, 200}));
  ops.push_back(Op::rect(0, float(g.help_top()), g.w, 1, Color{60, 200, 255, 200}));
}

void status(const Theme& th, Assets& a, float w, float h, const Status& st, std::vector<Op>& ops) {
  const Grid g(w, h);
  const size_t from = ops.size();
  g_audit_quiet = true;
  switch (kind_of(th)) {
    case Kind::Xmb: status_xmb(a, g, st, ops); break;
    case Kind::Art: status_art(a, g, st, ops); break;
    case Kind::Iconic:
      if (th.str("esCarousel", "") == "aurora") status_aurora(th, a, g, st, ops);
      else if (th.str("esCarousel", "") == "swiss") status_swiss(th, a, g, st, ops);
      else if (th.str("esCarousel", "") == "industrial") status_industrial(th, a, g, st, ops);
      else status_iconic(th, a, g, st, ops);
      break;
    case Kind::Tech: status_tech(a, g, st, ops); break;
    case Kind::Zen: status_zen(th, a, g, st, ops); break;
    default: break;
  }
  g_audit_quiet = false;
  if (g_audit && ops.size() > from) {
    float l = w, r = 0, t = h, b = 0;
    for (size_t i = from; i < ops.size(); ++i) l = std::min(l, ops[i].x), r = std::max(r, ops[i].x + ops[i].w), t = std::min(t, ops[i].y), b = std::max(b, ops[i].y + ops[i].h);
    rec(l, t, r - l, b - t, "barra", 'R', false);
  }
}

void menu_backdrop(const Theme& th, Assets& a, float w, float h, std::vector<Op>& ops) {
  const Grid g(w, h);
  switch (kind_of(th)) {
    case Kind::Xmb: xmb_back(th, a, g, ops); break;
    case Kind::Iconic: {
      const std::string_view car = th.str("esCarousel", "");
      const bool dk = iconic_ink(th).dark;
      if (car == "aurora") {  // the light of the carousel, quieter: three soft blobs on the page
        ops.push_back(Op::rect(0, 0, g.w, g.h, th.color("bg")));
        disc(a, ops, g.w * 0.10f, g.h * 0.86f, g.w * 0.50f, alpha(dk ? rgb(0x2EE6C8) : rgb(0x7FE8D6), dk ? 190 : 230));
        disc(a, ops, g.w * 0.82f, g.h * 0.08f, g.w * 0.44f, alpha(dk ? rgb(0xFF5FA8) : rgb(0xFFB3D1), dk ? 190 : 235));
        disc(a, ops, g.w * 0.50f, g.h * 0.40f, g.w * 0.40f, alpha(dk ? rgb(0x7A5CFF) : rgb(0xB9A8FF), dk ? 170 : 200));
        ops.push_back(Op::rect(0, 0, g.w, g.h, alpha(th.color("bg"), dk ? 50 : 70)));
      } else if (car == "swiss") {  // flat colour, nothing else
        ops.push_back(Op::rect(0, 0, g.w, g.h, th.color("bg")));
      } else if (car == "industrial") {  // the panel: a darker lip at the top and a thin orange line under it
        ops.push_back(Op::rect(0, 0, g.w, g.h, th.color("bg")));
        ops.push_back(Op::rect(0, 0, g.w, float(g.top()) * 0.5f, dk ? rgb(0x0C0B0A) : rgb(0xCFCDC4)));
        ops.push_back(Op::rect(0, float(g.top()) * 0.5f, g.w, std::max(1.0f, float(g.P(3))), rgb(0xFF5A1F)));
      } else {
        iconic_page(th, a, g, th.color("cyan"), ops);
      }
      break;
    }
    case Kind::Tech: tech_scene(th, a, g, 34, ops); break;
    case Kind::Art: art_menu_back(th, a, g, ops); break;
    case Kind::Zen: zen_back(th, a, g, ui::Style::from(th).cyan, ops, 0.45f); break;  // always the same: a menu is not of a system
    default: break;
  }
}

int menu_skin(const Theme& th) {
  if (kind_of(th) != Kind::Iconic) return 0;
  const std::string_view car = th.str("esCarousel", "");
  return car == "aurora" ? 1 : car == "swiss" ? 2 : car == "industrial" ? 3 : 0;
}

void menu_help(const Theme& th, Assets& a, float w, float h, const std::vector<std::pair<const char*, const char*>>& prompts, std::vector<Op>& ops) {
  // A frame at rest allocates nothing: the ops of the row are made once per theme, size, text scale and prompts, and copied after that.
  static struct {
    const Theme* th = nullptr;
    const Assets* as = nullptr;
    float w = 0, h = 0, scale = 0;
    std::vector<std::pair<std::string, std::string>> prompts;
    std::vector<Op> ops;
    bool valid = false;
  } cache;
  const float scale = std::max(ui::access().scale(), 1.0f);
  bool same = cache.valid && cache.th == &th && cache.as == &a && cache.w == w && cache.h == h && cache.scale == scale && cache.prompts.size() == prompts.size();
  for (size_t i = 0; same && i < prompts.size(); ++i) same = cache.prompts[i].first == prompts[i].first && cache.prompts[i].second == prompts[i].second;
  if (same && !g_audit) {
    ops.insert(ops.end(), cache.ops.begin(), cache.ops.end());
    return;
  }
  const size_t from = ops.size();
  const Grid g(w, h);
  const ui::Style st = ui::Style::from(th);
  HelpLook hl;
  hl.fg = st.dim;
  hl.accent = st.dark ? st.cyan : st.cyan_dark;
  hl.scale = scale;
  hl.cy = float(g.help_y());  // help_h() already carries the text size
  hl.right = g.w - float(g.margin());
  help(th, a, g, prompts, hl, ops);
  if (g_audit) return;  // the tests that measure it read the boxes it reports, every time
  cache.th = &th, cache.as = &a, cache.w = w, cache.h = h, cache.scale = scale;
  cache.prompts.clear();
  for (const auto& pr : prompts) cache.prompts.emplace_back(pr.first, pr.second);
  cache.ops.assign(ops.begin() + long(from), ops.end());
  cache.valid = true;
}

Color menu_icon_tile(const Theme& th, const std::string& icon) {
  if (kind_of(th) != Kind::Iconic) return Color{0, 0, 0, 0};
  static const struct {
    const char* name;
    uint32_t c;
  } kTiles[] = {{"games", 0xFF6B4A}, {"screen", 0x3A8DF5}, {"battery", 0x35B86B}, {"controller", 0x8A5CF0}, {"network", 0x1FB6C9}, {"library", 0xF0A020}, {"advanced", 0x6B7280}, {"quit", 0xE5484D}};
  for (const auto& t : kTiles)
    if (icon == t.name) return rgb(t.c);
  return rgb(0x8A8DA0);
}

}  // namespace zf::remake
