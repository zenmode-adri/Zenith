// Skinned menus: Aurora, Swiss, Industrial and Cinema draw every menu (main menu, Appearance, all the others) their own way, in both modes.
// It is the same Menu (same entries, cursor, scrolling, values); only the geometry (`skin_metrics`) and the drawing (`draw_skin`) change.
// Text sizes come from ui::level_px (see `skin_px`); boxes, shapes and gaps are a fraction of the screen height (the boards were drawn at 720 high):
// `P(v)` = v * H / 720, rounded, and never smaller than what the text needs (`skin_metrics`, `skin_frame`).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "i18n.hpp"
#include "menu.hpp"
#include "motion.hpp"
#include "remake.hpp"
#include "scene.hpp"
#include "util.hpp"

namespace zf {

static std::string skin_icon_dir() {
  const char* env = ::getenv("ZF_SKIN_FONTS");
  return env && *env ? std::string(env) : std::string("/usr/share/zfront/fonts");
}

std::string Menu::skin_glyph(const Entry& e, bool main) {
  if (e.kind == EntryKind::Header || e.kind == EntryKind::Separator) return {};
  if (main) {
    static const std::pair<const char*, const char*> kMain[] = {
        {"games", "games"}, {"battery", "battery"}, {"screen", "screen"}, {"network", "network"}, {"library", "library"},
        {"controller", "controller"}, {"ui", "appearance"}, {"system", "system"}, {"quit", "quit"}};
    for (const auto& p : kMain)
      if (e.icon_name == p.first) return p.second;
    return {};
  }
  struct L {
    std::string label;
    const char* glyph;
  };
  const L kSub[] = {{tr("SELECTOR STYLE"), "sel"},
                    {tr("TITLE ALIGNMENT"), "align"},
                    {tr("LIST TEXT CASE"), "t:Aa"},
                    {tr("TEXT SIZE"), "t:AA"},
                    {tr("HIGH CONTRAST"), "contrast"},
                    {tr("SHOW CLOCK"), "clock"},
                    {tr("SHOW CLOCK IN 12-HOUR FORMAT"), "t:12"},
                    {tr("SHOW BATTERY STATUS"), "batt"},
                    {tr("CAROUSEL TRANSITIONS"), "carousel"}};
  for (const L& l : kSub)
    if (e.label == l.label) return l.glyph;
  return {};
}

using ui::Icon;

namespace {

Color rgbc(uint32_t v, int a = 255) { return Color{uint8_t(v >> 16), uint8_t((v >> 8) & 255), uint8_t(v & 255), uint8_t(a)}; }

std::string upper(std::string s) { return utf8_upper(s); }  // the headers of a translated menu have accents and the letter n with tilde

// "CHOOSE" -> "Choose"; anything that is not all capitals stays
std::string sentence(std::string s) {
  bool caps = !s.empty();
  for (char c : s)
    if (c >= 'a' && c <= 'z') caps = false;
  if (!caps) return s;
  for (size_t i = 1; i < s.size(); ++i)
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] = char(s[i] - 'A' + 'a');
  return s;
}

void set_txt(Assets& a, ui::Txt& t, std::string_view s, int px, float max_w, bool alt) {
  a.use_alt_face(alt);
  t.set(a, s, px, max_w);
  a.use_alt_face(false);
}

// Every text of the three skins takes its size from ui::level_px (the same table as every other screen, TEXT SIZE and the 11 px floor included):
// no text is sized on its own and nothing is multiplied by the text size again. The boxes below are derived from these sizes.
struct SkinPx {
  int title, row, val, small, foot, help;
};

SkinPx skin_px(float W, float H) {
  SkinPx s;
  s.title = ui::level_px(ui::Level::Title, W, H);
  s.row = ui::level_px(ui::Level::Row, W, H);
  s.small = ui::level_px(ui::Level::Small, W, H);
  s.foot = ui::level_px(ui::Level::Foot, W, H);
  s.help = ui::level_px(ui::Level::Help, W, H);
  s.val = std::max(11, int(std::round(float(s.row) * 0.9f)));  // a value is a row a little smaller, the same in the three skins
  return s;
}

// The vertical frame shared by the drawing and the metrics: where the panel starts and ends, the Industrial title bar, the keycap of the help row.
// The panel starts under the title (so a big title or TEXT SIZE pushes it down) and ends over the help row (so a big help row pushes it up).
struct SkinFrame {
  float top = 0, bot = 0, bh = 0, kd = 0;
};

SkinFrame skin_frame(int sk, float W, float H) {
  const float f = H / 720.0f;
  const auto P = [f](float v) { return std::max(1.0f, std::round(v * f)); };
  const SkinPx s = skin_px(W, H);
  const remake::Grid grid(W, H);
  SkinFrame fr;
  fr.kd = std::max(P(24), float(std::lround(float(s.help) * 1.6f)));
  const float tpx = sk == 3 ? float(s.foot) : float(s.title);  // Industrial has a small chip up there; its title is on the bar
  const float th = std::ceil(tpx * 1.3f);
  const float title_cy = std::max(P(42), float(grid.top()) + std::ceil(th / 2.0f));
  fr.top = std::max(P(72), std::ceil(title_cy + th / 2.0f + P(6) + (sk == 2 ? P(8) : 0.0f)));
  fr.bot = std::min(P(648), H - float(grid.margin()) - fr.kd - P(8));
  fr.bh = sk == 3 ? std::max(P(32), float(std::lround(float(s.row) * 1.5f))) : 0.0f;
  return fr;
}

}  // namespace

void Menu::skin_metrics(Ctx& cx) {
  const float W = cx.w, H = cx.h, f = H / 720.0f;
  const auto P = [f](float v) { return std::max(1.0f, std::round(v * f)); };
  const SkinPx px = skin_px(W, H);
  const SkinFrame fr = skin_frame(skin_, W, H);
  // Text size (accessibility): the text sizes already include it (ui::level_px); the rows and headers grow with it (above normal) so the text keeps its room.
  // Smaller text keeps the rows. At normal size every line below is the original arithmetic.
  const float grow = std::max(ui::access().scale(), 1.0f);
  const auto G = [grow](float v) { return grow > 1.0f ? std::round(v * grow) : v; };
  const float row = has_icons_ ? 1.0f : 0.0f;
  // Boxes from the text: a row is at least 1.6 x the row text, a section header 2 x the header text; the designed size (scaled by the screen
  // height and by TEXT SIZE above normal) stays when it is already bigger.
  const float row_min = std::round(float(px.row) * 1.6f), hdr_min = std::round(float(px.small) * 2.0f);
  switch (skin_) {
    case 1:  // Aurora: a panel of glass
      m_.col_x = P(24) + P(8);
      m_.col_w = W - 2.0f * m_.col_x;
      m_.list_top = fr.top + P(8);
      m_.list_h = fr.bot - fr.top - P(16);
      m_.row_h = std::max(G(P(48 + 12 * row)), row_min);
      hdr_h_ = std::max(G(P(40)), hdr_min);
      break;
    case 2:  // Swiss: rows edge to edge between two thick rules
      m_.col_x = 0;
      m_.col_w = W;
      m_.list_top = fr.top;
      m_.list_h = fr.bot - fr.top;
      m_.row_h = std::max(G(P(52 + 12 * row)), row_min);
      hdr_h_ = std::max(G(P(36)), hdr_min);
      break;
    case 3:  // Industrial: the panel of an appliance, keys as rows
      m_.col_x = P(36);
      m_.col_w = W - 2.0f * m_.col_x;
      m_.list_top = fr.top + P(12) + fr.bh + P(12);  // panel, padding, title bar, padding
      m_.list_h = fr.bot - P(12) - m_.list_top;
      m_.row_h = std::max(G(P(44 + 8 * row)), row_min);
      hdr_h_ = std::max(G(P(32)), hdr_min);
      break;
    default:  // Cinema: open rows with a round icon
      m_.col_x = P(24);
      m_.col_w = W - 2.0f * m_.col_x;
      m_.list_top = P(has_icons_ ? 72 : 96);
      m_.list_h = P(has_icons_ ? 576 : 552);
      m_.row_h = std::max(G(P(has_icons_ ? 64 : 52)), row_min);
      hdr_h_ = std::max(G(P(28)), hdr_min);
      break;
  }
  m_.pill_h = m_.row_h;
  m_.lines = std::max(int(m_.list_h / m_.row_h), 1);
}

void Menu::draw_skin(Ctx& cx, std::vector<Op>& out) {
  Assets& a = cx.assets;
  const ui::Metrics& m = m_;
  const float W = m.W, H = m.H, f = H / 720.0f;
  const auto P = [f](float v) { return std::max(1.0f, std::round(v * f)); };
  // Text sizes come from ui::level_px (TEXT SIZE included): `px.title/row/val/small/foot/help`. P() is only for boxes, shapes and gaps.
  const SkinPx px = skin_px(W, H);
  const SkinFrame fr = skin_frame(skin_, W, H);
  const bool dk = st_.dark;
  const int sk = skin_;
  const remake::Grid grid(W, H);
  const float mg = float(grid.margin());  // the title and the rules start on the first line of the grid
  {
    const float bw = std::max(std::round(0.007f * H), 3.0f);
    scroll_room_ = content_h_ > m.list_h + 0.5f ? bw + P(6) : 0.0f;
  }

  // ---- palette of the skin ------------------------------------------------------------------------------------------------------------
  Color page = st_.bg, fg{}, dim{}, accent{}, on_accent{}, rule{}, sel_bg{}, sel_fg{}, glass{}, glass_edge{}, sep{}, circ{}, strip{}, strip_txt{}, bar{}, lip{}, led_off{}, key_bg{};
  switch (sk) {
    case 1:
      fg = dk ? rgbc(0xFFFFFF) : rgbc(0x1D1B3A);
      dim = dk ? rgbc(0xFFFFFF, 150) : rgbc(0x5A5790);
      accent = dk ? rgbc(0x2EE6C8) : rgbc(0x0FA58F);
      glass = dk ? rgbc(0xFFFFFF, 41) : rgbc(0xFFFFFF, 140);
      glass_edge = dk ? rgbc(0xFFFFFF, 128) : rgbc(0xFFFFFF, 230);
      sel_bg = dk ? rgbc(0xFFFFFF, 56) : rgbc(0xFFFFFF, 235);
      sel_fg = fg;
      break;
    case 2:
      fg = dk ? rgbc(0xFFFFFF) : rgbc(0x12163A);
      dim = fg;
      rule = fg;
      sel_bg = dk ? rgbc(0xFFFFFF) : rgbc(0x2540FF);
      sel_fg = dk ? rgbc(0x2540FF) : rgbc(0xF3F1EA);
      sep = dk ? rgbc(0xFFFFFF, 77) : rgbc(0x12163A, 64);
      break;
    case 3:
      fg = dk ? rgbc(0xF1EFE6) : rgbc(0x1B1A17);
      dim = dk ? rgbc(0x9A988D) : rgbc(0x8A887E);
      accent = rgbc(0xFF5A1F);
      lip = dk ? rgbc(0x0B0B0C) : rgbc(0xCFCDC4);
      glass = dk ? rgbc(0x2B2A27) : rgbc(0xF5F4EE);   // the panel
      key_bg = dk ? rgbc(0x35342F) : rgbc(0xE8E6DF);  // a key
      bar = dk ? rgbc(0x0B0B0C) : rgbc(0x17161A);     // the title bar and the value chips
      strip = dk ? rgbc(0x201F1D) : rgbc(0xDCDAD1);
      strip_txt = dk ? rgbc(0x9A988D) : rgbc(0x6E6C63);
      led_off = dk ? rgbc(0x4A4842) : rgbc(0xC4C2B8);
      break;
    default:
      fg = dk ? rgbc(0xF5F6FA) : rgbc(0x14161C);
      dim = dk ? rgbc(0xF5F6FA, 140) : rgbc(0x14161C, 140);
      sel_bg = dk ? rgbc(0xFFFFFF, 23) : rgbc(0xFFFFFF);
      rule = fg;  // the ring of the chosen row
      sep = dk ? rgbc(0xFFFFFF, 26) : rgbc(0x14161C, 26);
      circ = dk ? rgbc(0xFFFFFF, 31) : rgbc(0x14161C, 20);
      break;
  }
  (void)on_accent;
  // SELECTOR STYLE over the skin: `fill` is the skin's own focus (a filled box); outline / underline / bar are drawn in the skin's accent, with its stroke
  // and its corner, and the focused row keeps the colours of a row at rest (`hot` = the row sits on a filled box)
  const ui::SelStyle alt = sk >= 1 && sk <= 3 ? skin_sel_ : ui::SelStyle::Fill;
  const bool plain = alt != ui::SelStyle::Fill;
  const Color hl = sk == 2 ? sel_bg : accent;
  const float stroke = P(3);  // the rules of Swiss

  out.push_back(Op::rect(0, 0, W, H, page));
  remake::menu_backdrop(cx.th, a, W, H, out);

  // ---- header ---------------------------------------------------------------------------------------------------------------------------
  const size_t stack_from = out.size();
  if (sk != 4) status_bar(cx.th, a, W, H, cx.status, out);  // Cinema has a chip with the count instead
  (void)stack_from;
  float title_x = mg, title_cy = P(42), title_w = 0, title_h = 0;
  const std::string& title = spec_.title;
  // a title with a short form (MenuSpec::title_short) is measured whole first and swaps to the short one only when it does not fit its room
  auto set_title = [&](float room, int tpx, bool alt) {
    set_txt(a, t_title_, title, tpx, spec_.title_short.empty() ? room : 0.0f, alt);
    if (!spec_.title_short.empty() && t_title_.w > room) set_txt(a, t_title_, spec_.title_short, tpx, room, alt);
  };
  if (sk == 3) {  // a small orange lamp and the name of the machine; the title goes on the bar of the panel
    const float r = P(6);
    if (!t_chip_.alive(a)) set_txt(a, t_chip_, "ZENITH  \xC2\xB7  MENU", px.foot, W * 0.5f, true);
    title_cy = std::max(title_cy, float(grid.top()) + std::ceil(t_chip_.h / 2.0f));
    shapes_.rrect(a, out, mg, title_cy - r, 2.0f * r, 2.0f * r, int(r), accent);
    ui::put_text(out, t_chip_, mg + 2.0f * r + P(12), title_cy, fg);
    title_w = t_chip_.w, title_h = t_chip_.h;
    title_x = mg + 2.0f * r + P(12);
  } else {
    const int tpx = px.title;
    if (!t_title_.alive(a)) set_title(ui::title_max_w(st_, m, out, stack_from, W * 0.62f), tpx, sk == 4);
    if (sk == 4) title_cy = P(40);
    title_cy = std::max(title_cy, float(grid.top()) + std::ceil(t_title_.h / 2.0f));
    if (sk != 4) title_x = ui::title_x(st_, m, title_x, t_title_.w);  // TITLE ALIGNMENT: centred on the page (title_max_w left room for the status bar)
    ui::put_text(out, t_title_, title_x, title_cy, fg);
    title_w = t_title_.w, title_h = t_title_.h;
  }
  title_cy = std::max(title_cy, float(grid.top()) + std::ceil(title_h / 2.0f));
  ui::header_box() = {true, sk == 3 ? mg : title_x, (sk == 3 ? mg : title_x) + title_w + (sk == 3 ? title_x - mg : 0.0f), title_cy - title_h / 2.0f, title_cy + title_h / 2.0f, P(64)};

  // ---- containers -----------------------------------------------------------------------------------------------------------------------
  const float lt = m.list_top, lh = m.list_h, cx0 = m.col_x, cw = m.col_w;
  int n_sel = 0, at_sel = 0;
  for (size_t i = 0; i < e_.size(); ++i)
    if (e_[i].kind != EntryKind::Header && e_[i].kind != EntryKind::Separator) {
      ++n_sel;
      if (int(i) <= cur_) at_sel = n_sel;
    }
  if (sk == 4) {  // the count, in a white chip at the right of the title
    if (!t_chip_.alive(a)) set_txt(a, t_chip_, std::to_string(n_sel) + " " + (has_icons_ ? tr("items") : tr("options")), px.foot, W * 0.3f, false);
    const float chw = t_chip_.w + P(28), chh = std::max(P(28), std::round(float(px.foot) * 1.6f)), chx = W - mg - chw, chy = std::max(P(40), float(grid.top()) + chh / 2.0f);
    shapes_.pill(a, out, chx, chy - chh / 2.0f, chw, chh, fg);
    ui::put_text(out, t_chip_, chx + P(14), chy, page);
  }
  // the panel goes from `fr.top` (under the title) to `fr.bot` (over the help row); the designed 72..648 at 720 high when there is room
  const float panel_h = fr.bot - fr.top;
  if (sk == 1) {
    shapes_.rrect(a, out, mg, fr.top, W - 2.0f * mg, panel_h, int(P(40)), glass);
    shapes_.ring(a, out, mg, fr.top, W - 2.0f * mg, panel_h, int(P(40)), 1.0f, glass_edge);
  } else if (sk == 2) {
    out.push_back(Op::rect(mg, fr.top - P(8), W - 2.0f * mg, P(3), rule));
    out.push_back(Op::rect(mg, fr.bot, W - 2.0f * mg, P(3), rule));
  } else if (sk == 3) {
    shapes_.rrect(a, out, mg, fr.top + P(4), W - 2.0f * mg, panel_h, int(P(24)), lip);
    shapes_.rrect(a, out, mg, fr.top, W - 2.0f * mg, panel_h, int(P(24)), glass);
    const float bx = mg + P(12), by = fr.top + P(12), bw = W - 2.0f * mg - P(24), bh = fr.bh;
    shapes_.rrect(a, out, bx, by, bw, bh, int(P(8)), bar);
    // centred (TITLE ALIGNMENT): as wide as fits between the two ends of the bar, keeping clear of the count on the right on both sides
    const float chip_room = std::max(t_chip2_.alive(a) ? t_chip2_.w : 0.0f, float(px.foot) * 5.0f);
    const float title_max = st_.header_center ? std::min(bw * 0.6f, bw - 2.0f * (chip_room + P(24))) : bw * 0.6f;
    if (!t_title_.alive(a)) set_title(title_max, px.row, false);
    const float bar_tx = st_.header_center ? ui::title_x(st_, m, bx + P(12), t_title_.w) : bx + P(12);
    ui::put_text(out, t_title_, bar_tx, by + bh / 2.0f, rgbc(0xF4F2EA));
    if (remake::audit_sink() && !t_title_.blank) remake::audit(bar_tx, by + (bh - t_title_.h) / 2.0f, t_title_.w, t_title_.h, "titulo-barra", 'C');
    if (!t_chip2_.alive(a)) {
      char buf[24];
      std::snprintf(buf, sizeof buf, "%02d / %02d", std::max(at_sel, 1), n_sel);
      set_txt(a, t_chip2_, buf, px.foot, bw * 0.3f, true);
      chip_for_ = at_sel;
    }
    ui::put_text_right(out, t_chip2_, bx + bw - P(12), by + bh / 2.0f, accent);
    if (remake::audit_sink() && !t_chip2_.blank) remake::audit(bx + bw - P(12) - t_chip2_.w, by + (bh - t_chip2_.h) / 2.0f, t_chip2_.w, t_chip2_.h, "cuenta", 'C');
    if (chip_for_ != at_sel) t_chip2_.reset();
  }

  // ---- rows -----------------------------------------------------------------------------------------------------------------------------
  const bool scrolls = content_h_ > lh + 0.5f;
  const float bar_w = std::max(std::round(0.007f * H), 3.0f);
  float row_x0 = cx0, row_w = cw;
  if (sk == 3) row_x0 = cx0 + P(28), row_w = cw - P(28);
  out.push_back(Op::clip(0, lt, W, lh));

  // the chosen row (it glides)
  if (cur_ >= 0 && size_t(cur_) < e_.size()) {
    const float sy = std::round(lt + sel_y_ - scroll_);
    const float rh = m.row_h;
    if (plain) {  // the user's selector: the box of the row of each skin, marked as asked (the skin's accent, its corner and its stroke)
      float bx = cx0, by = sy + P(2), bw = cw, bh = rh - P(4);
      int br = int(bh / 2.0f);
      if (sk == 2) bx = 0, by = sy, bw = W, bh = rh, br = 0;
      else if (sk == 3) {
        bx = row_x0, by = sy, bw = row_w, bh = rh - P(4), br = int(P(has_icons_ ? 12 : 10));
        shapes_.rrect(a, out, bx, by + P(2), bw, bh, br, lip);  // the key stays a key: only its mark changes
        shapes_.rrect(a, out, bx, by, bw, bh, br, key_bg);
      }
      const float in = sk == 3 ? float(br) : 0.0f;  // the key of Industrial has a rounded plate: its mark keeps to the straight part; the others have no box drawn: the whole row
      if (alt == ui::SelStyle::Outline) shapes_.ring(a, out, bx, by, bw, bh, br, stroke, hl);
      else if (alt == ui::SelStyle::Underline) out.push_back(Op::rect(bx + in, by + bh - stroke, bw - 2.0f * in, stroke, hl));
      else shapes_.rrect(a, out, bx, by + in / 2.0f, std::max(2.0f * stroke, 3.0f), bh - in, int(stroke), hl);
    } else switch (sk) {
      case 1: {
        const float py = sy + P(2), ph = rh - P(4);
        shapes_.rrect(a, out, cx0, py, cw, ph, int(ph / 2.0f), sel_bg);
        shapes_.ring(a, out, cx0, py, cw, ph, int(ph / 2.0f), 1.0f, glass_edge);
        break;
      }
      case 2:
        out.push_back(Op::rect(0, sy, W, rh, sel_bg));
        break;
      case 3: {
        const float by = sy, bh = rh - P(4);
        shapes_.rrect(a, out, row_x0, by + P(2), row_w, bh, int(P(has_icons_ ? 12 : 10)), rgbc(0xB83A0D));
        shapes_.rrect(a, out, row_x0, by, row_w, bh, int(P(has_icons_ ? 12 : 10)), accent);
        break;
      }
      default: {
        const float by = sy + P(4), bh = rh - P(8);
        shapes_.rrect(a, out, cx0, by, cw, bh, int(P(12)), sel_bg);
        shapes_.ring(a, out, cx0, by, cw, bh, int(P(12)), P(3), rule);
        break;
      }
    }
  }

  const int lab_px = px.row, val_px = px.val, num_px = px.foot;
  // numbered rows: the label starts after the number, which grows with its text size
  const float num_gap = std::ceil(float(px.foot) * 1.3f);
  int ordinal = 0;
  if (t_num_.size() != e_.size()) t_num_.assign(e_.size(), ui::Txt());
  for (size_t i = 0; i < e_.size(); ++i) {
    Entry& e = e_[i];
    const bool is_hdr = e.kind == EntryKind::Header, is_sep = e.kind == EntryKind::Separator;
    if (!is_hdr && !is_sep) ++ordinal;
    const float y = std::round(lt + top_[i] - scroll_), h = hgt_[i];
    if (y + h <= lt || y >= lt + lh) continue;
    const float cy = y + h / 2.0f;
    const bool sel = int(i) == cur_;
    const bool last_before_hdr = i + 1 >= e_.size() || e_[i + 1].kind == EntryKind::Header;

    if (is_sep) continue;
    if (is_hdr) {
      const int hp = px.small;
      if (!e.t_label.alive(a)) set_txt(a, e.t_label, upper(e.label), hp, cw - P(48), sk != 4);
      if (sk == 1) ui::put_text(out, e.t_label, cx0 + P(16), y + std::round(h * 0.4f), dim);
      else if (sk == 2) {
        if (i > 0) out.push_back(Op::rect(mg, y, W - 2.0f * mg, P(3), rule));
        ui::put_text(out, e.t_label, mg, y + h / 2.0f, ui::with_alpha(fg, 0.75f));
      } else if (sk == 3) {
        const float sh = std::max(P(24), std::round(float(px.small) * 1.6f)), sy = y + h - sh;  // the strip sits on the foot of the header
        shapes_.rrect(a, out, cx0, sy, cw, sh, int(P(6)), strip);
        shapes_.disc(a, out, cx0 + P(8), sy + (sh - P(6)) / 2.0f, int(P(6)), led_off);
        shapes_.disc(a, out, cx0 + cw - P(14), sy + (sh - P(6)) / 2.0f, int(P(6)), led_off);
        ui::put_text(out, e.t_label, cx0 + P(24), sy + sh / 2.0f, strip_txt);
      } else {
        ui::put_text(out, e.t_label, cx0 + P(12), y + h - P(6) - e.t_label.h / 2.0f, dim);
      }
      continue;
    }

    // ---- colours of this row
    const bool hot = sel && !plain;  // on a filled box
    Color lab_c = fg, val_c = dim, num_c = dim;
    if (hot) {
      if (sk == 2) lab_c = val_c = num_c = sel_fg;
      else if (sk == 3) lab_c = val_c = rgbc(0xFFFFFF), num_c = rgbc(0xFFFFFF);
      else if (sk == 4) val_c = fg;
      else val_c = ui::with_alpha(fg, 0.85f);
    } else {
      if (sk == 2) val_c = fg, num_c = ui::with_alpha(fg, 0.6f);
      if (sk == 3) num_c = dim;
    }
    if (!e.enabled) lab_c = ui::with_alpha(lab_c, 0.45f), val_c = ui::with_alpha(val_c, 0.45f);
    if (e.is_danger) lab_c = st_.danger;

    // ---- row furniture (unchosen)
    float lx = cx0, rx = cx0 + cw - scroll_room_;
    switch (sk) {
      case 1:
        lx = cx0 + P(16), rx = cx0 + cw - P(16) - scroll_room_;
        break;
      case 2:
        if (!hot && !last_before_hdr) out.push_back(Op::rect(0, y + h - 1.0f, W, 1, sep));
        lx = mg + std::max(P(48), num_gap + P(12)), rx = W - mg - scroll_room_;
        break;
      case 3: {
        const float bh = h - P(4);
        const float lx0 = cx0 + P(14);
        if (!sel) {
          shapes_.rrect(a, out, row_x0, y + P(2), row_w, bh, int(P(has_icons_ ? 12 : 10)), lip);
          shapes_.rrect(a, out, row_x0, y, row_w, bh, int(P(has_icons_ ? 12 : 10)), key_bg);
        } else {
          shapes_.disc(a, out, lx0 - P(10), y + bh / 2.0f - P(10), int(P(20)), rgbc(0xFF5A1F, 60));
        }
        shapes_.disc(a, out, lx0 - P(4), y + bh / 2.0f - P(4), int(P(8)), sel ? rgbc(0xFF5A1F) : led_off);
        lx = row_x0 + P(12) + std::max(P(has_icons_ ? 32 : 28), num_gap + P(10)), rx = row_x0 + row_w - P(has_icons_ ? 16 : 8) - scroll_room_;
        break;
      }
      default: {
        lx = cx0 + P(12), rx = cx0 + cw - P(12) - scroll_room_;
        if (!sel && !last_before_hdr) out.push_back(Op::rect(cx0 + P(56), y + h - 1.0f, cw - P(56), 1, sep));
        break;
      }
    }
    const float rcy = sk == 3 ? y + (h - P(4)) / 2.0f : cy;

    // numbered rows (Swiss, Industrial)
    if (sk == 2 || sk == 3) {
      if (!t_num_[i].alive(a)) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%02d", ordinal);
        set_txt(a, t_num_[i], buf, num_px, 0, sk == 3);
      }
      ui::put_text(out, t_num_[i], sk == 2 ? mg : row_x0 + P(12), rcy, num_c);
    }
    // icon of the row (Cinema)
    if (sk == 4 && (has_icons_ || sub_gl_)) {
      std::string gl = skin_glyph(e, has_icons_);
      if (gl.empty() && sub_gl_) gl = "dot";
      if (gl.empty()) gl = "-";  // main menu row without a glyph: the theme's own icon below
      const int d = int(P(has_icons_ ? 40 : 32)), isz = int(P(has_icons_ ? 22 : 17));
      const float ix = lx, iy = cy - float(d) / 2.0f;
      shapes_.disc(a, out, ix, iy, d, sel ? fg : circ);
      const Color gc = sel ? page : fg;
      if (!e.t_icon.alive(a)) {
        ImgId id = 0;
        e.t_icon.ok = true;
        if (gl.rfind("t:", 0) == 0) {
          set_txt(a, e.t_icon, gl.substr(2), px.foot, 0, true);
          e.t_icon.blank = false;
        } else {
          const std::string path = gl == "-" ? cx.th.path("assets/menu/menu-icon-" + e.icon_name + ".svg") : skin_icon_dir() + "/icon-" + gl + ".svg";
          e.t_icon.blank = !a.svg(path, uint32_t(isz), id);
          e.t_icon.id = id;
          if (!e.t_icon.blank) e.t_icon.w = float(a.img(id).w), e.t_icon.h = float(a.img(id).h);
        }
      }
      if (!e.t_icon.blank) {
        if (gl.rfind("t:", 0) == 0) ui::put_text(out, e.t_icon, ix + (float(d) - e.t_icon.w) / 2.0f, iy + float(d) / 2.0f, gc);
        else out.push_back(Op::image(e.t_icon.id, std::round(ix + (float(d) - e.t_icon.w) / 2.0f), std::round(iy + (float(d) - e.t_icon.h) / 2.0f), e.t_icon.w, e.t_icon.h, gc));
      }
      lx += float(d) + P(12);
    } else if (sk == 1 && has_icons_ && !e.icon_name.empty()) {
      const int d = int(P(40)), isz = int(P(24));
      const float ix = lx, iy = cy - float(d) / 2.0f;
      // the three tints of the board: violet, teal, pink (translucent with a hairline of the same hue)
      struct Tint { uint32_t fill_d; uint8_t a_d, b_d; uint32_t fill_l; uint8_t a_l; uint32_t line_l; uint8_t b_l; uint32_t ink_l; };
      static const Tint kT[3] = {{0x7A5CFF, 127, 102, 0x7A5CFF, 56, 0x7A5CFF, 102, 0x3B2FA0},
                                 {0x2EE6C8, 87, 71, 0x2EC8AA, 71, 0x14A08C, 115, 0x0B6F60},
                                 {0xFF5FA8, 97, 76, 0xFF5FA8, 56, 0xC83C78, 102, 0xB0306E}};
      const Tint& t = kT[size_t(ordinal - 1) % 3];
      if (dk) shapes_.framed(a, out, ix, iy, float(d), float(d), int(P(14)), rgbc(t.fill_d, t.a_d), rgbc(0xFFFFFF, t.b_d), 1.0f);
      else shapes_.framed(a, out, ix, iy, float(d), float(d), int(P(14)), rgbc(t.fill_l, t.a_l), rgbc(t.line_l, t.b_l), 1.0f);
      if (!e.t_icon.alive(a)) {
        ImgId id = 0;
        e.t_icon.ok = true;
        std::string path = skin_icon_dir() + "/icon-a-" + e.icon_name + ".svg";
        if (!file_exists(path)) path = cx.th.path("assets/menu/menu-icon-" + e.icon_name + ".svg");
        e.t_icon.blank = !a.svg(path, uint32_t(isz), id);
        e.t_icon.id = id;
        if (!e.t_icon.blank) e.t_icon.w = float(a.img(id).w), e.t_icon.h = float(a.img(id).h);
      }
      if (!e.t_icon.blank) out.push_back(Op::image(e.t_icon.id, std::round(ix + (float(d) - e.t_icon.w) / 2.0f), std::round(iy + (float(d) - e.t_icon.h) / 2.0f), e.t_icon.w, e.t_icon.h, dk ? rgbc(0xFFFFFF) : rgbc(kT[size_t(ordinal - 1) % 3].ink_l)));
      lx += float(d) + P(16);
    }

    // ---- value / controls, from the right
    const float arrow = std::max(P(sk == 2 ? 22 : 16), std::round(float(px.row) * (sk == 2 ? 0.8f : 0.6f)));
    float x = rx;
    float ctrl_w = 0;
    const bool chip = sk == 3;
    auto put_value = [&](bool with_chev) {
      if (with_chev && sk == 2 && sel && has_icons_ && e.show_arrow) {  // Swiss: the chosen main row ends in an arrow
        if (!e.t_icon.alive(a)) {
          ImgId id = 0;
          e.t_icon.ok = true;
          e.t_icon.blank = !a.svg(skin_icon_dir() + "/icon-s-arrow.svg", uint32_t(arrow), id);
          e.t_icon.id = id;
          if (!e.t_icon.blank) e.t_icon.w = float(a.img(id).w), e.t_icon.h = float(a.img(id).h);
        }
        if (!e.t_icon.blank) out.push_back(Op::image(e.t_icon.id, std::round(x - e.t_icon.w), std::round(rcy - e.t_icon.h / 2.0f), e.t_icon.w, e.t_icon.h, lab_c));
        x -= arrow + P(6);
        ctrl_w += arrow + P(6);
      } else if (with_chev && (e.show_arrow || e.kind == EntryKind::Choice) && sk != 2 && !(sk == 3 && !e.value_text.empty())) {
        shapes_.icon(a, out, Icon::ChevronRight, x - arrow, rcy - arrow / 2.0f, int(arrow), sk == 3 && !chip ? val_c : (sel ? (sk == 3 ? rgbc(0xFFFFFF) : lab_c) : ui::with_alpha(val_c, 0.8f)));
        x -= arrow + P(6);
        ctrl_w += arrow + P(6);
      }
      if (!e.t_value.alive(a)) set_txt(a, e.t_value, e.value_text.empty() && e.kind == EntryKind::Text ? std::string("\xE2\x80\x94") : e.value_text, val_px, cw * 0.45f, sk == 3);
      if (e.t_value.blank || e.value_text.empty()) return;
      if (chip) {
        const float cw2 = e.t_value.w + std::max(P(20), float(val_px)), ch = std::max(P(24), std::round(float(val_px) * 1.5f));
        shapes_.rrect(a, out, x - cw2, rcy - ch / 2.0f, cw2, ch, int(P(6)), bar);
        ui::put_text(out, e.t_value, x - cw2 + std::max(P(20), float(val_px)) / 2.0f, rcy, accent);
        x -= cw2 + P(8);
        ctrl_w += cw2 + P(8);
      } else {
        ui::put_text_right(out, e.t_value, x, rcy, val_c);
        x -= e.t_value.w + P(8);
        ctrl_w += e.t_value.w + P(8);
      }
    };
    switch (e.kind) {
      case EntryKind::Toggle: {
        const float t = e.kn >= 0.0f ? e.kn : (e.b ? 1.0f : 0.0f);
        const bool on = t >= 0.5f;
        if (sk == 2) {
          const float tw = P(48), th = P(24), bx = rx - tw, by = rcy - th / 2.0f, bd = P(3);
          const Color base = hot ? sel_bg : page;
          out.push_back(Op::rect(bx, by, tw, th, lab_c));
          if (!on) out.push_back(Op::rect(bx + bd, by + bd, tw - 2.0f * bd, th - 2.0f * bd, base));
          const float tx = bx + P(6) + (tw - P(12) - P(12)) * t;
          out.push_back(Op::rect(tx, by + (th - P(12)) / 2.0f, P(12), P(12), on ? base : lab_c));
          ctrl_w = tw;
        } else if (sk == 1) {
          const float tw = P(44), th = P(24), bx = rx - tw, by = rcy - th / 2.0f, kd = P(18);
          const Color fill = on ? ui::with_alpha(accent, dk ? 0.40f : 0.45f) : (dk ? rgbc(0xFFFFFF, 31) : rgbc(0x1D1B3A, 30));
          const Color edge = on ? ui::with_alpha(accent, 0.9f) : (dk ? rgbc(0xFFFFFF, 89) : rgbc(0x1D1B3A, 77));
          shapes_.rrect(a, out, bx, by, tw, th, int(th / 2.0f), fill);
          shapes_.ring(a, out, bx, by, tw, th, int(th / 2.0f), 1.0f, edge);
          shapes_.disc(a, out, bx + P(3) + (tw - kd - P(6)) * t, by + (th - kd) / 2.0f, int(kd), on ? rgbc(0xFFFFFF) : (dk ? rgbc(0xFFFFFF, 178) : rgbc(0x1D1B3A, 110)));
          ctrl_w = tw;
        } else if (sk == 3) {
          const float tw = P(52), th = P(24), bx = rx - tw, by = rcy - th / 2.0f;
          shapes_.rrect(a, out, bx, by, tw, th, int(P(6)), bar);
          const float kw = P(24), kh = P(18);
          const float kx = bx + P(2) + (tw - kw - P(4)) * t;
          shapes_.rrect(a, out, kx, by + P(4), kw, kh, int(P(4)), rgbc(dk ? 0x8A887E : 0x8A887E));
          shapes_.rrect(a, out, kx, by + P(2), kw, kh, int(P(4)), rgbc(0xF5F4EE));
          const float lx2 = on ? bx + P(10) : bx + tw - P(10) - P(8);
          if (on) shapes_.disc(a, out, lx2 - P(5), rcy - P(9), int(P(18)), rgbc(0xFF5A1F, 70));
          shapes_.disc(a, out, lx2, rcy - P(4), int(P(8)), on ? rgbc(0xFF5A1F) : rgbc(0x4A4842));
          ctrl_w = tw;
        } else {
          const float tw = P(44), th = P(26), bx = rx - tw, by = rcy - th / 2.0f, kd = P(22);
          const Color fill = on ? fg : (dk ? rgbc(0xFFFFFF, 51) : rgbc(0x14161C, 46));
          shapes_.rrect(a, out, bx, by, tw, th, int(th / 2.0f), fill);
          shapes_.disc(a, out, bx + P(2) + (tw - kd - P(4)) * t, by + (th - kd) / 2.0f, int(kd), on ? page : rgbc(0xFFFFFF));
          ctrl_w = tw;
        }
        break;
      }
      case EntryKind::Slider: {
        std::string v = e.fmt ? e.fmt(e.i) : std::to_string(e.i) + e.unit_text;
        std::string hi = e.fmt ? e.fmt(e.max) : std::to_string(e.max) + e.unit_text;
        if (!e.t_value.alive(a)) set_txt(a, e.t_value, v, val_px, cw * 0.3f, sk == 3);
        const float vw = std::max(e.t_value.w, a.measure(hi, float(val_px)));
        ui::put_text_right(out, e.t_value, rx, rcy, val_c);
        const float track = std::round(cw * 0.28f), th = std::max(P(5), 3.0f), kd = P(18);
        const float tl = rx - vw - P(12) - track;
        const float frac = e.max > e.min ? float(e.i - e.min) / float(e.max - e.min) : 0.0f;
        const Color tc = sk == 2 ? ui::with_alpha(lab_c, 0.35f) : ui::with_alpha(fg, 0.25f);
        const Color fc = sk == 2 ? lab_c : (sk == 3 ? rgbc(hot ? 0xFFFFFF : 0xFF5A1F) : (sk == 4 ? fg : accent));
        shapes_.rrect(a, out, tl, rcy - th / 2.0f, track, th, int(th / 2.0f), tc);
        const float kx = tl + kd / 2.0f + frac * (track - kd);
        shapes_.rrect(a, out, tl, rcy - th / 2.0f, std::max(kx - tl, th), th, int(th / 2.0f), fc);
        shapes_.disc(a, out, kx - kd / 2.0f, rcy - kd / 2.0f, int(kd), sk == 2 ? lab_c : rgbc(0xFFFFFF));
        ctrl_w = track + vw + P(12);
        break;
      }
      case EntryKind::Action:
      case EntryKind::Choice:
        put_value(true);
        if (e.kind == EntryKind::Choice && sel && e.cycle && e.options.size() > 1 && sk != 3 && sk != 2) {
          shapes_.icon(a, out, Icon::ChevronLeft, x - arrow + P(6) - e.t_value.w - P(4), rcy - arrow / 2.0f, int(arrow), val_c);
        }
        break;
      default:
        put_value(false);
        break;
    }

    // ---- label
    const float maxw = std::max(rx - lx - ctrl_w - P(12), 3.0f * float(lab_px));
    if (!e.t_label.alive(a)) set_txt(a, e.t_label, e.label, lab_px, maxw, false);
    ui::put_text(out, e.t_label, lx, rcy, lab_c);
    if (remake::audit_sink() && y >= lt && y + h <= lt + lh) {
      remake::audit(lx, rcy - e.t_label.h / 2.0f, e.t_label.w, e.t_label.h, "\"" + e.label.substr(0, 18) + "\"", 'C');
      if (ctrl_w > 0) remake::audit(rx - ctrl_w, rcy - float(val_px) / 2.0f, ctrl_w, float(val_px), "valor de \"" + e.label.substr(0, 18) + "\"", 'C');
    }
  }
  out.push_back(Op::unclip());

  // scrollbar
  if (scrolls) {
    const float maxs = content_h_ - lh;
    const float th = std::max(std::round(lh * lh / content_h_), 2.0f * float(m.em));
    const float ty = lt + std::min(std::max(scroll_ / maxs, 0.0f), 1.0f) * (lh - th);
    shapes_.pill(a, out, W - mg - bar_w, ty, bar_w, th, ui::with_alpha(sk == 3 ? fg : fg, 0.30f));
  }

  // ---- help row at the foot: a key and a word, from the right -------------------------------------------------------------------
  const ui::Prompt* hp = nullptr;
  size_t hn = 2;
  static const ui::Prompt kPlain[] = {{"updown", N_("CHOOSE")}, {"B", N_("BACK")}};
  static const ui::Prompt kAct[] = {{"updown", N_("CHOOSE")}, {"A", N_("SELECT")}, {"B", N_("BACK")}};
  static const ui::Prompt kTog[] = {{"updown", N_("CHOOSE")}, {"A", N_("CHANGE")}, {"B", N_("BACK")}};
  static const ui::Prompt kSli[] = {{"updown", N_("CHOOSE")}, {"leftright", N_("ADJUST")}, {"B", N_("BACK")}};
  static const ui::Prompt kCho[] = {{"updown", N_("CHOOSE")}, {"leftright", N_("CHANGE")}, {"A", N_("SELECT")}, {"B", N_("BACK")}};
  hp = kPlain;
  if (cur_ >= 0 && size_t(cur_) < e_.size()) {
    switch (e_[size_t(cur_)].kind) {
      case EntryKind::Toggle: hp = kTog, hn = 3; break;
      case EntryKind::Slider: hp = kSli, hn = 3; break;
      case EntryKind::Choice: hp = e_[size_t(cur_)].cycle ? kCho : kAct, hn = e_[size_t(cur_)].cycle ? 4 : 3; break;
      case EntryKind::Action:
      case EntryKind::Text: hp = kAct, hn = 3; break;
      default: break;
    }
  }
  if (spec_.help && spec_.help_n) hp = spec_.help, hn = spec_.help_n;
  if (hk_ != hp || hn_ != hn || hlang_ != i18n::language() || t_hl_.size() != hn) {
    hk_ = hp, hn_ = hn, hlang_ = i18n::language();
    t_hl_.assign(hn, ui::Txt());
    t_hg_.assign(hn, ui::Txt());
    h_icon_.assign(hn, 0);
    h_iw_.assign(hn, 0.0f);
  }
  const float kd = fr.kd, gap = P(24), hpx = float(px.help);  // keycap = 1.6 x the help text (at least the designed 24)
  const float hcy = H - mg - kd / 2.0f;
  float total = 0;
  for (size_t i = 0; i < hn; ++i) {
    if (!t_hl_[i].alive(a)) set_txt(a, t_hl_[i], sentence(tr(hp[i].text)), int(hpx), W * 0.25f, sk == 2 || sk == 3);
    total += kd + P(8) + t_hl_[i].w + (i + 1 < hn ? gap : 0.0f);
  }
  if (remake::audit_sink()) remake::audit(W - mg - total, hcy - kd / 2.0f, total, kd, "ayuda", 'C');
  float hx = W - mg - total;
  for (size_t i = 0; i < hn; ++i) {
    const std::string key = hp[i].icon;
    const bool letter = key == "A" || key == "B";
    const bool main_key = key == "A";
    Color kbg, kfg;
    switch (sk) {
      case 2: kbg = dk ? rgbc(0xFFFFFF) : rgbc(0x12163A), kfg = dk ? rgbc(0x2540FF) : rgbc(0xF3F1EA); break;
      case 3: kbg = main_key ? rgbc(0xFF5A1F) : rgbc(0xF5F4EE), kfg = main_key ? rgbc(0xFFFFFF) : rgbc(0x1B1A17); break;
      case 1: kbg = main_key ? rgbc(0xFFFFFF) : (dk ? rgbc(0xFFFFFF, 56) : rgbc(0x1D1B3A, 40)), kfg = main_key ? rgbc(0x3B2FA0) : fg; break;
      default: kbg = main_key ? fg : (dk ? rgbc(0xFFFFFF, 46) : rgbc(0x14161C, 30)), kfg = main_key ? page : fg; break;
    }
    if (sk == 2) out.push_back(Op::rect(hx, hcy - kd / 2.0f, kd, kd, kbg));
    else {
      shapes_.disc(a, out, hx, hcy - kd / 2.0f, int(kd), kbg);
      if (sk == 3 && !main_key) shapes_.ring(a, out, hx, hcy - kd / 2.0f, kd, kd, int(kd / 2.0f), P(2), rgbc(0x1B1A17));
    }
    if (letter) {
      if (!t_hg_[i].alive(a)) set_txt(a, t_hg_[i], key, px.foot, 0, sk == 3);
      ui::put_text(out, t_hg_[i], hx + (kd - t_hg_[i].w) / 2.0f, hcy, kfg);
    } else {
      if (h_icon_[i] == 0 && h_iw_[i] == 0.0f) {
        ImgId id = 0;
        h_iw_[i] = a.file(cx.th.path(std::string("./assets/help-") + key + ".png"), id) ? 1.0f : -1.0f;
        h_icon_[i] = id;
      }
      if (h_iw_[i] > 0) {
        const float isz = std::max(P(14), std::round(kd * 0.5f)), ar = float(a.img(h_icon_[i]).w) / std::max(float(a.img(h_icon_[i]).h), 1.0f);
        out.push_back(Op::image(h_icon_[i], std::round(hx + (kd - isz * ar) / 2.0f), std::round(hcy - isz / 2.0f), isz * ar, isz, kfg));
      }
    }
    hx += kd + P(8);
    ui::put_text(out, t_hl_[i], hx, hcy, sk == 3 ? fg : (dk || sk == 2 ? ui::with_alpha(fg, 0.9f) : fg));
    hx += t_hl_[i].w + gap;
  }

  // the hint of the entry under the cursor, at the left of the help row
  // Only the room that really is left of the keys (never a minimum that would run into them); with less than a few letters of room it is not drawn.
  const float hint_room = W - 2.0f * mg - total - gap;
  if (cur_ >= 0 && size_t(cur_) < e_.size() && !e_[size_t(cur_)].hint_text.empty() && hint_room >= hpx * 5.0f) {
    if (hint_for_ != cur_) hint_for_ = cur_, t_hint_.reset();
    if (!t_hint_.alive(a)) set_txt(a, t_hint_, e_[size_t(cur_)].hint_text, px.help, hint_room, false);
    ui::put_text(out, t_hint_, mg, hcy, ui::with_alpha(fg, 0.7f));
    if (remake::audit_sink()) remake::audit(mg, hcy - t_hint_.h / 2.0f, t_hint_.w, t_hint_.h, "pista", 'C');
  }
}

}  // namespace zf
