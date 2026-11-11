#include "i18n.hpp"
#include "ui_style.hpp"
#include "ui_prefs.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <map>

#include "remake.hpp"
#include "services.hpp"
#include "util.hpp"

namespace zf::ui {

Color mix(Color a, Color b, float t) {
  Color o = a;
  for (int i = 0; i < 3; ++i) o[size_t(i)] = uint8_t(std::lround(float(a[size_t(i)]) + (float(b[size_t(i)]) - float(a[size_t(i)])) * t));
  return o;
}

Color with_alpha(Color c, float k) {
  c[3] = uint8_t(std::lround(std::min(std::max(float(c[3]) * k, 0.0f), 255.0f)));
  return c;
}

namespace {

float luma(Color c) { return (0.2126f * float(c[0]) + 0.7152f * float(c[1]) + 0.0722f * float(c[2])) / 255.0f; }

/// Colour text of the theme: `RRGGBB[AA]` or `${variable}`.
Color resolve(const Theme& th, const char* text, Color def) {
  std::string_view v = trim(text ? text : "");
  if (v.empty()) return def;
  if (starts_with(v, "${") && ends_with(v, "}")) {
    v = th.str(v.substr(2, v.size() - 3));
    if (v.empty()) return def;
  }
  return parse_color(v);
}

float fv(const Theme& th, const char* name, float def) { return th.has(name) ? th.f(name) : def; }

}  // namespace

SelStyle parse_sel_style(std::string_view v) {
  if (v == "outline") return SelStyle::Outline;
  if (v == "underline") return SelStyle::Underline;
  if (v == "bar") return SelStyle::Bar;
  return SelStyle::Fill;
}

TextSize parse_text_size(std::string_view v) {
  if (v == "small") return TextSize::Small;
  if (v == "large") return TextSize::Large;
  if (v == "xlarge") return TextSize::XLarge;
  return TextSize::Normal;
}

float text_scale(TextSize s) {
  switch (s) {
    case TextSize::Small: return 0.9f;
    case TextSize::Large: return 1.15f;
    case TextSize::XLarge: return 1.3f;
    default: return 1.0f;
  }
}

Access& access() {
  static Access a;
  return a;
}

void set_access(TextSize size, bool contrast) {
  Access& a = access();
  if (a.size == size && a.contrast == contrast) return;
  a.size = size;
  a.contrast = contrast;
  ++a.gen;
}

namespace {

/// The high-contrast copy of a style: text pure white / black, every panel opaque, a selector fill without translucency.
Style high_contrast(Style s) {
  const Color white = {255, 255, 255, 255}, black = {0, 0, 0, 255};
  const Color pure = s.dark ? white : black;
  const Color bg = {s.bg[0], s.bg[1], s.bg[2], 255};
  s.bg = bg;
  s.txt = pure;
  s.dim = mix(bg, pure, 0.88f);
  s.dim_dark = s.dim;
  s.group_txt = pure;
  s.group_bg = mix(bg, pure, 0.14f);
  s.panel = mix(bg, pure, s.dark ? 0.10f : 0.06f);
  s.line = mix(s.panel, pure, 0.65f);
  s.cyan = s.dark ? Color{0x00, 0xE5, 0xFF, 255} : Color{0x00, 0x46, 0x8C, 255};
  s.cyan_dark = s.dark ? Color{0x00, 0xE5, 0xFF, 255} : Color{0x00, 0x32, 0x78, 255};
  s.cyan_soft = s.dark ? black : white;  // the selected row: opaque, with the thick ring of `Shapes::selector` around it
  s.sw_on = s.cyan;
  s.sw_off = mix(bg, pure, 0.60f);
  s.knob = white;
  s.track = mix(bg, pure, 0.50f);
  s.osd_bg = s.panel;
  s.osd_bar = s.cyan;
  s.osd_track = s.track;
  s.osd_txt = pure;
  s.key = mix(bg, pure, 0.16f);
  s.key_alt = mix(bg, pure, 0.26f);
  s.btn = mix(s.panel, pure, 0.22f);
  s.danger = s.dark ? Color{0xFF, 0x80, 0x80, 255} : Color{0xA8, 0x00, 0x14, 255};
  s.danger_soft = s.dark ? black : white;
  s.scrim = Color{0, 0, 0, uint8_t(s.dark ? 235 : 170)};
  return s;
}

Style base_style(const Theme& th);

}  // namespace

Style Style::from(const Theme& th) {
  const Style s = base_style(th);
  return access().contrast ? high_contrast(s) : s;
}

namespace {

Style base_style(const Theme& th) {
  // one parse per theme folder
  static std::map<std::string, Style> cache;
  auto hit = cache.find(th.dir());
  if (hit != cache.end()) return hit->second;

  Style s;
  s.corner = std::min(std::max(th.f("corner", 1.0f), 0.0f), 1.0f);
  s.sel_style = parse_sel_style(th.str("selStyle", "fill"));
  s.header_center = th.str("headerAlign", "left") == "center";
  const Color white = {255, 255, 255, 255};
  s.bg = th.has("bg") ? th.color("bg") : Color{0x26, 0x26, 0x2A, 255};
  s.txt = th.has("txt") ? th.color("txt") : Color{0xF0, 0xF0, 0xF2, 255};
  s.dim = th.has("dim") ? th.color("dim") : Color{0xBE, 0xBE, 0xC6, 255};
  s.cyan = th.has("cyan") ? th.color("cyan") : Color{0x00, 0xC3, 0xE3, 255};
  s.cyan_dark = th.has("cyanDark") ? th.color("cyanDark") : Color{0x4F, 0xD8, 0xF0, 255};
  s.cyan_soft = th.has("cyanSoft") ? th.color("cyanSoft") : Color{0x22, 0x52, 0x5F, 255};
  s.dim_dark = th.has("dimDark") ? th.color("dimDark") : s.dim;
  s.dark = luma(s.bg) < 0.5f;
  // derived defaults (fit the shipped dark and light themes)
  s.panel = mix(s.bg, white, s.dark ? 0.04f : 0.60f);
  s.line = mix(s.panel, s.txt, 0.09f);
  s.sw_on = mix(s.bg, s.cyan, 0.55f);
  s.sw_off = mix(s.bg, s.txt, 0.35f);
  s.track = mix(s.bg, s.txt, 0.30f);
  s.group_txt = mix(s.bg, s.txt, 0.60f);
  s.group_bg = with_alpha(s.txt, 0.05f);
  s.osd_bg = mix(s.bg, white, s.dark ? 0.08f : 1.0f);
  s.osd_bar = s.cyan;
  s.osd_track = s.track;
  s.osd_txt = s.txt;

  pugi::xml_document doc;
  if (doc.load_file(th.path("theme.xml").c_str())) {
    for (pugi::xml_node view : doc.child("theme").children("view")) {
      if (std::string_view(view.attribute("name").as_string()) != "menu") continue;
      auto pick = [&](const char* tag, const char* name, const char* field, Color& out) {
        for (pugi::xml_node n : view.children(tag)) {
          if (name && std::string_view(n.attribute("name").as_string()) != name) continue;
          pugi::xml_node f = n.child(field);
          if (f) out = resolve(th, f.child_value(), out);
          return;
        }
      };
      pick("menuBackground", "menubg", "color", s.panel);
      pick("menuText", "menutext", "separatorColor", s.line);
      pick("menuSwitch", nullptr, "colorOn", s.sw_on);
      pick("menuSwitch", nullptr, "colorOff", s.sw_off);
      pick("menuVolume", nullptr, "color", s.osd_bg);
      pick("menuVolume", nullptr, "barColor", s.osd_bar);
      pick("menuVolume", nullptr, "trackColor", s.osd_track);
      pick("menuVolume", nullptr, "textColor", s.osd_txt);
      pick("menuGroup", nullptr, "color", s.group_txt);
      pick("menuGroup", nullptr, "backgroundColor", s.group_bg);
    }
  }
  s.track = s.osd_track;
  s.knob = s.dark ? s.txt : white;
  s.key = mix(s.bg, s.txt, 0.10f);
  s.key_alt = mix(s.bg, s.txt, 0.18f);
  s.btn = mix(s.panel, s.txt, 0.12f);
  s.danger = s.dark ? Color{0xEE, 0x6B, 0x74, 255} : Color{0xC2, 0x34, 0x40, 255};
  s.danger_soft = mix(s.bg, s.danger, s.dark ? 0.28f : 0.15f);
  s.scrim = Color{0, 0, 0, uint8_t(s.dark ? 165 : 80)};
  cache.emplace(th.dir(), s);
  return s;
}

}  // namespace

Metrics Metrics::from(const Theme& th, float w, float h) {
  Metrics m;
  m.W = w;
  m.H = h;
  // Text size (accessibility): the text scales by `sc`; rows and the selector grow with it (`grow` = sc above 1) so the text keeps its room, and
  // the viewport shows fewer of them. Smaller text keeps the rows. At 1.0 every line below is the original arithmetic.
  const float sc = access().scale();
  const float grow = std::max(sc, 1.0f);
  m.margin = std::round(fv(th, "mx", 0.03f) * w);
  m.em = px_floor(std::floor(fv(th, "fMenu", 0.0375f) * h * sc));
  m.title_px = px_floor(std::floor(fv(th, "fMenuTitle", 0.0383f) * h * kTitleScale * sc));
  m.small_px = px_floor(std::floor(fv(th, "fMenuSmall", 0.0302f) * h * sc));
  m.foot_px = px_floor(std::floor(fv(th, "fMenuFoot", 0.0271f) * h * sc));
  m.col_w = std::round(std::min(w - 2.0f * m.margin, kColMaxH * h));
  m.col_x = std::round((w - m.col_w) / 2.0f);
  int lines = int(fv(th, "listLines", 9));
  if (lines < 3) lines = 9;
  const int theme_lines = lines;
  m.list_top = std::round(fv(th, "listPosY", 0.1375f) * h);
  m.row_h = std::max(std::floor(fv(th, "listH", 0.7125f) * h / float(lines)), 1.0f);  // a theme with listH 0: no division by a zero row below
  if (grow > 1.0f) {
    const float viewport = m.row_h * float(lines);
    m.row_h = std::round(m.row_h * grow);
    lines = std::max(int(viewport / m.row_h), 1);
  }
  m.list_h = m.row_h * float(lines);
  m.pill_h = std::round(fv(th, "selH", 0.0625f) * h * grow);
  if (m.pill_h > m.row_h) m.pill_h = m.row_h;
  if (int(m.row_h - m.pill_h) % 2 != 0) m.pill_h = std::min(m.pill_h + 1.0f, m.row_h);
  m.header_cy = std::round(fv(th, "avY", 0.0667f) * h);
  m.help_y = std::round(fv(th, "helpY", 0.948f) * h);
  m.lines = lines;
  m.title_left = m.col_x + std::round(m.pill_h * kPillInset);
  m.inset = std::round(m.pill_h * kPillInset);
  m.hint_y = std::round(m.list_top + m.list_h + (m.help_y - m.list_top - m.list_h) * 0.30f);
  {  // every theme on the grid of the views: same margins, status bar and help, so nothing jumps from one screen to the next
    const remake::Grid g(w, h);
    m.grid = true;
    m.u = g.u;
    m.inset = float(g.U(2));
    m.margin = m.col_x = float(g.margin());
    m.col_w = w - 2.0f * m.margin;
    m.title_left = m.col_x;
    m.header_cy = float(g.status_y());
    // the help row grows with the text (remake::menu_help draws it with the same factor), its bottom stays on the margin
    const int help_h = int(std::lround(float(g.help_h()) * grow));
    // a theme zfront does not draw keeps its own help pill (Zenith Minimal): about two of its text lines tall, on whole cells
    // (smaller text keeps it: only a larger one grows it, like the rows)
    auto pill_cells = [&](float k) { return g.snap(2.06f * std::floor(fv(th, "fHelp", 0.025f) * h * k), 3); };
    const int help_h_cells = remake::active(th) ? g.snap(float(help_h), 3) : pill_cells(grow);
    const int help_top_n = g.bottom() - (remake::active(th) ? g.help_h() : pill_cells(1.0f)) - g.U(2);  // at the normal size
    m.help_h = float(help_h_cells);
    m.help_bottom = float(g.bottom());
    const int help_top = g.bottom() - help_h_cells - g.U(2);
    m.help_y = float(g.bottom() - help_h_cells / 2);
    m.list_top = float(g.content_top());
    if (remake::next(th)) m.list_top += float(g.U(1));  // Next: the card sits further from the title than the title from the edge
    // the title: at the normal size its line (1.25 em) fits the status bar's four cells; a larger text size makes it bigger and its band
    // grows by whole cells, the list going down with it
    const int title_fit = px_floor(std::floor(float(g.bar_h()) / 1.25f));
    const int title_base = std::min(px_floor(std::floor(fv(th, "fMenuTitle", 0.0383f) * h * kTitleScale)), title_fit);
    m.title_px = sc > 1.0f ? std::max(px_floor(std::round(float(title_base) * sc)), title_base + 1) : std::min(m.title_px, title_base);
    const int title_band = std::max(g.bar_h(), g.snap(float(m.title_px) * 1.25f, 4));
    m.header_cy = float(g.top() + title_band / 2);
    m.list_top += float(title_band - g.bar_h());
    const float hint_h = float(g.snap(float(m.small_px) * 1.5f, 2));
    const float avail = float(help_top) - hint_h - m.list_top;
    lines = theme_lines;
    // rows of whole cells: down to the cell under what the theme's count of lines leaves, never under four
    const float cell = float(g.u);
    auto rows_in = [&](float room) { return std::max(std::floor(room / float(lines) / cell) * cell, 4.0f * cell); };
    m.row_h = rows_in(avail);
    if (grow > 1.0f) {
      // what the list had at the normal size (title in the bar, help of three cells): its rows and its viewport, never exceeded
      const float hint_n = float(g.snap(float(px_floor(std::floor(fv(th, "fMenuSmall", 0.0302f) * h))) * 1.5f, 2));
      const float top_n = float(g.content_top()) + (remake::next(th) ? float(g.U(1)) : 0.0f);
      const float row_n = rows_in(float(help_top_n) - hint_n - top_n);
      const float viewport = row_n * float(lines);
      m.row_h = std::max(std::ceil(row_n * grow / cell) * cell, row_n + cell);  // at least a cell taller
      lines = std::max(int(std::min(viewport, avail) / m.row_h), 1);
    }
    while (lines > 1 && m.row_h * float(lines) > avail) --lines;
    m.lines = lines;
    m.list_h = m.row_h * float(lines);
    m.pill_h = m.row_h;  // the selector is the row: its edges on the lines of the cells
    m.hint_y = std::round(m.list_top + m.list_h + hint_h / 2.0f);
  }
  return m;
}

// ---- shapes ----------------------------------------------------------------------------------------------------------

namespace {

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

/// One quarter of a disc of radius r, anti-aliased. q: 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left.
Img make_corner(int r, int q) {
  Img im;
  im.w = im.h = uint32_t(r);
  im.rgba.assign(size_t(r) * size_t(r) * 4, 255);
  float cx = (q == 0 || q == 3) ? float(r) : 0.0f;
  float cy = (q == 0 || q == 1) ? float(r) : 0.0f;
  for (int j = 0; j < r; ++j)
    for (int i = 0; i < r; ++i) {
      float dx = float(i) + 0.5f - cx, dy = float(j) + 0.5f - cy;
      float cov = clamp01(float(r) - std::sqrt(dx * dx + dy * dy) + 0.5f);
      im.rgba[(size_t(j) * size_t(r) + size_t(i)) * 4 + 3] = uint8_t(std::lround(cov * 255.0f));
    }
  return im;
}

/// Same quarter as `make_corner` but only the band of thickness `t` along the arc (the ring of an outline).
Img make_ring_corner(int r, int t, int q) {
  Img im = make_corner(r, q);
  const float ri = float(r - t);
  float cx = (q == 0 || q == 3) ? float(r) : 0.0f;
  float cy = (q == 0 || q == 1) ? float(r) : 0.0f;
  for (int j = 0; j < r; ++j)
    for (int i = 0; i < r; ++i) {
      float dx = float(i) + 0.5f - cx, dy = float(j) + 0.5f - cy;
      float inner = ri > 0.0f ? clamp01(ri - std::sqrt(dx * dx + dy * dy) + 0.5f) : 0.0f;
      uint8_t& al = im.rgba[(size_t(j) * size_t(r) + size_t(i)) * 4 + 3];
      al = uint8_t(std::lround(float(al) * (1.0f - inner)));
    }
  return im;
}

float seg_dist(float x, float y, float ax, float ay, float bx, float by) {
  float vx = bx - ax, vy = by - ay, wx = x - ax, wy = y - ay;
  float t = clamp01((vx * wx + vy * wy) / (vx * vx + vy * vy + 1e-9f));
  float dx = x - (ax + t * vx), dy = y - (ay + t * vy);
  return std::sqrt(dx * dx + dy * dy);
}

struct Seg {
  float ax, ay, bx, by;  // unit square
};

/// White strokes of `thick` (fraction of the box) with round caps; plus an optional filled disc (cx, cy, radius).
Img make_strokes(int px, const Seg* s, int n, float thick, float disc_r) {
  Img im;
  im.w = im.h = uint32_t(px);
  im.rgba.assign(size_t(px) * size_t(px) * 4, 255);
  float half = std::max(thick * float(px), 1.4f) / 2.0f;
  for (int j = 0; j < px; ++j)
    for (int i = 0; i < px; ++i) {
      float x = float(i) + 0.5f, y = float(j) + 0.5f;
      float cov = 0.0f;
      for (int k = 0; k < n; ++k) {
        float d = seg_dist(x, y, s[k].ax * float(px), s[k].ay * float(px), s[k].bx * float(px), s[k].by * float(px));
        cov = std::max(cov, clamp01(half - d + 0.5f));
      }
      if (disc_r > 0.0f) {
        float dx = x - float(px) / 2.0f, dy = y - float(px) / 2.0f;
        cov = std::max(cov, clamp01(disc_r * float(px) - std::sqrt(dx * dx + dy * dy) + 0.5f));
      }
      im.rgba[(size_t(j) * size_t(px) + size_t(i)) * 4 + 3] = uint8_t(std::lround(cov * 255.0f));
    }
  return im;
}

Img make_icon(Icon k, int px) {
  switch (k) {
    case Icon::ChevronRight: {
      const Seg s[] = {{0.36f, 0.18f, 0.66f, 0.50f}, {0.66f, 0.50f, 0.36f, 0.82f}};
      return make_strokes(px, s, 2, 0.12f, 0);
    }
    case Icon::ChevronLeft: {
      const Seg s[] = {{0.64f, 0.18f, 0.34f, 0.50f}, {0.34f, 0.50f, 0.64f, 0.82f}};
      return make_strokes(px, s, 2, 0.12f, 0);
    }
    case Icon::Check: {
      const Seg s[] = {{0.16f, 0.54f, 0.40f, 0.78f}, {0.40f, 0.78f, 0.84f, 0.26f}};
      return make_strokes(px, s, 2, 0.13f, 0);
    }
    case Icon::Sun: {
      Seg s[8];
      for (int i = 0; i < 8; ++i) {
        float ang = float(i) * 0.7853982f;
        float c = std::cos(ang), sn = std::sin(ang);
        s[i] = {0.5f + c * 0.34f, 0.5f + sn * 0.34f, 0.5f + c * 0.45f, 0.5f + sn * 0.45f};
      }
      return make_strokes(px, s, 8, 0.075f, 0.20f);
    }
  }
  return Img();
}

}  // namespace

void Shapes::bind(Assets& a) {
  static const std::string kMarker = "ui:marker";
  ImgId id;
  if (owner_ == &a && a.find(kMarker, id) && id == marker_) return;
  rs_.clear();
  rrs_.clear();  // the ring corners too: their ids are of the old Assets
  is_.clear();
  owner_ = &a;
  if (!a.find(kMarker, id)) {
    Img im;
    im.w = im.h = 1;
    im.rgba = {0, 0, 0, 0};
    id = a.add(kMarker, std::move(im));
    a.pin(id);
  }
  marker_ = id;
}

const std::array<ImgId, 4>& Shapes::corners(Assets& a, int r) {
  for (const R& e : rs_)
    if (e.r == r) return e.q;
  R e;
  e.r = r;
  for (int q = 0; q < 4; ++q) {
    std::string key = "ui:c:" + std::to_string(r) + ":" + std::to_string(q);
    ImgId id;
    if (!a.find(key, id)) id = a.add(key, make_corner(r, q));
    a.pin(id);
    e.q[size_t(q)] = id;
  }
  rs_.push_back(e);
  return rs_.back().q;
}

const std::array<ImgId, 4>& Shapes::ring_corners(Assets& a, int r, int t) {
  for (const RR& e : rrs_)
    if (e.r == r && e.t == t) return e.q;
  RR e;
  e.r = r;
  e.t = t;
  for (int q = 0; q < 4; ++q) {
    std::string key = "ui:cr:" + std::to_string(r) + ":" + std::to_string(t) + ":" + std::to_string(q);
    ImgId id;
    if (!a.find(key, id)) id = a.add(key, make_ring_corner(r, t, q));
    a.pin(id);
    e.q[size_t(q)] = id;
  }
  rrs_.push_back(e);
  return rrs_.back().q;
}

void Shapes::rrect(Assets& a, std::vector<Op>& out, float x, float y, float w, float h, int r, Color c) {
  int x0 = int(std::lround(x)), y0 = int(std::lround(y)), x1 = int(std::lround(x + w)), y1 = int(std::lround(y + h));
  int ww = x1 - x0, hh = y1 - y0;
  if (ww <= 0 || hh <= 0 || c[3] == 0) return;
  r = std::min(r, std::min(ww, hh) / 2);
  if (r <= 0) {
    out.push_back(Op::rect(float(x0), float(y0), float(ww), float(hh), c));
    return;
  }
  bind(a);
  std::array<ImgId, 4> q = corners(a, r);
  if (ww - 2 * r > 0) out.push_back(Op::rect(float(x0 + r), float(y0), float(ww - 2 * r), float(hh), c));
  if (hh - 2 * r > 0) {
    out.push_back(Op::rect(float(x0), float(y0 + r), float(r), float(hh - 2 * r), c));
    out.push_back(Op::rect(float(x1 - r), float(y0 + r), float(r), float(hh - 2 * r), c));
  }
  float fr = float(r);
  out.push_back(Op::image(q[0], float(x0), float(y0), fr, fr, c));
  out.push_back(Op::image(q[1], float(x1 - r), float(y0), fr, fr, c));
  out.push_back(Op::image(q[2], float(x1 - r), float(y1 - r), fr, fr, c));
  out.push_back(Op::image(q[3], float(x0), float(y1 - r), fr, fr, c));
}

void Shapes::ring(Assets& a, std::vector<Op>& out, float x, float y, float w, float h, int r, float t, Color c) {
  int x0 = int(std::lround(x)), y0 = int(std::lround(y)), x1 = int(std::lround(x + w)), y1 = int(std::lround(y + h));
  int ww = x1 - x0, hh = y1 - y0;
  if (ww <= 0 || hh <= 0 || c[3] == 0) return;
  int ti = std::min(std::max(int(std::lround(t)), 1), std::min(ww, hh) / 2);
  if (ti < 1) {
    out.push_back(Op::rect(float(x0), float(y0), float(ww), float(hh), c));
    return;
  }
  r = std::min(r, std::min(ww, hh) / 2);
  if (r <= 0) {
    out.push_back(Op::rect(float(x0), float(y0), float(ww), float(ti), c));
    out.push_back(Op::rect(float(x0), float(y1 - ti), float(ww), float(ti), c));
    out.push_back(Op::rect(float(x0), float(y0 + ti), float(ti), float(hh - 2 * ti), c));
    out.push_back(Op::rect(float(x1 - ti), float(y0 + ti), float(ti), float(hh - 2 * ti), c));
    return;
  }
  bind(a);
  std::array<ImgId, 4> q = ring_corners(a, r, std::min(ti, r));
  const int te = std::min(ti, r);  // band along the straight sides: as thick as the one on the arcs
  if (ww - 2 * r > 0) {
    out.push_back(Op::rect(float(x0 + r), float(y0), float(ww - 2 * r), float(te), c));
    out.push_back(Op::rect(float(x0 + r), float(y1 - te), float(ww - 2 * r), float(te), c));
  }
  if (hh - 2 * r > 0) {
    out.push_back(Op::rect(float(x0), float(y0 + r), float(te), float(hh - 2 * r), c));
    out.push_back(Op::rect(float(x1 - te), float(y0 + r), float(te), float(hh - 2 * r), c));
  }
  float fr = float(r);
  out.push_back(Op::image(q[0], float(x0), float(y0), fr, fr, c));
  out.push_back(Op::image(q[1], float(x1 - r), float(y0), fr, fr, c));
  out.push_back(Op::image(q[2], float(x1 - r), float(y1 - r), fr, fr, c));
  out.push_back(Op::image(q[3], float(x0), float(y1 - r), fr, fr, c));
}

void Shapes::selector(Assets& a, const Style& st, std::vector<Op>& out, float x, float y, float w, float h, Color fill, Color accent) {
  const bool hc = access().contrast;
  const int weight = hc ? std::max(3, int(std::lround(h * 0.12f))) : std::max(2, int(std::lround(h * 0.07f)));  // ring / underline thickness (2 px at 30 px tall, 3 at 40)
  switch (st.sel_style) {
    case SelStyle::Fill:
      rrect(a, out, x, y, w, h, st.radius(h), fill);
      break;
    case SelStyle::Outline:
      ring(a, out, x, y, w, h, st.radius(h), float(weight), accent);
      break;
    case SelStyle::Underline:
      out.push_back(Op::rect(float(std::lround(x)), float(std::lround(y + h)) - float(weight), float(std::lround(x + w) - std::lround(x)), float(weight), accent));
      break;
    case SelStyle::Bar: {
      const int bw = std::max(3, int(std::lround(h * 0.14f)));
      rrect(a, out, x, y, float(bw), h, int(float(bw) / 2.0f * st.corner), accent);
      break;
    }
  }
  // high contrast: every style also gets a thick ring in the accent colour, so the selection never rests on a fill colour alone
  if (hc && st.sel_style != SelStyle::Outline) ring(a, out, x, y, w, h, st.radius(h), float(weight), accent);
}

void Shapes::framed(Assets& a, std::vector<Op>& out, float x, float y, float w, float h, int r, Color fill, Color border, float t) {
  rrect(a, out, x, y, w, h, r, border);
  int ti = std::max(int(std::lround(t)), 1);
  rrect(a, out, x + float(ti), y + float(ti), w - 2.0f * float(ti), h - 2.0f * float(ti), std::max(r - ti, 0), fill);
}

void Shapes::icon(Assets& a, std::vector<Op>& out, Icon k, float x, float y, int px, Color tint) {
  if (px < 4) return;
  bind(a);
  ImgId id = 0;
  bool found = false;
  for (const I& e : is_)
    if (e.k == k && e.px == px) {
      id = e.id;
      found = true;
      break;
    }
  if (!found) {
    std::string key = "ui:i:" + std::to_string(int(k)) + ":" + std::to_string(px);
    if (!a.find(key, id)) id = a.add(key, make_icon(k, px));
    a.pin(id);
    is_.push_back({k, px, id});
  }
  out.push_back(Op::image(id, float(std::lround(x)), float(std::lround(y)), float(px), float(px), tint));
}

// ---- text ------------------------------------------------------------------------------------------------------------

void Txt::set(Assets& a, std::string_view s, int px, float max_w) {
  ok = true;
  if (s.empty()) {
    blank = true;
    id = 0;
    w = h = 0;
    return;
  }
  blank = false;
  const Color white = {255, 255, 255, 255};
  id = max_w > 0 ? a.text_ellipsis(s, float(px), white, max_w) : a.text(s, float(px), white);
  w = float(a.img(id).w);
  h = float(a.img(id).h);
}

void Txt::set_wrap(Assets& a, std::string_view s, int px, float max_w, size_t max_lines, float spacing) {
  ok = true;
  if (s.empty()) {
    blank = true;
    id = 0;
    w = h = 0;
    return;
  }
  blank = false;
  id = a.text_wrap(s, float(px), Color{255, 255, 255, 255}, max_w, max_lines, spacing);
  w = float(a.img(id).w);
  h = float(a.img(id).h);
}

Color on_color(Color fill) { return luma(fill) > 0.5f ? Color{0x12, 0x15, 0x19, 255} : Color{0xFF, 0xFF, 0xFF, 255}; }

void HelpBar::draw(Assets& a, const Theme& th, const Metrics& m, const Style& st, const Prompt* p, size_t n, std::vector<Op>& out) {
  if (!ui_prefs::help_prompts()) return;  // ES ShowHelpPrompts (ON-SCREEN HELP) off
  if (m.grid && remake::active(th)) {  // the help row of the views of the theme, in the same place
    if (gkey_ != p || gn_ != n || ggen_ != i18n::generation()) {
      gkey_ = p, gn_ = n, ggen_ = i18n::generation();
      glabels_.clear();
      grow_.clear();
      for (size_t i = 0; i < n; ++i) glabels_.push_back(i18n::sentence_case(tr(p[i].text)));
      for (size_t i = 0; i < n; ++i) grow_.emplace_back(p[i].icon, glabels_[i].c_str());
    }
    remake::menu_help(th, a, m.W, m.H, grow_, out);
    return;
  }
  float px = std::floor(fv(th, "fHelp", 0.025f) * m.H * access().scale());
  bool stale = key_ != p || n_ != n || px_ != px;
  for (size_t i = 0; !stale && i < items_.size(); ++i)
    if (!items_[i].t.alive(a) || a.img(items_[i].icon).rgba.empty()) stale = true;
  if (stale) {
    items_.clear();
    key_ = p, n_ = n, px_ = px;
    for (size_t i = 0; i < n; ++i) {
      Item it;
      if (!a.file(th.path(std::string("./assets/help-") + p[i].icon + ".png"), it.icon)) continue;
      it.t.set(a, i18n::sentence_case(tr(p[i].text)), px_floor(px));
      items_.push_back(it);
    }
  }
  hi_.clear();
  for (size_t i = 0; i < items_.size(); ++i) {
    const Item& it = items_[i];
    if (it.t.blank || !it.t.ok) continue;
    hi_.push_back({it.icon, float(a.img(it.icon).w), float(a.img(it.icon).h), it.t.id, it.t.w, it.t.h, std::string(p[i].icon) == "A"});
  }
  help_pill(a, st, m.W, m.H, m.help_y, px, hi_, out, m.grid ? &m : nullptr);
}

namespace {
float help_pill_y0(float W, float H, float help_y, float ph) { return std::min(std::round(help_y - ph / 2.0f), H - std::round(W * 0.03f) - ph); }
}  // namespace

float help_pill_top(Assets& a, const Theme& th, const Metrics& m) {
  if (m.grid && !remake::active(th)) return m.help_bottom - m.help_h;  // the theme's pill, on the grid
  if (m.grid && access().scale() > 1.0f) {  // the help row of the grid themes: bigger with the text, its bottom on the margin
    const remake::Grid g(m.W, m.H);
    return float(g.bottom() - g.snap(float(g.help_h()) * access().scale(), 3));
  }
  const float px = std::floor(fv(th, "fHelp", 0.025f) * m.H * access().scale());
  const float ih = std::round(a.letter_height(px) * 1.55f);
  const float ph = std::round(ih * 1.85f);
  return help_pill_y0(m.W, m.H, m.help_y, ph);
}

void help_pill(Assets& a, const Style& st, float W, float H, float help_y, float px, std::vector<HelpItem>& it, std::vector<Op>& out,
               const Metrics* grid) {
  static Shapes shapes;
  if (it.empty()) return;
  const float ph = grid ? grid->help_h : std::round(std::round(a.letter_height(px) * 1.55f) * 1.85f);
  const float ih = grid ? std::round(ph / 1.85f) : std::round(a.letter_height(px) * 1.55f);
  const float pad = std::round(ph * 0.62f);
  const float igap = std::max(3.0f, std::round(W * 0.0075f));
  const float egap = std::round(ph * 0.85f);
  auto width = [&](size_t n) {
    float t = 2.0f * pad;
    for (size_t i = 0; i < n; ++i) t += it[i].iw * ih / it[i].ih + igap + it[i].tw + (i + 1 < n ? egap : 0.0f);
    return t;
  };
  size_t n = it.size();
  while (n > 1 && width(n) > W * 0.96f) --n;
  float total = std::round(width(n));
  float x0 = std::round((W - total) / 2.0f), y0 = help_pill_y0(W, H, help_y, ph);
  if (grid) {  // centred on the grid: the middle of the screen is a line, so a width of whole pairs of cells puts both edges on lines
    const float pair = 2.0f * float(grid->u);
    total = std::ceil(total / pair) * pair;
    const float m = float(grid->margin), cell = float(grid->u);
    x0 = m + std::round((W - 2.0f * m - total) / 2.0f / cell) * cell;  // centred to the nearest cell (half a cell at most off the middle)
    y0 = grid->help_bottom - ph;
  }
  if (remake::audit_sink()) remake::audit(x0, y0, total, ph, "ayuda", grid ? 'B' : 'C', false, grid != nullptr);
  shapes.rrect(a, out, x0, y0 + 2.0f, total, ph, st.radius(ph), st.dark ? Color{0, 0, 0, 90} : Color{0, 0, 0, 22});
  shapes.framed(a, out, x0, y0, total, ph, st.radius(ph), st.panel, st.line, 1.0f);
  const Color accent = st.dark ? st.cyan : st.cyan_dark;
  float x = x0 + std::round((total - width(n) + 2.0f * pad) / 2.0f);  // the prompts centred in the pill (it may be wider on the grid)
  const float cy = y0 + ph / 2.0f;
  for (size_t i = 0; i < n; ++i) {
    const HelpItem& e = it[i];
    const float iw = e.iw * ih / e.ih;
    out.push_back(Op::image(e.icon, std::round(x), std::round(cy - ih / 2.0f), iw, ih, e.primary ? accent : st.dim_dark));
    x += iw + igap;
    out.push_back(Op::image(e.text, std::round(x), std::floor(cy - e.th / 2.0f + 0.5f), e.tw, e.th, e.primary ? st.txt : st.dim_dark));
    x += e.tw + egap;
  }
}

float title_max_w(const Style& st, const Metrics& m, const std::vector<Op>& out, size_t stack_from, float dflt) {
  float left = m.W;  // leftmost image of the status stack (wifi / battery / clock)
  for (size_t i = stack_from; i < out.size(); ++i)
    if ((out[i].kind == Op::Image || (m.grid && out[i].kind == Op::Rect)) && out[i].w > 0) left = std::min(left, out[i].x);  // a remade bar may have a frame of rects
  // left-aligned (also Zenith, without the grid): a title that keeps clear of the bar; a long one ran into the wifi icon
  if (!st.header_center) return std::max(std::min(dflt, left - remake::Grid(m.W, m.H).gutter() - m.title_left), 3.0f * float(m.em));
  const float cx = m.col_x + m.col_w / 2.0f;
  const float room = 2.0f * std::min(cx - m.col_x, left - std::round(0.0222f * m.W) - cx);  // the same gap as the left-aligned list title
  return std::max(std::min(dflt, room), 3.0f * float(m.em));
}

float title_x(const Style& st, const Metrics& m, float left_x, float title_w) {
  if (!st.header_center) return left_x;
  return std::round(m.col_x + (m.col_w - title_w) / 2.0f);
}

HeaderBox& header_box() {
  static HeaderBox hb;
  return hb;
}

void put_text(std::vector<Op>& out, const Txt& t, float x, float cy, Color tint) {
  if (t.blank || !t.ok) return;
  out.push_back(Op::image(t.id, std::round(x), std::floor(cy - t.h / 2.0f + 0.5f), t.w, t.h, tint));
}

void put_text_right(std::vector<Op>& out, const Txt& t, float x_right, float cy, Color tint) { put_text(out, t, x_right - t.w, cy, tint); }

}  // namespace zf::ui
