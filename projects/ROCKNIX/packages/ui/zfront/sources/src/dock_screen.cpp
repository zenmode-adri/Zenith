// Drawing of the bedside clock and of the plug-in notice. Rules followed (reglas-agentes-cpp.md and the owner's rules):
//   * No fixed pixel sizes: positions and boxes are whole cells of `remake::Grid` (u = min(W,H)/90) counted from its margins; the time is a
//     fraction of the SHORT side, the other texts are `ui::Level::Row` (which carries TEXT SIZE).
//   * The drift against burn-in is in cells and is clamped so the card never leaves the margins.
//   * The boxes are reported to the layout auditor (`remake::audit`): `dock-card` and `dock-pill`.
//   * No allocation on a warm frame: the texts are `ui::Txt` slots (rendered again only when the string or the size changes), the date
//     string is rebuilt once a minute, the percentage string when it changes.
#include "dock_screen.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>

#include "app.hpp"
#include "i18n.hpp"
#include "motion.hpp"
#include "remake.hpp"
#include "ui_prefs.hpp"
#include "ui_style.hpp"
#include "util.hpp"

namespace zf {
namespace dock {

namespace {

// The clock is a night screen: the Zenith dark look (#0e1116, cyan accent) whatever the theme, with the text a little dimmed (the panel's
// backlight cannot be lowered from here). Proposal B of preparado/reloj-carga/propuestas.html.
constexpr Color kBg = {14, 17, 22, 255};
constexpr Color kPanel = {22, 27, 34, 255};
constexpr Color kLine = {255, 255, 255, 28};
constexpr Color kTxt = {176, 184, 196, 255};
constexpr Color kDim = {104, 112, 124, 255};
constexpr Color kAccent = {34, 211, 238, 255};
// The plug-in notice has a FIXED palette too (proposal 3 of the same file, .pill): #161b22, 1 px #ffffff1c, text #b0b8c4, percentage and light #22d3ee,
// on any theme. Its shadow (0 1.6cqmin 4cqmin #0008) is made of stacked, growing rounded rectangles: there is no blur primitive.
constexpr Color kPillBg = {22, 27, 34, 255};

using remake::Grid;

ui::Shapes g_sh;

// ---- grid helpers ------------------------------------------------------------------------------------------------------------------

// Smallest rectangle on the cell lines (x from the side margin, y from the top margin) that holds [x, x + w] x [y, y + h].
struct CellBox {
  float x, y, w, h;
};
CellBox snap_out(const Grid& g, float x, float y, float w, float h) {
  const float u = float(g.u), mx = float(g.mx), my = float(g.my);
  const float x0 = mx + std::floor((x - mx) / u + 0.001f) * u;
  const float y0 = my + std::floor((y - my) / u + 0.001f) * u;
  const float x1 = mx + std::ceil((x + w - mx) / u - 0.001f) * u;
  const float y1 = my + std::ceil((y + h - my) / u - 0.001f) * u;
  return {x0, y0, x1 - x0, y1 - y0};
}

// Top-left of a (cw x ch) block centred on the screen and moved by `off` cells. The block (plus one cell of slack for the snap) stays inside
// the margins: the drift gives way before the margin does. Centred for real at zero offset.
void place(const Grid& g, float cw, float ch, Offset off, float& x, float& y) {
  const float u = float(g.u);
  float cx = g.w / 2.0f + float(off.dx) * u, cy = g.h / 2.0f + float(off.dy) * u;
  const float lo_x = float(g.mx) + u + cw / 2.0f, hi_x = g.w - float(g.mx) - u - cw / 2.0f;
  const float lo_y = float(g.my) + u + ch / 2.0f, hi_y = g.h - float(g.my) - u - ch / 2.0f;
  cx = lo_x <= hi_x ? std::clamp(cx, lo_x, hi_x) : g.w / 2.0f;
  cy = lo_y <= hi_y ? std::clamp(cy, lo_y, hi_y) : g.h / 2.0f;
  x = std::round(cx - cw / 2.0f);
  y = std::round(cy - ch / 2.0f);
}

void report(const char* tag, const CellBox& b) {
  if (remake::audit_sink()) remake::audit(b.x, b.y, b.w, b.h, tag, 'B', false, true);  // a container: its four edges on the cell lines
}

// ---- texts kept between frames ---------------------------------------------------------------------------------------------------

struct Slot {
  uint64_t owner = 0;  // Assets::uid of the instance the picture belongs to
  std::string s;
  int px = 0;
  ui::Txt t;
};
const ui::Txt& slot_text(Assets& a, Slot& sl, std::string_view s, int px) {
  if (sl.owner != a.uid() || sl.px != px || sl.s != s || !sl.t.alive(a)) {
    sl.owner = a.uid();
    sl.s.assign(s);
    sl.px = px;
    sl.t.set(a, s, px);
  }
  return sl.t;
}
Slot g_time, g_day, g_pct, g_label, g_pill_pct;

void put_centred(std::vector<Op>& out, const ui::Txt& t, float cx, float top, Color c) {
  if (!t.blank) out.push_back(Op::image(t.id, std::round(cx - t.w / 2.0f), std::round(top), t.w, t.h, c));
}

// px so that `probe` is `target_w` wide. Probed at 100 px and scaled: one measure per layout, not per frame.
int fit_px(Assets& a, const char* probe, float target_w) {
  const float w100 = std::max(a.measure(probe, 100.0f), 1.0f);
  return std::max(12, int(std::floor(target_w / w100 * 100.0f)));
}

// What depends only on the screen, the TEXT SIZE and the length of the time ("10:42" or "10:42 PM"): recomputed when one changes.
struct Layout {
  uint64_t owner = 0;
  float w = 0, h = 0;
  unsigned gen = ~0u;
  size_t tlen = 0;
  int px_time = 0, px_row = 0;
  float card_w = 0;
};
Layout g_lay;

void ensure_layout(Assets& a, const Grid& g, size_t tlen) {
  if (g_lay.owner == a.uid() && g_lay.w == g.w && g_lay.h == g.h && g_lay.gen == ui::access().gen && g_lay.tlen == tlen) return;
  g_lay = Layout();
  g_lay.owner = a.uid();
  g_lay.w = g.w;
  g_lay.h = g.h;
  g_lay.gen = ui::access().gen;
  g_lay.tlen = tlen;
  g_lay.px_row = g.L(ui::Level::Row);
  const float u = float(g.u), short_side = std::min(g.w, g.h);
  const float avail_w = g.w - 2.0f * float(g.mx) - 2.0f * u;  // one cell of slack each side (see `place`)
  const float avail_h = g.h - 2.0f * float(g.my) - 2.0f * u;
  g_lay.card_w = std::min(std::floor(0.72f * g.w / u) * u, std::min(avail_w, 117.0f * u));
  // the time: 24 % of the short side, but never wider than the card minus its padding, and the stack must fit the height
  int px = int(0.24f * short_side);
  px = std::min(px, fit_px(a, tlen > 5 ? "00:00 PM" : "00:00", g_lay.card_w - float(g.U(8))));
  const float rest = float(g_lay.px_row) * 1.5f * 2.0f + float(g.U(5 + 3 + 2 + 2 + 2 + 10));  // two text lines, gaps, bar, padding
  px = std::min(px, int(std::max(avail_h - rest, 0.0f) / 1.3f));
  g_lay.px_time = std::max(12, px);
}

// ---- the date line (rebuilt once a minute) ------------------------------------------------------------------------------------------

constexpr const char* kDays[7] = {NC_("date", "Sun"), NC_("date", "Mon"), NC_("date", "Tue"), NC_("date", "Wed"),
                                  NC_("date", "Thu"), NC_("date", "Fri"), NC_("date", "Sat")};
constexpr const char* kMonths[12] = {NC_("date", "Jan"), NC_("date", "Feb"), NC_("date", "Mar"), NC_("date", "Apr"), NC_("date", "May"), NC_("date", "Jun"),
                                     NC_("date", "Jul"), NC_("date", "Aug"), NC_("date", "Sep"), NC_("date", "Oct"), NC_("date", "Nov"), NC_("date", "Dec")};

struct DateCache {
  int64_t minute = -1;
  unsigned gen = ~0u;
  std::string text;
};
DateCache g_date;

// "SUN 15 NOV" in the language of the interface, upper case (the boceto's small caps); the names are in the catalogue under the context "date".
const std::string& date_text(int64_t wall_minute) {
  if (g_date.minute != wall_minute || g_date.gen != i18n::generation()) {
    g_date.minute = wall_minute;
    g_date.gen = i18n::generation();
    std::time_t t = std::time_t(wall_minute * 60);
    std::tm tmv{};
    if (!localtime_r(&t, &tmv)) tmv = std::tm{};
    const int wd = std::clamp(tmv.tm_wday, 0, 6), mo = std::clamp(tmv.tm_mon, 0, 11);
    g_date.text = utf8_upper(i18n::ctr("date", kDays[wd])) + " " + std::to_string(tmv.tm_mday) + " " + utf8_upper(i18n::ctr("date", kMonths[mo]));
  }
  return g_date.text;
}

struct PctCache {
  int pct = -1;
  std::string text;
};
PctCache g_pct_text;
const std::string& pct_text(int pct) {
  if (g_pct_text.pct != pct) {
    g_pct_text.pct = pct;
    g_pct_text.text = std::to_string(pct) + "%";
  }
  return g_pct_text.text;
}

}  // namespace

// ---- the bedside clock ------------------------------------------------------------------------------------------------------------

void draw_clock(Ctx& cx, Offset off, int64_t wall_minute, std::vector<Op>& out) {
  Assets& a = cx.assets;
  const Grid g(cx.w, cx.h);
  const std::string& raw_time = ui_prefs::bedside_time();  // "HH:MM" (or 12-hour); empty while the clock is not set
  static const std::string kNoTime = "--:--";
  const std::string& time = raw_time.empty() ? kNoTime : raw_time;
  ensure_layout(a, g, time.size());
  out.push_back(Op::rect(0, 0, g.w, g.h, kBg));

  const std::optional<Battery> bat = ui_prefs::raw_battery();
  const int pct = bat ? int(bat->percent) : 0;
  const bool plugged = bat && (bat->plugged || bat->charging);
  const float u = float(g.u);
  const int rpx = g_lay.px_row;

  const ui::Txt& t = slot_text(a, g_time, time, g_lay.px_time);
  const ui::Txt& d = slot_text(a, g_day, date_text(wall_minute), rpx);
  const ui::Txt& p = slot_text(a, g_pct, bat ? pct_text(pct) : std::string(), rpx);
  const float bar_h = 2.0f * u;
  // time, day, thin battery bar, percentage: the gaps are whole cells
  const float gap_d = 2.0f * u, gap_b = 3.0f * u, gap_p = 2.0f * u;
  const float stack = t.h + gap_d + d.h + (bat ? gap_b + bar_h + gap_p + p.h : 0.0f);
  const float pad = float(g.U(5));
  const float cw = g_lay.card_w, ch = stack + 2.0f * pad;
  float x, y;
  place(g, cw, ch, off, x, y);
  const CellBox box = snap_out(g, x, y, cw, ch);
  g_sh.framed(a, out, box.x, box.y, box.w, box.h, g.u * 3, kPanel, kLine, 1);
  report("dock-card", box);

  const float mid = box.x + box.w / 2.0f;
  float cy = box.y + (box.h - stack) / 2.0f;
  put_centred(out, t, mid, cy, kTxt);
  cy += t.h + gap_d;
  put_centred(out, d, mid, cy, kDim);
  cy += d.h;
  if (bat) {
    cy += gap_b;
    const float bw = std::floor(0.6f * box.w / u) * u;
    const float bx = std::round(mid - bw / 2.0f), by = std::round(cy);
    g_sh.pill(a, out, bx, by, bw, bar_h, kLine);
    const float fw = std::floor(bw * float(std::clamp(pct, 0, 100)) / 100.0f);
    if (fw >= bar_h) g_sh.pill(a, out, bx, by, fw, bar_h, plugged ? kAccent : kTxt);
    else if (fw > 0.0f) out.push_back(Op::rect(bx, by, fw, bar_h, plugged ? kAccent : kTxt));
    cy += bar_h + gap_p;
    put_centred(out, p, mid, cy, plugged ? kAccent : kTxt);
  }
}

// ---- the plug-in notice ----------------------------------------------------------------------------------------------------------

PillState& pill_state() {
  static PillState s;
  return s;
}

float draw_plug_pill(Ctx& cx, float slot_top, std::vector<Op>& out) {
  const PillState& ps = pill_state();
  if (!ps.on) return 0.0f;
  Assets& a = cx.assets;
  const Grid g(cx.w, cx.h);
  const float u = float(g.u);
  const std::optional<Battery> bat = ui_prefs::raw_battery();
  const int rpx = g.L(ui::Level::Row);

  const ui::Txt& l = slot_text(a, g_label, tr("Charging"), rpx);
  const ui::Txt& p = slot_text(a, g_pill_pct, bat ? pct_text(int(bat->percent)) : std::string(), rpx);
  const float pad = float(g.U(3));
  const float need = l.w + (bat ? p.w + 2.0f * u : 0.0f) + 2.0f * pad;
  const float room = g.w - 2.0f * float(g.mx);
  float pw = std::min(std::floor(0.7f * g.w / u) * u, room);
  pw = std::min(std::max(pw, std::ceil(need / u) * u), std::floor(room / u) * u);
  const float ph = float(g.cells_over(float(rpx) * 2.4f)) * u;
  const float x = float(g.mx) + std::floor((room - pw) / 2.0f / u) * u;
  // rests on the first cell line at or under the toast slot (under the OSD pill and the menu's header line)
  const float y_end = float(g.my) + std::ceil((slot_top - float(g.my)) / u - 0.001f) * u;
  const float t = ps.t;
  const bool moving = ps.motion;
  float e = 1.0f;  // fades in at once and out over the last fifth
  if (moving) e = std::clamp(std::min(t / 0.08f, (1.0f - t) / 0.2f), 0.0f, 1.0f);
  if (e <= 0.0f) return y_end + ph;
  const float y = moving ? std::round(-ph + (y_end + ph) * motion::ease_out(std::min(t / 0.18f, 1.0f))) : y_end;
  const float cq = 0.01f * std::min(g.w, g.h);  // the boceto's cqmin
  const int r = std::min(int(std::round(6.0f * cq)), int(ph / 2.0f));

  {  // 0 1.6cqmin 4cqmin #0008: ten faint layers up to 4 cqmin wider; each point is covered by fewer of them the further out it is (one at the rim: ~7 %), ~50 % black under the pill
    const float dy = std::round(1.6f * cq);
    for (int k = 9; k >= 0; --k) {
      const float grow = std::round(float(k) * 0.44f * cq);
      g_sh.rrect(a, out, x - grow, y + dy - grow, pw + 2.0f * grow, ph + 2.0f * grow, r + int(grow), Color{0, 0, 0, uint8_t(17.0f * e)});
    }
  }
  g_sh.framed(a, out, x, y, pw, ph, r, ui::with_alpha(kPillBg, e), ui::with_alpha(kLine, e), 1);
  report("dock-pill", {x, y_end, pw, ph});
  if (!l.blank) out.push_back(Op::image(l.id, x + pad, std::round(y + (ph - l.h) / 2.0f), l.w, l.h, ui::with_alpha(kTxt, e)));
  if (bat && !p.blank) out.push_back(Op::image(p.id, std::round(x + pw - pad - p.w), std::round(y + (ph - p.h) / 2.0f), p.w, p.h, ui::with_alpha(kAccent, e)));
  if (moving) {  // a light runs along the bottom edge, inside the rounded ends
    const float ins = float(r);
    const float span = pw - 2.0f * ins;
    const float bar_w = std::max(u, std::floor(span / 4.0f / u) * u), bar_h = std::max(1.0f, std::floor(u / 2.0f));
    if (span > bar_w) {
      const float ph01 = std::fmod(t * 2.0f, 1.0f);
      const float bx = x + ins + std::floor((span - bar_w) * ph01 / u) * u;
      out.push_back(Op::clip(x + ins, y, span, ph));
      out.push_back(Op::rect(bx, y + ph - bar_h - 1.0f, bar_w, bar_h, ui::with_alpha(kAccent, e)));
      out.push_back(Op::unclip());
    }
  }
  return y_end + ph;
}

}  // namespace dock
}  // namespace zf
