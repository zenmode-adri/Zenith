#include "i18n.hpp"
#include "ui_style.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <map>

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

Style Style::from(const Theme& th) {
  // one parse per theme folder
  static std::map<std::string, Style> cache;
  auto hit = cache.find(th.dir());
  if (hit != cache.end()) return hit->second;

  Style s;
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

Metrics Metrics::from(const Theme& th, float w, float h) {
  Metrics m;
  m.W = w;
  m.H = h;
  m.margin = std::round(fv(th, "mx", 0.03f) * w);
  m.em = px_floor(std::floor(fv(th, "fMenu", 0.0375f) * h));
  m.title_px = px_floor(std::floor(fv(th, "fMenuTitle", 0.0383f) * h * kTitleScale));
  m.small_px = px_floor(std::floor(fv(th, "fMenuSmall", 0.0302f) * h));
  m.foot_px = px_floor(std::floor(fv(th, "fMenuFoot", 0.0271f) * h));
  m.col_w = std::round(std::min(w - 2.0f * m.margin, kColMaxH * h));
  m.col_x = std::round((w - m.col_w) / 2.0f);
  int lines = int(fv(th, "listLines", 9));
  if (lines < 3) lines = 9;
  m.list_top = std::round(fv(th, "listPosY", 0.1375f) * h);
  m.row_h = std::floor(fv(th, "listH", 0.7125f) * h / float(lines));
  m.list_h = m.row_h * float(lines);
  m.pill_h = std::round(fv(th, "selH", 0.0625f) * h);
  if (m.pill_h > m.row_h) m.pill_h = m.row_h;
  if (int(m.row_h - m.pill_h) % 2 != 0) m.pill_h = std::min(m.pill_h + 1.0f, m.row_h);
  m.header_cy = std::round(fv(th, "avY", 0.0667f) * h);
  m.help_y = std::round(fv(th, "helpY", 0.948f) * h);
  m.lines = lines;
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
  float px = std::floor(fv(th, "fHelp", 0.025f) * m.H);
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
  help_pill(a, st, m.W, m.help_y, px, hi_, out);
}

float help_pill_top(Assets& a, const Theme& th, const Metrics& m) {
  const float px = std::floor(fv(th, "fHelp", 0.025f) * m.H);
  const float ih = std::round(a.letter_height(px) * 1.55f);
  const float ph = std::round(ih * 1.85f);
  return std::round(m.help_y - ph / 2.0f);
}

void help_pill(Assets& a, const Style& st, float W, float help_y, float px, std::vector<HelpItem>& it, std::vector<Op>& out) {
  static Shapes shapes;
  if (it.empty()) return;
  const float ih = std::round(a.letter_height(px) * 1.55f);
  const float ph = std::round(ih * 1.85f);
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
  const float total = std::round(width(n));
  const float x0 = std::round((W - total) / 2.0f), y0 = std::round(help_y - ph / 2.0f);
  shapes.pill(a, out, x0, y0 + 2.0f, total, ph, st.dark ? Color{0, 0, 0, 90} : Color{0, 0, 0, 22});
  shapes.framed(a, out, x0, y0, total, ph, int(ph / 2.0f), st.panel, st.line, 1.0f);
  const Color accent = st.dark ? st.cyan : st.cyan_dark;
  float x = x0 + pad;
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
