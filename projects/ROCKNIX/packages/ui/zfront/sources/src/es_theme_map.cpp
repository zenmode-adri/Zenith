// ES theme -> Zenith variables: colour heuristics, font and logo detection, and the generated theme folder (see es_theme.hpp).
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "es_theme.hpp"
#include "gfx.hpp"
#include "imgdec.hpp"
#include "text.hpp"
#include "util.hpp"

namespace zf::estheme {

namespace {

// ---- colours -------------------------------------------------------------------------------------------------------------------

double chan(uint8_t v) {
  double c = v / 255.0;
  return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double rel_lum(const Rgb& c) { return 0.2126 * chan(c[0]) + 0.7152 * chan(c[1]) + 0.0722 * chan(c[2]); }
// The luma `ui::Style` uses to decide dark/light (gamma-encoded values).
double luma(const Rgb& c) { return (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]) / 255.0; }

Rgb mix(const Rgb& a, const Rgb& b, double t) {
  Rgb o;
  for (int i = 0; i < 3; ++i) o[size_t(i)] = uint8_t(std::lround(std::min(255.0, std::max(0.0, a[size_t(i)] * (1 - t) + b[size_t(i)] * t))));
  return o;
}
std::string hex(const Rgb& c) {
  char b[8];
  std::snprintf(b, sizeof b, "%02X%02X%02X", c[0], c[1], c[2]);
  return b;
}

void to_hsv(const Rgb& c, double& h, double& s, double& v) {
  double r = c[0] / 255.0, g = c[1] / 255.0, b = c[2] / 255.0;
  double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
  v = mx;
  s = mx > 0 ? d / mx : 0;
  if (d < 1e-9) h = 0;
  else if (mx == r) h = std::fmod((g - b) / d + 6.0, 6.0) / 6.0;
  else if (mx == g) h = ((b - r) / d + 2.0) / 6.0;
  else h = ((r - g) / d + 4.0) / 6.0;
}

/// `RRGGBB`, `RRGGBBAA`, with or without `#`. Alpha 255 when absent.
bool parse_hex(std::string_view s, Rgb& rgb, int* alpha = nullptr) {
  s = trim(s);
  if (!s.empty() && s[0] == '#') s.remove_prefix(1);
  if (s.size() != 6 && s.size() != 8) return false;
  unsigned v[4] = {0, 0, 0, 255};
  for (size_t i = 0; i < s.size() / 2; ++i) {
    unsigned x = 0;
    for (size_t k = 0; k < 2; ++k) {
      char ch = s[i * 2 + k];
      unsigned d;
      if (ch >= '0' && ch <= '9') d = unsigned(ch - '0');
      else if (ch >= 'a' && ch <= 'f') d = unsigned(ch - 'a' + 10);
      else if (ch >= 'A' && ch <= 'F') d = unsigned(ch - 'A' + 10);
      else return false;
      x = x * 16 + d;
    }
    v[i] = x;
  }
  rgb = {uint8_t(v[0]), uint8_t(v[1]), uint8_t(v[2])};
  if (alpha) *alpha = int(v[3]);
  return true;
}

// ---- reading the flattened theme -------------------------------------------------------------------------------------------------

const std::string& prop(const Element& e, const char* k) {
  static const std::string empty;
  auto it = e.props.find(k);
  return it == e.props.end() ? empty : it->second;
}
bool hidden(const Element& e) {
  const std::string& v = prop(e, "visible");
  if (v == "false" || v == "0") return true;
  float op = 1;
  const std::string& o = prop(e, "opacity");
  return !o.empty() && parse_float(o, op) && op < 0.05f;
}
bool is_list_view(const std::string& v) {
  return v != "system" && v != "menu" && v != "screen" && v != "splash" && v != "imageviewer" && v != "default" && !v.empty();
}
double fprop(const Element& e, const char* k, double def) {
  float f = 0;
  return parse_float(prop(e, k), f) ? double(f) : def;
}
// "1 1" -> both numbers >= 0.98 in at least one of size/minSize/maxSize: an image that covers the screen
bool full_size(const Element& e) {
  for (const char* k : {"size", "minSize", "maxSize"}) {
    const std::string& v = prop(e, k);
    size_t sp = v.find(' ');
    if (sp == std::string::npos) continue;
    float a = 0, b = 0;
    if (parse_float(trim(std::string_view(v).substr(0, sp)), a) && parse_float(trim(std::string_view(v).substr(sp + 1)), b) && a >= 0.98f && b >= 0.98f) return true;
  }
  return false;
}

/// Mean colour of the pixels that are not transparent (sampled on a grid); false if `path` is not a decodable image.
bool image_mean(const std::string& path, Rgb& out, bool* tiny = nullptr) {
  Img im;
  if (!decode_image_file(path, im) || im.w == 0 || im.h == 0) return false;
  if (tiny) *tiny = uint64_t(im.w) * im.h <= 1024;
  uint64_t n = 0, sum[3] = {0, 0, 0};
  uint32_t step = std::max<uint32_t>(1, uint32_t(std::sqrt(double(im.w) * im.h / 40000.0)));
  for (uint32_t y = 0; y < im.h; y += step)
    for (uint32_t x = 0; x < im.w; x += step) {
      const uint8_t* p = &im.rgba[(size_t(y) * im.w + x) * 4];
      if (p[3] < 128) continue;
      sum[0] += p[0];
      sum[1] += p[1];
      sum[2] += p[2];
      ++n;
    }
  if (!n) return false;
  out = {uint8_t(sum[0] / n), uint8_t(sum[1] / n), uint8_t(sum[2] / n)};
  return true;
}

struct BgCand {
  Rgb c;
  double z;
  std::string how;
};

/// Full-screen images of the given views with their effective colour (tint x picture mean), lowest zIndex first.
std::vector<BgCand> backgrounds(const Resolved& r, bool list_views) {
  std::vector<BgCand> v;
  for (const Element& e : r.elems) {
    if (e.type != "image" || hidden(e) || !full_size(e) || !e.parent.empty()) continue;  // nested = a per-item image, not a background
    if (list_views ? !is_list_view(e.view) : e.view != "system") continue;
    Rgb tint = {255, 255, 255};
    int alpha = 255;
    const std::string& col = prop(e, "color");
    if (!col.empty() && !parse_hex(col, tint, &alpha)) continue;
    if (alpha < 0xC0) continue;  // a scrim, not a background
    double op = fprop(e, "opacity", 1.0);
    if (op < 0.9) continue;
    Rgb base = {255, 255, 255};
    std::string how = "image '" + e.name + "' colour " + (col.empty() ? "(none)" : col);
    const std::string& pth = prop(e, "path");
    if (!pth.empty() && pth.find("${") == std::string::npos && pth.find('{') == std::string::npos) {
      std::string f = theme_file(r.root, pth);
      bool tiny = false;
      Rgb m;
      if (!f.empty() && image_mean(f, m, &tiny) && !tiny) {
        base = m;
        how += ", mean of " + path_filename(f);
      }
    }
    Rgb c;
    for (size_t i = 0; i < 3; ++i) c[i] = uint8_t((int(base[i]) * int(tint[i]) + 127) / 255);
    v.push_back({c, fprop(e, "zIndex", 0.0), how});
  }
  std::stable_sort(v.begin(), v.end(), [](const BgCand& a, const BgCand& b) { return a.z < b.z; });
  return v;
}

struct Cand {
  Rgb c;
  double w;
};

bool get_color(const Element& e, const char* k, Rgb& out, int min_alpha = 0x80) {
  const std::string& v = prop(e, k);
  int a = 255;
  if (v.empty() || !parse_hex(v, out, &a)) return false;
  return a >= min_alpha;
}

std::string pct(double v) {
  char b[16];
  std::snprintf(b, sizeof b, "%.1f", v);
  return b;
}

/// The font must draw plain Latin and the Spanish letters (a missing glyph would come from a fallback face and look broken).
bool font_covers_spanish(const std::string& path, std::string& missing) {
  FT_Library lib = nullptr;
  if (FT_Init_FreeType(&lib)) return true;  // cannot look: Font::load decides
  FT_Face face = nullptr;
  bool ok = true;
  if (FT_New_Face(lib, path.c_str(), 0, &face)) {
    FT_Done_FreeType(lib);
    return true;
  }
  static const uint32_t need[] = {'A', 'a', 'Z', 'z', '0', '9', '.', ',', 0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xFC, 0xF1, 0xC1, 0xC9, 0xCD, 0xD3, 0xDA, 0xDC, 0xD1, 0xBF, 0xA1};
  for (uint32_t cp : need)
    if (FT_Get_Char_Index(face, cp) == 0) {
      ok = false;
      utf8_append(missing, cp);
    }
  FT_Done_Face(face);
  FT_Done_FreeType(lib);
  return ok;
}

bool blocked_font(const std::string& file) {
  std::string l;
  for (char ch : path_filename(file)) l += char(std::tolower(static_cast<unsigned char>(ch)));
  for (const char* bad : {"gilroy", "rodin", "fot-", "fot_", "helvetica", "arial", "segoe", "sf-pro", "sfpro", "proxima", "calibri", "tahoma", "verdana"})
    if (l.find(bad) != std::string::npos) return true;
  return false;
}

}  // namespace

double contrast(const Rgb& a, const Rgb& b) {
  double la = rel_lum(a), lb = rel_lum(b);
  if (la < lb) std::swap(la, lb);
  return (la + 0.05) / (lb + 0.05);
}

bool fix_contrast(Rgb& fg, const Rgb& bg, double min) {
  if (contrast(fg, bg) >= min) return false;
  const Rgb white = {255, 255, 255}, black = {0, 0, 0};
  // the extreme that contrasts more with the background first
  const bool to_white = contrast(white, bg) >= contrast(black, bg);
  const Rgb first = to_white ? white : black, second = to_white ? black : white;
  for (const Rgb& target : {first, second}) {
    if (contrast(target, bg) < min) continue;
    for (int s = 1; s <= 50; ++s) {
      Rgb c = mix(fg, target, s / 50.0);
      if (contrast(c, bg) >= min) {
        fg = c;
        return true;
      }
    }
  }
  fg = contrast(white, bg) >= contrast(black, bg) ? white : black;  // cannot reach it: the best there is
  return true;
}

bool fix_multi(Rgb& fg, const std::vector<Rgb>& bgs, double min) {
  auto worst = [&](const Rgb& c) {
    double w = 1e9;
    for (const Rgb& b : bgs) w = std::min(w, contrast(c, b));
    return w;
  };
  if (worst(fg) >= min) return false;
  const Rgb white = {255, 255, 255}, black = {0, 0, 0};
  const bool to_white = worst(white) >= worst(black);
  for (const Rgb& target : {to_white ? white : black, to_white ? black : white}) {
    if (worst(target) < min) continue;
    for (int st = 1; st <= 50; ++st) {
      Rgb c = mix(fg, target, st / 50.0);
      if (worst(c) >= min) {
        fg = c;
        return true;
      }
    }
  }
  fg = worst(white) >= worst(black) ? white : black;  // cannot reach it: the best there is
  return true;
}

std::string Report::text() const {
  std::string o;
  size_t w = 0;
  for (const ReportLine& l : lines) w = std::max(w, l.item.size());
  for (const ReportLine& l : lines) {
    o += "  [" + l.status + "]";
    o += std::string(l.status.size() < 11 ? 11 - l.status.size() : 0, ' ');
    o += " " + l.item + std::string(w - l.item.size(), ' ') + "  " + l.detail + "\n";
  }
  return o;
}

void translate(const Resolved& r, const Palette& light, const Palette& dark, Translation& out) {
  out = Translation();
  Report& rep = out.report;

  // ---- background ---------------------------------------------------------------------------------------------------------------
  std::vector<BgCand> lb = backgrounds(r, true), sb = backgrounds(r, false);
  Rgb bg{};
  bool have_bg = false;
  std::string bg_how;
  if (!lb.empty()) {
    bg = lb[0].c;
    have_bg = true;
    bg_how = "gamelist " + lb[0].how;
  } else if (!sb.empty()) {
    bg = sb[0].c;
    have_bg = true;
    bg_how = "system view " + sb[0].how;
  }
  Rgb panel_c{};
  bool have_panel = false;
  for (const Element& e : r.elems)
    if (e.type == "menuBackground" && !hidden(e) && get_color(e, "color", panel_c, 0x40)) {
      have_panel = true;
      break;
    }
  if (!have_bg && have_panel) {
    bg = panel_c;
    have_bg = true;
    bg_how = "menu background colour";
  }
  out.dark = have_bg ? luma(bg) < 0.5 : true;
  const Palette& def = out.dark ? dark : light;
  Palette& p = out.pal;
  p = def;
  if (have_bg) {
    p.bg = bg;
    rep.add("background", "translated", hex(bg) + " from " + bg_how + " -> Zenith " + (out.dark ? "dark" : "light") + " base");
  } else {
    rep.add("background", "zenith", "no full-screen image or menu colour found: Zenith dark");
  }
  if (!lb.empty() && !sb.empty())
    rep.add("system view bg", "unsupported", "only one flat background colour is used (system view: " + sb[0].how + ")");

  // ---- text, accent, selection --------------------------------------------------------------------------------------------------
  std::vector<Cand> texts;        // text colours
  std::map<std::string, double> hist;  // every colour of the theme -> weight (for the accent)
  Rgb sel_bg{}, sel_txt{}, msel_bg{}, msel_txt{}, sep{}, hint{}, sw_on{}, sw_off{};
  bool have_sel_bg = false, have_sel_txt = false, have_msel_bg = false, have_msel_txt = false, have_sep = false, have_hint = false, have_on = false, have_off = false;
  int videos = 0;
  for (const Element& e : r.elems) {
    if (e.type == "video") ++videos;
    if (hidden(e)) continue;
    const bool list = is_list_view(e.view);
    Rgb c;
    if (e.type == "textlist") {
      if (get_color(e, "primaryColor", c)) texts.push_back({c, 3});
      if (get_color(e, "secondaryColor", c)) texts.push_back({c, 1.5});
      if (get_color(e, "selectedColor", c)) {
        sel_txt = c;
        have_sel_txt = true;
        texts.push_back({c, 2});
      }
      if (get_color(e, "selectorColor", c)) {
        sel_bg = c;
        have_sel_bg = true;
      }
    } else if (e.type == "menuText") {
      if (get_color(e, "color", c)) texts.push_back({c, 3});
      if (get_color(e, "selectedColor", c)) {
        msel_txt = c;
        have_msel_txt = true;
      }
      if (get_color(e, "selectorColor", c)) {
        msel_bg = c;
        have_msel_bg = true;
      }
      if (get_color(e, "separatorColor", c)) {
        sep = c;
        have_sep = true;
      }
    } else if (e.type == "menuTextSmall") {
      if (get_color(e, "color", c)) texts.push_back({c, 1});
    } else if (e.type == "text" && (list || e.view == "system")) {
      if (get_color(e, "color", c)) texts.push_back({c, list ? 1.0 : 0.5});
    } else if (e.type == "helpsystem") {
      if (get_color(e, "textColor", c)) {
        hint = c;
        have_hint = true;
        texts.push_back({c, 0.5});
      }
    } else if (e.type == "menuSwitch") {
      if (get_color(e, "colorOn", c)) {
        sw_on = c;
        have_on = true;
      }
      if (get_color(e, "colorOff", c)) {
        sw_off = c;
        have_off = true;
      }
    }
    for (const auto& kv : e.props) {
      const std::string& k = kv.first;
      if (k != "color" && !(k.size() > 5 && k.compare(k.size() - 5, 5, "Color") == 0)) continue;
      if (k == "shadowColor" || k == "separatorColor" || k == "selectorColor" || k == "selectorColorEnd") continue;
      if (get_color(e, k.c_str(), c)) hist[hex(c)] += (e.type == "textlist" || e.type == "menuText" || e.type == "text") ? 2 : 1;
    }
  }
  rep.add("video/animation/pos", "unsupported", std::to_string(videos) + " video element(s), animations and all positions ignored: Zenith's layout");

  // accent: the most used chromatic colour
  Rgb accent = def.accent;
  bool have_accent = false;
  {
    double best = 0;
    for (const auto& kv : hist) {
      Rgb c;
      parse_hex(kv.first, c);
      double h, s, v;
      to_hsv(c, h, s, v);
      if (s >= 0.35 && v >= 0.30 && kv.second > best) {
        best = kv.second;
        accent = c;
        have_accent = true;
      }
    }
  }
  if (!have_accent) {
    // a monochrome theme: the selection highlight or, failing that, the text colour carries the accent
    if (have_sel_bg && contrast(sel_bg, p.bg) >= 1.5) {
      accent = sel_bg;
      have_accent = true;
      rep.add("accent", "derived", hex(accent) + " (no chromatic colour in the theme: selector colour)");
    } else {
      accent = mix(p.bg, out.dark ? Rgb{255, 255, 255} : Rgb{0, 0, 0}, 0.8);
      rep.add("accent", "derived", hex(accent) + " (no chromatic colour in the theme: neutral)");
    }
  }
  p.accent = accent;
  if (have_accent && hist.count(hex(accent))) rep.add("accent", "translated", hex(accent) + " (most used chromatic colour, " + pct(hist[hex(accent)]) + " weight)");

  // text: the neutral candidate that reads best on the background
  {
    bool have = false;
    Rgb best{};
    double bc = 0, bw = 0;
    for (const Cand& t : texts) {
      double h, s, v;
      to_hsv(t.c, h, s, v);
      if (s >= 0.30) continue;
      double k = contrast(t.c, p.bg);
      if (!have || k > bc + 0.05 || (std::fabs(k - bc) <= 0.05 && t.w > bw)) {
        have = true;
        best = t.c;
        bc = k;
        bw = t.w;
      }
    }
    if (have) {
      p.txt = best;
      rep.add("text", "translated", hex(best) + " (neutral text colour with the best contrast, " + pct(bc) + ":1)");
    } else {
      rep.add("text", "zenith", hex(p.txt) + " (the theme's text colours are all chromatic)");
    }
    // dim: a neutral colour readable but below the text
    bool hd = false;
    Rgb dim{};
    double dw = 0;
    for (const Cand& t : texts) {
      double h, s, v;
      to_hsv(t.c, h, s, v);
      if (s >= 0.30) continue;
      double k = contrast(t.c, p.bg);
      if (k < 4.5 || k > contrast(p.txt, p.bg) * 0.92 || t.c == p.txt) continue;
      if (!hd || t.w > dw) {
        hd = true;
        dim = t.c;
        dw = t.w;
      }
    }
    if (hd) {
      p.dim = dim;
      rep.add("dim text", "translated", hex(dim));
    } else {
      p.dim = mix(p.bg, p.txt, 0.68);
      rep.add("dim text", "derived", hex(p.dim) + " (between background and text)");
    }
  }

  // selection pill: soft background + text on it
  if (have_sel_bg || have_msel_bg) {
    Rgb sb2 = have_sel_bg ? sel_bg : msel_bg;
    p.accent_soft = sb2;
    rep.add("selection bg", "translated", hex(sb2) + (have_sel_bg ? " (list selector)" : " (menu selector)"));
  } else {
    p.accent_soft = mix(p.bg, accent, 0.25);
    rep.add("selection bg", "derived", hex(p.accent_soft) + " (accent over the background)");
  }
  if (contrast(p.accent_soft, p.bg) < 1.12) {
    p.accent_soft = mix(p.bg, accent, 0.25);
    rep.add("selection bg", "adjusted", "too close to the background: " + hex(p.accent_soft));
  }
  if (have_sel_txt || have_msel_txt) {
    p.accent_dark = have_sel_txt ? sel_txt : msel_txt;
    rep.add("selection text", "translated", hex(p.accent_dark));
  } else {
    p.accent_dark = accent;
    rep.add("selection text", "derived", hex(accent) + " (accent)");
  }

  // surface of the cards: the next full-screen layer above the background, else the menu panel, else derived
  {
    bool have_surf = false;
    Rgb s{};
    const std::vector<BgCand>& src = !lb.empty() ? lb : sb;
    for (size_t i = 1; i < src.size(); ++i)
      if (contrast(src[i].c, p.bg) >= 1.04) {
        s = src[i].c;
        have_surf = true;
        rep.add("card surface", "translated", hex(s) + " from " + src[i].how);
        break;
      }
    if (!have_surf && have_panel && contrast(panel_c, p.bg) >= 1.04) {
      s = panel_c;
      have_surf = true;
      rep.add("card surface", "translated", hex(s) + " (menu background)");
    }
    if (!have_surf) {
      s = out.dark ? mix(p.bg, Rgb{255, 255, 255}, 0.07) : mix(p.bg, Rgb{255, 255, 255}, 0.70);
      rep.add("card surface", "derived", hex(s) + " (lighter than the background)");
    }
    p.surface = s;
  }
  if (have_panel) {
    p.panel = panel_c;
    rep.add("menu panel", "translated", hex(panel_c));
  } else {
    p.panel = out.dark ? mix(p.bg, Rgb{255, 255, 255}, 0.04) : mix(p.bg, Rgb{255, 255, 255}, 0.60);
    rep.add("menu panel", "derived", hex(p.panel));
  }
  if (have_sep) {
    p.separator = sep;
    rep.add("separator", "translated", hex(sep));
  } else {
    p.separator = mix(p.panel, p.txt, 0.15);
    rep.add("separator", "derived", hex(p.separator));
  }
  if (have_hint) {
    p.hint = hint;
    rep.add("help bar", "translated", hex(hint));
  } else {
    p.hint = p.dim;
    rep.add("help bar", "derived", hex(p.hint) + " (dim text)");
  }
  p.track = mix(p.bg, p.txt, 0.30);
  p.switch_on = have_on ? sw_on : mix(p.bg, accent, 0.55);
  p.switch_off = have_off ? sw_off : mix(p.bg, p.txt, 0.35);
  p.input_border = mix(p.surface, p.txt, 0.25);
  rep.add("switch/slider/input", have_on || have_off ? "translated" : "derived", "switch on " + hex(p.switch_on) + ", off " + hex(p.switch_off));

  // ---- font ---------------------------------------------------------------------------------------------------------------------
  {
    // the font that carries most of the text: lists and menus weigh more than labels (zfront draws everything with one font)
    std::map<std::string, double> fonts;
    std::vector<std::string> seen;
    for (const Element& e : r.elems) {
      const std::string& fp = prop(e, "fontPath");
      if (fp.empty() || fp.find("${") != std::string::npos || hidden(e)) continue;
      if (!fonts.count(fp)) seen.push_back(fp);
      fonts[fp] += e.type == "textlist" ? 3 : e.type == "menuText" || e.type == "menuTextSmall" ? 2 : 1;
    }
    std::vector<std::string> order = seen;
    std::stable_sort(order.begin(), order.end(), [&](const std::string& x, const std::string& y) { return fonts[x] > fonts[y]; });
    std::string why = fonts.empty() ? "the theme names no font file" : "";
    auto reject = [&](const std::string& m) { why += (why.empty() ? "" : "; ") + m; };
    why.clear();
    if (fonts.empty()) why = "the theme names no font file";
    for (const std::string& fp : order) {
      std::string f = theme_file(r.root, fp);
      if (f.empty()) {
        reject("font '" + fp + "' is not a file in the theme (built-in or outside)");
        continue;
      }
      if (blocked_font(f)) {
        reject("font '" + path_filename(f) + "' is proprietary and is not redistributed");
        continue;
      }
      struct stat sb;
      if (::stat(f.c_str(), &sb) != 0 || sb.st_size > (8 << 20) || sb.st_size < 1024) {
        reject("font '" + path_filename(f) + "' has an unusual size");
        continue;
      }
      Font ft;
      std::string ferr;
      if (!ft.load(f, ferr)) {
        reject("font '" + path_filename(f) + "' cannot be loaded (" + ferr + ")");
        continue;
      }
      std::string lacks;
      if (!font_covers_spanish(f, lacks)) {
        reject("font '" + path_filename(f) + "' lacks letters (" + lacks + "), Spanish text would fall back to another face");
        continue;
      }
      out.font_src = f;
      break;
    }
    if (!out.font_src.empty()) rep.add("font", "translated", path_filename(out.font_src));
    else rep.add("font", "zenith", "Rubik: " + why);
  }

  // ---- logos --------------------------------------------------------------------------------------------------------------------
  {
    std::string pattern, tint;
    int rank = 9;
    for (const Element& e : r.elems) {
      if (e.type != "image" || hidden(e)) continue;
      const std::string& pth = prop(e, "path");
      size_t k = pth.find("${system.theme}");
      if (k == std::string::npos) continue;
      std::string low;
      for (char ch : pth) low += char(std::tolower(static_cast<unsigned char>(ch)));
      bool art = false;  // big artwork / backgrounds / overlays named by system are not logos
      for (const char* w : {"artwork", "background", "overlay", "cover", "screenshot", "fanart", "/bg", "wheel", "marquee"})
        if (low.find(w) != std::string::npos) art = true;
      int rk = art ? 9 : (low.find("logo") != std::string::npos && e.view == "system") ? 0 : (low.find("logo") != std::string::npos) ? 1 : (e.view == "system" && e.name == "logo") ? 2 : 9;
      if (rk < rank) {
        rank = rk;
        pattern = pth;
        tint = prop(e, "color");
      }
    }
    std::string note;
    if (!pattern.empty()) {
      size_t k = pattern.find("${system.theme}");
      std::string pre = pattern.substr(0, k), suf = pattern.substr(k + 15);
      size_t slash = pre.rfind('/');
      std::string dirpart = slash == std::string::npos ? std::string(".") : pre.substr(0, slash);
      std::string fpre = slash == std::string::npos ? pre : pre.substr(slash + 1);
      std::string dir = theme_file(r.root, dirpart);
      std::string ext;  // the last dot of the suffix on (path_ext_lower would take ".png" for a whole name)
      if (size_t dot = suf.rfind('.'); dot != std::string::npos)
        for (char ch : suf.substr(dot)) ext += char(std::tolower(static_cast<unsigned char>(ch)));
      if (pre.find("${") != std::string::npos || suf.find("${") != std::string::npos || suf.find('/') != std::string::npos) {
        note = "logo path uses variables that are not resolved: " + pattern;
      } else if (dir.empty() || !is_dir(dir)) {
        note = "logo folder '" + dirpart + "' is not in the theme";
      } else if (ext == ".svg") {
        note = "logos are SVG (" + pattern + "): zfront draws raster logos only; system names are drawn as text";
      } else if (ext != ".png" && ext != ".webp" && ext != ".jpg" && ext != ".jpeg") {
        note = "unknown logo type " + ext;
      } else {
        out.logo_dir = dir;
        out.logo_prefix = fpre;
        out.logo_suffix = suf;
      }
    } else {
      note = "no per-system logo image found";
    }
    if (!out.logo_dir.empty()) {
      // sample the ink colour of a few logos: they must be readable on the card surface
      int n = 0, sampled = 0;
      double lum_sum = 0;
      if (DIR* d = ::opendir(out.logo_dir.c_str())) {
        while (dirent* en = ::readdir(d)) {
          std::string nm = en->d_name;
          if (nm.size() <= out.logo_prefix.size() + out.logo_suffix.size() || nm.compare(0, out.logo_prefix.size(), out.logo_prefix) != 0 ||
              nm.compare(nm.size() - out.logo_suffix.size(), out.logo_suffix.size(), out.logo_suffix) != 0)
            continue;
          ++n;
          if (sampled < 3 && n % 5 == 1) {
            Rgb m;
            if (image_mean(out.logo_dir + "/" + nm, m)) {
              lum_sum += rel_lum(m);
              ++sampled;
            }
          }
        }
        ::closedir(d);
      }
      std::string det = std::to_string(n) + " logo(s) from " + out.logo_dir.substr(r.root.size() + (out.logo_dir.size() > r.root.size() ? 1 : 0));
      if (sampled > 0) {
        double ink = lum_sum / sampled;
        Rgb inkc = ink > 0.5 ? Rgb{255, 255, 255} : Rgb{0, 0, 0};
        if (contrast(inkc, p.surface) < 2.5) {
          p.surface = ink > 0.5 ? Rgb{0x20, 0x20, 0x24} : Rgb{0xF4, 0xF4, 0xF6};
          rep.add("card surface", "adjusted", hex(p.surface) + " so that the " + std::string(ink > 0.5 ? "light" : "dark") + " logos are readable");
        }
      }
      if (!tint.empty() && tint != "ffffff" && tint != "FFFFFF") det += "; tint " + tint + " not applied";
      if (n == 0) {
        out.logo_dir.clear();
        rep.add("logos", "zenith", "the logo folder has no files named like the pattern");
      } else {
        rep.add("logos", "translated", det);
      }
    } else {
      rep.add("logos", "zenith", note + " (Zenith's logos or text are used)");
    }
  }

  // ---- contrast rules ---------------------------------------------------------------------------------------------------------
  // Every foreground/background pair zfront really draws (ui_style, menu, dialogs, scene): text on the background, the menu panel and the
  // card surface; dim text; the accent (icons, focus ring, captions); accent_dark (selected row text, titles and captions on light
  // themes) on the pill, the background and the panel; the help bar.
  auto adjust = [&](const char* name, Rgb& fg, std::vector<Rgb> bgs, double min) {
    Rgb before = fg;
    if (fix_multi(fg, bgs, min)) rep.add("contrast", "adjusted", std::string(name) + ": " + hex(before) + " -> " + hex(fg) + " (needs " + pct(min) + ":1)");
  };
  adjust("text on background", p.txt, {p.bg}, 7.0);
  adjust("text on menu panel", p.txt, {p.panel}, 4.5);
  p.tile_txt = p.txt;
  adjust("card title on surface", p.tile_txt, {p.surface}, 4.5);
  adjust("dim text", p.dim, {p.bg, p.panel}, 4.5);
  p.dim_dark = p.dim;
  adjust("dim text on surface", p.dim_dark, {p.bg, p.surface}, 4.5);
  adjust("help bar", p.hint, {p.bg}, 4.5);
  adjust("accent", p.accent, {p.bg, p.surface, p.panel}, out.dark ? 4.5 : 3.0);
  if (!have_sel_txt && !have_msel_txt) p.accent_dark = p.accent;
  {
    // accent_dark is the selected row's text on the pill and also title/caption text on the background and the panel: all three must read
    auto sel_ok = [&] { return contrast(p.accent_dark, p.accent_soft) >= 4.5 && contrast(p.accent_dark, p.bg) >= 4.5 && contrast(p.accent_dark, p.panel) >= 4.5; };
    if (!sel_ok() && (have_sel_bg || have_msel_bg)) {
      p.accent_soft = mix(p.bg, p.accent, 0.25);
      rep.add("selection bg", "adjusted", "the theme's selector colour cannot carry text that also reads on the background: " + hex(p.accent_soft) + " (accent over the background)");
    }
    if (!sel_ok()) {
      Rgb from = (have_sel_txt || have_msel_txt) && contrast(p.accent_dark, p.bg) >= 3.0 ? p.accent_dark : p.accent;
      p.accent_dark = from;
      adjust("selection text (on pill, background, panel)", p.accent_dark, {p.accent_soft, p.bg, p.panel}, 4.5);
    }
  }
}

// ---- generation ----------------------------------------------------------------------------------------------------------------------

namespace {

uint32_t crc32_update(uint32_t crc, const uint8_t* d, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put32(std::string& s, uint32_t v) {
  for (int i = 3; i >= 0; --i) s += char((v >> (i * 8)) & 0xFF);
}
void chunk(std::string& out, const char* type, const std::string& data) {
  put32(out, uint32_t(data.size()));
  std::string body = std::string(type, 4) + data;
  out += body;
  put32(out, crc32_update(0, reinterpret_cast<const uint8_t*>(body.data()), body.size()));
}

/// PNG with stored (uncompressed) deflate blocks: no compressor needed, the files are small and live in tmpfs.
bool write_png(const std::string& path, const Img& im) {
  if (im.w == 0 || im.h == 0 || im.rgba.size() != size_t(im.w) * im.h * 4) return false;
  std::string raw;
  raw.reserve(size_t(im.h) * (size_t(im.w) * 4 + 1));
  for (uint32_t y = 0; y < im.h; ++y) {
    raw += char(0);
    raw.append(reinterpret_cast<const char*>(&im.rgba[size_t(y) * im.w * 4]), size_t(im.w) * 4);
  }
  std::string z;
  z += char(0x78);
  z += char(0x01);
  size_t pos = 0;
  do {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    z += char(pos + n >= raw.size() ? 1 : 0);
    z += char(n & 0xFF);
    z += char((n >> 8) & 0xFF);
    z += char(~n & 0xFF);
    z += char((~n >> 8) & 0xFF);
    z.append(raw, pos, n);
    pos += n;
  } while (pos < raw.size());
  uint32_t a = 1, b = 0;
  for (unsigned char ch : raw) {
    a = (a + ch) % 65521;
    b = (b + a) % 65521;
  }
  put32(z, (b << 16) | a);
  std::string out = "\x89PNG\r\n\x1a\n";
  std::string ihdr;
  put32(ihdr, im.w);
  put32(ihdr, im.h);
  ihdr += char(8);
  ihdr += char(6);
  ihdr += char(0);
  ihdr += char(0);
  ihdr += char(0);
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", "");
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  return std::fclose(f) == 0 && ok;
}

/// Flat colour swap with tolerance, alpha kept (mkvariant.swap).
void swap_color(Img& im, const Rgb& src, const Rgb& dst, int tol) {
  for (size_t i = 0; i + 3 < im.rgba.size(); i += 4) {
    uint8_t* p = &im.rgba[i];
    if (p[3] && std::abs(int(p[0]) - src[0]) <= tol && std::abs(int(p[1]) - src[1]) <= tol && std::abs(int(p[2]) - src[2]) <= tol) {
      p[0] = dst[0];
      p[1] = dst[1];
      p[2] = dst[2];
    }
  }
}

/// Zenith's saturated cyan becomes the accent (hue of the accent; saturation and value scaled), mkvariant.retint_accent.
bool retint_accent(Img& im, const Rgb& accent) {
  double ch, cs, cv, ah, as, av;
  to_hsv(Rgb{0, 195, 227}, ch, cs, cv);
  to_hsv(accent, ah, as, av);
  bool any = false;
  for (size_t i = 0; i + 3 < im.rgba.size(); i += 4) {
    uint8_t* px = &im.rgba[i];
    if (!px[3]) continue;
    double r = px[0] / 255.0, g = px[1] / 255.0, b = px[2] / 255.0;
    double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = std::max(mx - mn, 1e-6);
    double s = mx > 0 ? (mx - mn) / std::max(mx, 1e-6) : 0;
    double h = (mx == r ? std::fmod((g - b) / d + 6.0, 6.0) : mx == g ? (b - r) / d + 2.0 : (r - g) / d + 4.0) / 6.0;
    if (!(s >= 0.35 && std::fabs(h - ch) <= 0.07)) continue;
    double s2 = std::min(1.0, s * as / cs), v2 = std::min(1.0, mx * av / cv);
    double h6 = ah * 6.0;
    int ii = int(std::floor(h6)) % 6;
    double f = h6 - std::floor(h6);
    double pp = v2 * (1 - s2), qq = v2 * (1 - s2 * f), tt = v2 * (1 - s2 * (1 - f));
    double rgb[6][3] = {{v2, tt, pp}, {qq, v2, pp}, {pp, v2, tt}, {pp, qq, v2}, {tt, pp, v2}, {v2, pp, qq}};
    for (int k = 0; k < 3; ++k) px[k] = uint8_t(std::lround(std::min(1.0, std::max(0.0, rgb[ii][k])) * 255.0));
    any = true;
  }
  return any;
}

bool mkdir_p(const std::string& path) {
  if (is_dir(path)) return true;
  size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0 && !mkdir_p(path.substr(0, slash))) return false;
  return ::mkdir(path.c_str(), 0755) == 0 || is_dir(path);
}

int rm_cb(const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }
bool remove_tree(const std::string& dir) {
  return ::nftw(dir.c_str(), rm_cb, 16, FTW_DEPTH | FTW_PHYS) == 0;
}

std::vector<std::string> ls(const std::string& dir) {
  std::vector<std::string> v;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d))
      if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) v.emplace_back(e->d_name);
    ::closedir(d);
  }
  std::sort(v.begin(), v.end());
  return v;
}

bool write_text(const std::string& path, const std::string& text) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  return std::fclose(f) == 0 && ok;
}

std::string real(const std::string& p) {
  char buf[PATH_MAX];
  return ::realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}

bool link_to(const std::string& target, const std::string& at) {
  ::unlink(at.c_str());
  return ::symlink(target.c_str(), at.c_str()) == 0;
}

bool usable_base(const std::string& d) { return !d.empty() && file_exists(d + "/theme.xml") && file_exists(d + "/layout.xml") && is_dir(d + "/assets"); }

// The colours of a Zenith base folder (variables from its theme.xml; the rest are the literals of the shipped Zenith).
bool base_palette(const std::string& dir, bool dark, Palette& p, std::string& err) {
  Theme t;
  if (!Theme::load(dir, "1/1", t, err)) return false;
  auto col = [&](const char* k, Rgb& o) {
    Color c = t.color(k);
    o = {c[0], c[1], c[2]};
    return t.has(k);
  };
  if (!col("bg", p.bg) || !col("txt", p.txt) || !col("dim", p.dim) || !col("cyan", p.accent) || !col("cyanDark", p.accent_dark) || !col("cyanSoft", p.accent_soft)) {
    err = dir + ": the base theme lacks colour variables";
    return false;
  }
  if (!col("dimDark", p.dim_dark)) p.dim_dark = p.dim;
  if (!col("tileTxt", p.tile_txt)) p.tile_txt = p.txt;
  Rgb v[6];
  if (dark) {
    v[0] = {0x38, 0x38, 0x3E}; v[1] = {0x2F, 0x2F, 0x34}; v[2] = {0x3E, 0x3E, 0x44}; v[3] = {0x55, 0x55, 0x5C}; v[4] = {0x4C, 0x97, 0xA8}; v[5] = {0x6C, 0x6C, 0x76};
    p.hint = {0xA8, 0xA8, 0xB0};
    p.input_border = {0x5B, 0x5B, 0x63};
  } else {
    v[0] = {0xFF, 0xFF, 0xFF}; v[1] = {0xF7, 0xF7, 0xF9}; v[2] = {0xE1, 0xE1, 0xE6}; v[3] = {0xD6, 0xD6, 0xDC}; v[4] = {0x3A, 0x8C, 0xA0}; v[5] = {0xA9, 0xA9, 0xB2};
    p.hint = {0x5F, 0x5F, 0x66};
    p.input_border = {0xC8, 0xC8, 0xCE};
  }
  p.surface = v[0]; p.panel = v[1]; p.separator = v[2]; p.track = v[3]; p.switch_on = v[4]; p.switch_off = v[5];
  return true;
}

// `<tag>OLD</tag>` -> `<tag>NEW</tag>`; first occurrence only or all of them. Returns how many were replaced.
int replace_tag(std::string& x, const std::string& tag, const std::string& oldv, const std::string& newv, bool all) {
  const std::string a = "<" + tag + ">" + oldv + "</" + tag + ">", b = "<" + tag + ">" + newv + "</" + tag + ">";
  int n = 0;
  size_t pos = 0;
  while ((pos = x.find(a, pos)) != std::string::npos) {
    x.replace(pos, a.size(), b);
    pos += b.size();
    ++n;
    if (!all) break;
  }
  return n;
}
// One pass over every `<tag>VALUE</tag>`: VALUE found in `map` (case-insensitive) is replaced by its new value, so a new value that equals
// another old one is not replaced twice.
void replace_tag_multi(std::string& x, const std::string& tag, const std::vector<std::pair<std::string, std::string>>& map) {
  const std::string open = "<" + tag + ">", close = "</" + tag + ">";
  size_t pos = 0;
  while ((pos = x.find(open, pos)) != std::string::npos) {
    size_t vs = pos + open.size(), ve = x.find(close, vs);
    if (ve == std::string::npos) break;
    std::string cur = x.substr(vs, ve - vs);
    for (char& ch : cur) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    for (const auto& m : map)
      if (cur == m.first) {
        x.replace(vs, ve - vs, m.second);
        ve = vs + m.second.size();
        break;
      }
    pos = ve + close.size();
  }
}
// first `<tag>HEX6</tag>`, whatever the value
bool replace_first_hex(std::string& x, const std::string& tag, const std::string& newv) {
  const std::string open = "<" + tag + ">", close = "</" + tag + ">";
  size_t pos = x.find(open);
  if (pos == std::string::npos) return false;
  size_t end = x.find(close, pos);
  if (end == std::string::npos || end - pos - open.size() != 6) return false;
  x.replace(pos + open.size(), 6, newv);
  return true;
}

}  // namespace

namespace {
uint64_t fnv(uint64_t h, const std::string& s) {
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  h ^= 0xFF;
  h *= 1099511628211ULL;
  return h;
}
std::string stamp(const std::string& p) {
  struct stat sb;
  if (::stat(p.c_str(), &sb) != 0) return "-";
  return std::to_string(uint64_t(sb.st_size)) + ":" + std::to_string(int64_t(sb.st_mtime));
}
// What the generated folder depends on: the flattened theme, the screen, the Zenith bases and the generator itself.
std::string fingerprint(const std::string& es_dir, const Resolved& r, const Options& opt, const Bases& bases) {
  uint64_t h = 1469598103934665603ULL;
  auto feed = [&](const std::string& v) { h = fnv(h, v); };
  feed("estheme-gen-6");
  feed(real(es_dir));
  feed(opt.ratio + "|" + std::to_string(opt.width) + "x" + std::to_string(opt.height) + "|" + opt.lang);
  for (const auto& kv : r.vars) feed(kv.first + "=" + kv.second);
  for (const auto& kv : r.subsets) feed("subset " + kv.first + "=" + kv.second);
  for (const Element& e : r.elems) {
    feed(e.view + "/" + e.type + "/" + e.name + "/" + e.parent);
    for (const auto& kv : e.props) feed(kv.first + "=" + kv.second);
  }
  feed(stamp(es_dir + "/theme.xml"));
  feed(stamp(es_dir));
  for (const std::string& b : {bases.light, bases.dark}) {
    feed(real(b));
    feed(stamp(b + "/theme.xml") + stamp(b + "/layout.xml") + stamp(b + "/assets"));
  }
  char buf[24];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return buf;
}
}  // namespace

std::string default_out_root() {
  // The folder survives a reboot on purpose: generating it is the slow part of the start, and its fingerprint says when it is stale.
  if (const char* e = ::getenv("ZF_ES_THEME_OUT"); e && *e) return e;
  if (::access("/storage/.cache", W_OK) == 0) return "/storage/.cache/zfront-estheme";
  if (::access("/run", W_OK) == 0) return "/run/zfront-estheme";
  return "/tmp/zfront-estheme";
}

std::string remade_style(const std::string& es_dir) {
  const std::string n = path_filename(es_dir);
  if (n == "es-theme-xmb-fcamod") return "xmb";
  if (n == "es-theme-art-book-next") return "artbook";
  if (n == "es-theme-iconic") return "iconic";
  if (n == "es-theme-techdweeb") return "techdweeb";
  return {};
}

namespace {
struct RemadeLook {
  const char* name;
  bool dark;
  const char* font;      // main face, inside the ES theme; "" = Zenith's
  const char* font_alt;  // face of the titles
  const char* extra;     // more variables
  uint32_t bg, txt, dim, dim_dark, tile_txt, accent, accent_dark, accent_soft, surface, panel, separator, track, switch_on, switch_off, hint, input_border;
};
const RemadeLook kLooks[] = {
    {"xmb", true, "", "", "", 0x14206E, 0xFFFFFF, 0xC8D0F0, 0xD8DEF5, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0x3A4DB0, 0x26348C, 0x1C2A7C, 0x3A48A0, 0x4A58B0, 0x7F95FF, 0x6C78B8, 0xDDE3FA, 0x5A68C0},
    {"artbook", true, "_inc/fonts/Roboto-Regular.ttf", "_inc/fonts/Roboto-Bold.ttf", "<selStyle>fill</selStyle><corner>0.2</corner>", 0x0A0A0C, 0xF0F0F2, 0xA0A0A6, 0x9A9AA0, 0xF0F0F2, 0xFFFFFF, 0xFFFFFF, 0x3E3E46, 0x1C1C20, 0x141417, 0x2C2C30, 0x3A3A40, 0x3A7FE0, 0x55555C, 0xC8C8CE, 0x44444A},
    {"iconic", false, "_inc/fonts/Oxygen-Regular.ttf", "", "<corner>0.5</corner>", 0xF2F3F8, 0x1C1E26, 0x5C5F6C, 0x464953, 0x1C1E26, 0x4B4FC4, 0x3E41A8, 0xDFE0F7, 0xFFFFFF, 0xFFFFFF, 0xEEF0F4, 0xD6D8E4, 0x4B4FC4, 0xA9ABB8, 0x5C5F6C, 0xC8CAD6},
    {"techdweeb", true, "", "_inc/fonts/dogicapixel.ttf", "<corner>0</corner>", 0x08080A, 0xE6E6EA, 0xA8A8AE, 0xC3C3C8, 0xE6E6EA, 0xFF7A1A, 0x08080A, 0xFF7A1A, 0x141416, 0x121214, 0x2C2C31, 0x3A3A3E, 0xFF7A1A, 0x55555C, 0xC0C0C6, 0x44444A},
};
Rgb rgb_of(uint32_t v) { return Rgb{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; }
uint32_t u32_of(const Rgb& c) { return uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | uint32_t(c[2]); }

// A colour variable of the ES theme (6 or 8 hex digits, the alpha ignored); false when it is not there or not a colour.
bool var_rgb(const Resolved& r, const char* name, Rgb& out) {
  auto it = r.vars.find(name);
  if (it == r.vars.end()) return false;
  std::string v = it->second;
  if (!v.empty() && v[0] == '#') v.erase(0, 1);
  if (v.size() != 6 && v.size() != 8) return false;
  for (char ch : v)
    if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
  out = rgb_of(uint32_t(std::stoul(v.substr(0, 6), nullptr, 16)));
  return true;
}

// Iconic as the user's choices ask: COLOR SCHEME (the dark colours are the ones its own XML defines for "dark": menuBackgroundColor,
// gamelistSelectorColor... in ui-components/theme-variables.xml), PLAY VIDEO PREVIEWS and SHOW DETAILED GAME INFO. The colours the remake
// draws its pages with (not part of the Zenith palette) travel as theme variables (`ic*`), which remake.cpp reads; `extra` owns their text.
RemadeLook iconic_look(const RemadeLook& base, const Resolved& r, std::string& extra) {
  RemadeLook l = base;
  auto sub = [&](const char* n) {
    auto it = r.subsets.find(n);
    return it == r.subsets.end() ? std::string() : it->second;
  };
  auto pick = [&](const char* n, uint32_t def) {
    Rgb c;
    return var_rgb(r, n, c) ? c : rgb_of(def);
  };
  Rgb ink = rgb_of(0x1C1E26), body = rgb_of(0x464953), dim = rgb_of(0x5C5F6C), card = rgb_of(0xFFFFFF), star = rgb_of(0xCDD0DC);
  const bool dark = sub("color-scheme") == "dark";
  if (dark) {
    const Rgb white = rgb_of(0xFFFFFF);
    const Rgb bg = pick("menuBackgroundColor", 0x151515), sel = pick("gamelistSelectorColor", 0x444444);
    ink = pick("menuSelectedColor", 0xF0F0F0);
    body = pick("gameMetadataColor", 0xCCCCCC);
    dim = pick("systemInfoSecondaryColor", 0xA0A0A0);
    star = pick("gamelistPrimaryColor", 0x777777);
    card = mix(bg, sel, 0.5);  // the cards sit one step above the page, like the panels of the theme sit on its dark overlay
    const Rgb accent = mix(rgb_of(base.accent), white, 0.35);  // the indigo is too dark to read on the page
    l.dark = true;
    l.bg = u32_of(bg);
    l.txt = u32_of(pick("menuTextColor", 0xCCCCCC));
    l.dim = u32_of(dim);
    l.dim_dark = u32_of(pick("systemManufacturerColor", 0xADADAD));
    l.tile_txt = u32_of(ink);
    l.accent = u32_of(accent);
    l.accent_soft = u32_of(mix(card, accent, 0.3));
    l.surface = u32_of(card);
    l.panel = u32_of(card);
    l.separator = u32_of(pick("menuGroupSeparatorColor", 0x444444));
    l.track = u32_of(sel);
    l.switch_on = u32_of(accent);
    l.switch_off = u32_of(star);
    l.hint = u32_of(dim);
    l.input_border = u32_of(sel);
  }
  extra = base.extra;
  extra += "<icDark>" + std::string(dark ? "1" : "0") + "</icDark>";
  extra += "<icInk>" + hex(ink) + "</icInk><icBody>" + hex(body) + "</icBody><icDim>" + hex(dim) + "</icDim><icCard>" + hex(card) + "</icCard><icStar>" + hex(star) +
           "</icStar>";
  extra += "<icVideo>" + std::string(sub("play-video-previews-of-games") == "no" ? "0" : "1") + "</icVideo>";
  extra += "<icDetail>" + std::string(sub("show-detailed-game-info") == "no" ? "0" : "1") + "</icDetail>";
  return l;
}
}  // namespace

bool generate(const std::string& es_dir_in, const Bases& bases, const std::string& out_root, const Options& opt, Generated& out, std::string& err) {
  out = Generated();
  std::string es_dir = es_dir_in;
  while (es_dir.size() > 1 && es_dir.back() == '/') es_dir.pop_back();
  Resolved r;
  if (!resolve(es_dir, opt, r, err)) return false;
  Palette lp{}, dp{};
  std::string e2;
  const bool have_l = usable_base(bases.light) && base_palette(bases.light, false, lp, e2);
  const bool have_d = usable_base(bases.dark) && base_palette(bases.dark, true, dp, e2);
  if (!have_l && !have_d) {
    err = "no usable Zenith base theme to build from" + (e2.empty() ? std::string() : " (" + e2 + ")");
    return false;
  }
  if (!have_l) lp = dp;  // one base only: it serves both (the colours are replaced anyway)
  if (!have_d) dp = lp;
  std::string name;
  for (char ch : path_filename(es_dir)) name += (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') ? ch : '_';
  if (name.empty() || name[0] == '.') name = "estheme";
  const std::string dir = out_root + "/" + name;

  // Same inputs as the folder already there (a restart of the frontend, THEME SET...): reuse it, generation is the slow part.
  const std::string key = fingerprint(es_dir, r, opt, bases);
  {
    std::string m;
    if (read_file(dir + "/.zfront-estheme", m) && m.find("\nkey=" + key + "\n") != std::string::npos && file_exists(dir + "/theme.xml") && is_dir(dir + "/assets")) {
      std::string rt;
      read_file(dir + "/report.txt", rt);
      out.dir = dir;
      out.dark = m.find("\ndark=1\n") != std::string::npos;
      out.cached = true;
      out.report_text = "  [cached]     generated folder reused (same inputs)\n" + rt;
      return true;
    }
  }

  Translation t;
  translate(r, lp, dp, t);
  const std::string style = remade_style(es_dir);
  const RemadeLook* look = nullptr;
  for (const RemadeLook& l : kLooks)
    if (style == l.name) look = &l;
  RemadeLook scheme_look;  // the look with the user's choices of the theme (Iconic)
  std::string look_extra;
  if (look && style == "iconic") {
    scheme_look = iconic_look(*look, r, look_extra);
    scheme_look.extra = look_extra.c_str();
    look = &scheme_look;
  }
  if (look) {  // these four carry their own, chosen colours and faces: nothing is derived from the XML
    t.dark = look->dark;
    t.pal = Palette{rgb_of(look->bg), rgb_of(look->txt), rgb_of(look->dim), rgb_of(look->dim_dark), rgb_of(look->tile_txt), rgb_of(look->accent), rgb_of(look->accent_dark), rgb_of(look->accent_soft),
                    rgb_of(look->surface), rgb_of(look->panel), rgb_of(look->separator), rgb_of(look->track), rgb_of(look->switch_on), rgb_of(look->switch_off), rgb_of(look->hint), rgb_of(look->input_border)};
    t.font_src = *look->font ? theme_file(es_dir, look->font) : std::string();
    t.report.add("remade", "translated", style + ": layout, pictures and colours of the zfront remake (remake.cpp)");
  }
  if (t.dark && !have_d) t.dark = false;  // only the light base exists
  if (!t.dark && !have_l) t.dark = true;
  const std::string base = real(t.dark ? bases.dark : bases.light);
  if (base.empty()) {
    err = "Zenith base theme not found";
    return false;
  }
  const bool mode_dark = t.dark;
  const Palette& pal = t.pal;
  const Palette& bp = mode_dark ? dp : lp;

  if (!mkdir_p(out_root)) {
    err = "cannot create " + out_root;
    return false;
  }
  if (is_dir(dir)) {
    if (!file_exists(dir + "/.zfront-estheme") || !remove_tree(dir)) {
      err = dir + " exists and is not ours";
      return false;
    }
  }
  if (!mkdir_p(dir + "/assets")) {
    err = "cannot create " + dir;
    return false;
  }
  // marker first, "pending" (no key, so it is never reused): a power cut in the middle leaves a folder this code recognises as its own
  // and regenerates, instead of "exists and is not ours" for ever. The real marker replaces it at the end.
  if (!write_text(dir + "/.zfront-estheme", path_filename(es_dir) + "\npending=1\n")) {
    err = "cannot write the marker file";
    return false;
  }
  out.dir = dir;
  out.dark = mode_dark;

  // theme.xml: Zenith's with the translated colours (the literal values of the base are the ones mkvariant.py replaces)
  std::string x;
  if (!read_file(base + "/theme.xml", x)) {
    err = "cannot read the base theme.xml";
    return false;
  }
  const struct {
    const char* tag;
    const Rgb* val;
  } vars[] = {{"bg", &pal.bg}, {"txt", &pal.txt}, {"dim", &pal.dim}, {"dimDark", &pal.dim_dark}, {"tileTxt", &pal.tile_txt},
              {"cyan", &pal.accent}, {"cyanDark", &pal.accent_dark}, {"cyanSoft", &pal.accent_soft}};
  for (const auto& v : vars)
    if (!replace_first_hex(x, v.tag, hex(*v.val))) {
      err = std::string("the base theme.xml has no <") + v.tag + ">";
      return false;
    }
  replace_tag_multi(x, "color", {{hex(bp.panel), hex(pal.panel)}, {hex(bp.surface), hex(pal.surface)}, {hex(bp.hint), hex(pal.hint)}});
  replace_tag(x, "separatorColor", hex(bp.separator), hex(pal.separator), true);
  replace_tag(x, "trackColor", hex(bp.track), hex(pal.track), true);
  replace_tag(x, "textColor", mode_dark ? "F0F0F2" : "2D2D32", hex(pal.txt), true);
  replace_tag(x, "colorOn", hex(bp.switch_on), hex(pal.switch_on), true);
  replace_tag(x, "colorOff", hex(bp.switch_off), hex(pal.switch_off), true);
  if (look) {
    const size_t vars_at = x.find("<variables>");
    if (vars_at != std::string::npos) {
      std::string more = std::string("\n<esStyle>") + look->name + "</esStyle><esDir>" + real(es_dir) + "</esDir>" + look->extra;
      if (*look->font_alt) {
        const std::string alt = theme_file(es_dir, look->font_alt);
        if (!alt.empty()) more += "<esFontAlt>" + real(alt) + "</esFontAlt>";
      }
      x.insert(vars_at + 11, more + "\n");
    }
  }
  // fonts
  std::string font_ext;
  if (!t.font_src.empty()) {
    font_ext = path_ext_lower(t.font_src);
    if (font_ext != ".ttf" && font_ext != ".otf" && font_ext != ".ttc") font_ext = ".ttf";
    size_t a = x.find("<font>");
    size_t b = a == std::string::npos ? a : x.find("</font>", a);
    if (b != std::string::npos) x.replace(a, b - a + 7, "<font>./assets/fonts/esfont" + font_ext + "</font>");
  }
  x.insert(x.find("<theme"), "<!-- Generated by zfront from the EmulationStation theme '" + path_filename(es_dir) + "' -->\n");
  if (!write_text(dir + "/theme.xml", x)) {
    err = "cannot write " + dir + "/theme.xml";
    return false;
  }
  link_to(base + "/layout.xml", dir + "/layout.xml");
  if (file_exists(base + "/splash.xml")) link_to(base + "/splash.xml", dir + "/splash.xml");

  // assets: links to the base, except the PNGs that carry colour (recoloured) and the font / logo folders
  const std::string ba = base + "/assets", da = dir + "/assets";
  int recolored = 0;
  auto process_png = [&](const std::string& src, const std::string& dst, const std::string& rel) {
    Img im;
    if (!decode_image_file(src, im)) {
      link_to(src, dst);
      return;
    }
    const std::string f = path_filename(rel);
    if (f.compare(0, 4, "pill") == 0) {
      for (size_t i = 0; i + 3 < im.rgba.size(); i += 4)
        if (im.rgba[i + 3]) {
          im.rgba[i] = pal.accent_soft[0];
          im.rgba[i + 1] = pal.accent_soft[1];
          im.rgba[i + 2] = pal.accent_soft[2];
        }
    } else {
      const std::vector<uint8_t> orig = im.rgba;
      if (f == "frame.png" || f == "tile.png") swap_color(im, bp.surface, pal.surface, 4);
      else if (f.compare(0, 9, "textinput") == 0) {
        const Rgb inbg = mode_dark ? Rgb{42, 42, 47} : Rgb{255, 255, 255};
        const Rgb border = mode_dark ? Rgb{91, 91, 99} : Rgb{200, 200, 206};
        swap_color(im, inbg, pal.surface, 4);
        if (f == "textinput.png") swap_color(im, border, pal.input_border, 4);
      }
      retint_accent(im, pal.accent);
      if (im.rgba == orig) {  // nothing in it follows the palette (help buttons, avatar...): keep the link
        link_to(src, dst);
        return;
      }
    }
    if (write_png(dst, im)) ++recolored;
    else link_to(src, dst);
  };
  for (const std::string& n : ls(ba)) {
    const std::string src = ba + "/" + n, dst = da + "/" + n;
    if (n == "fonts" && !t.font_src.empty()) {
      mkdir_p(dst);
      link_to(real(t.font_src), dst + "/esfont" + font_ext);
    } else if (n == "logos") {
      mkdir_p(dst);
      for (const std::string& l : ls(src)) link_to(src + "/" + l, dst + "/" + l);
    } else if (n == "menu") {
      mkdir_p(dst);
      for (const std::string& l : ls(src)) {
        if (ends_with(l, ".png")) process_png(src + "/" + l, dst + "/" + l, "menu/" + l);
        else link_to(src + "/" + l, dst + "/" + l);
      }
    } else if (ends_with(n, ".png") && n.compare(0, 13, "zenith-splash") != 0) {
      process_png(src, dst, n);
    } else {
      link_to(src, dst);
    }
  }
  // Zenith's collection icons (favourites, tools...) carry the cyan and get their caption drawn by the scene: they keep their own picture
  // (retinted to the accent) even when the ES theme has a logo of that name, otherwise the caption would repeat the logo's text.
  std::set<std::string> icon_names;
  if (is_dir(ba + "/logos/icon-labels"))
    for (const std::string& sub : ls(ba + "/logos/icon-labels"))
      for (const std::string& f : ls(ba + "/logos/icon-labels/" + sub))
        if (ends_with(f, ".txt")) icon_names.insert(f.substr(0, f.size() - 4));
  // the theme's own logos replace Zenith's of the same system
  int logos = 0;
  std::set<std::string> own;  // systems whose logo comes from the ES theme
  if (!t.logo_dir.empty()) {
    mkdir_p(da + "/logos");
    const std::string root_real = real(es_dir) + "/";
    for (const std::string& n : ls(t.logo_dir)) {
      if (n.size() <= t.logo_prefix.size() + t.logo_suffix.size() || n.compare(0, t.logo_prefix.size(), t.logo_prefix) != 0 ||
          n.compare(n.size() - t.logo_suffix.size(), t.logo_suffix.size(), t.logo_suffix) != 0)
        continue;
      std::string sys = n.substr(t.logo_prefix.size(), n.size() - t.logo_prefix.size() - t.logo_suffix.size());
      if (sys.empty() || sys.find('/') != std::string::npos || sys[0] == '.' || icon_names.count(sys)) continue;
      const std::string rf = real(t.logo_dir + "/" + n);
      if (rf.compare(0, root_real.size(), root_real) != 0 || !file_exists(rf)) continue;  // a symlink that leaves the theme
      if (link_to(rf, da + "/logos/" + sys + ".webp")) {
        ++logos;
        own.insert(sys);
      }
    }
  }
  // collection icons (favourites, tools...) have the cyan baked in: they follow the accent, unless the theme brings its own
  int icons = 0;
  for (const std::string& n : icon_names) {
    if (own.count(n) || !file_exists(ba + "/logos/" + n + ".webp")) continue;
    Img im;
    if (!decode_image_file(ba + "/logos/" + n + ".webp", im) || !retint_accent(im, pal.accent)) continue;
    ::unlink((da + "/logos/" + n + ".webp").c_str());
    if (write_png(da + "/logos/" + n + ".webp", im)) ++icons;
    else link_to(ba + "/logos/" + n + ".webp", da + "/logos/" + n + ".webp");
  }
  t.report.add("generated", "translated", dir + " (" + std::to_string(recolored) + " PNG recoloured, " + std::to_string(logos) + " logo link(s), " + std::to_string(icons) + " icon(s) retinted)");
  for (const std::string& w : r.warnings) t.report.add("warning", "unsupported", w);
  if (!write_text(dir + "/.zfront-estheme", path_filename(es_dir) + "\nkey=" + key + "\ndark=" + (mode_dark ? "1" : "0") + "\n")) {
    err = "cannot write the marker file";
    return false;
  }
  write_text(dir + "/report.txt", t.report.text());
  {  // the same JSON as tools/mkvariant.py palettes, so tools/montage.py can check the contrast of a translated theme
    std::string j = "{\"name\":\"" + name + "\",\"mode\":\"" + (mode_dark ? "dark" : "light") + "\"";
    const struct {
      const char* k;
      std::string v;
    } kv[] = {{"bg", hex(pal.bg)}, {"txt", hex(pal.txt)}, {"dim", hex(pal.dim)}, {"dimDark", hex(pal.dim_dark)}, {"tileTxt", hex(pal.tile_txt)},
              {"accent", hex(pal.accent)}, {"accentDark", hex(pal.accent_dark)}, {"accentSoft", hex(pal.accent_soft)}, {"surface", hex(pal.surface)},
              {"panel", hex(pal.panel)}, {"separator", hex(pal.separator)}, {"track", hex(pal.track)}, {"switchOn", hex(pal.switch_on)},
              {"switchOff", hex(pal.switch_off)}, {"hint", hex(pal.hint)}, {"inputBorder", hex(pal.input_border)}};
    for (const auto& e : kv) j += std::string(",\"") + e.k + "\":\"" + e.v + "\"";
    j += ",\"hintBg\":\"FFFFFF0D\",\"scroll\":\"F0F0F260\"}\n";
    write_text(dir + "/palette.json", j);
  }
  out.report = t.report;
  out.report_text = t.report.text();
  return true;
}

}  // namespace zf::estheme
