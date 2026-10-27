#include "ui_osd.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ui_style.hpp"
#include "util.hpp"

namespace zf {

namespace {

constexpr char kMarker[] = "\x1f";  // text of the toast that keeps the event loop awake until the OSD expires

struct OsdState {
  bool active = false;
  char kind[16] = {0};
  float level = 0;
  TimePoint until;
};
OsdState g_osd;

// images of the pill, kept between frames (one Assets instance at a time)
struct OsdCache {
  uint64_t owner = 0;  // Assets::uid (an address can be reused by a later Assets)
  ui::Shapes shapes;
  ui::Txt pct;
  int pct_val = -1, pct_px = 0;
  ui::Txt icon_svg;
  int icon_px = 0;
};
OsdCache g_cache;

}  // namespace

void osd_show_level(Ctx& cx, const char* kind, float level, int ms) {
  g_osd.active = true;
  std::strncpy(g_osd.kind, kind ? kind : "", sizeof g_osd.kind - 1);
  g_osd.kind[sizeof g_osd.kind - 1] = 0;
  g_osd.level = std::min(std::max(level, 0.0f), 1.0f);
  g_osd.until = Clock::now() + std::chrono::milliseconds(ms);
  // a marker toast with the same expiry wakes the loop to hide the pill (skipped when one already covers it)
  bool covered = false;
  for (const Toast& t : cx.sv->toasts.items())
    if (!t.text.empty() && t.text[0] == kMarker[0] && t.until + std::chrono::milliseconds(ms / 4) >= g_osd.until) covered = true;
  if (!covered) cx.sv->toasts.push(kMarker, ms);
}

bool osd_visible() { return g_osd.active && Clock::now() < g_osd.until; }

void osd_clear() { g_osd.active = false; }

OsdBox osd_box(Ctx& cx) {
  float pw = std::round((cx.th.has("volW") ? cx.th.f("volW") : 0.347f) * cx.w);
  float ph = std::round((cx.th.has("volH") ? cx.th.f("volH") : 0.079f) * cx.h);
  float cy = std::round((cx.th.has("volY") ? cx.th.f("volY") : 0.067f) * cx.h);
  OsdBox b{std::round(cx.w / 2.0f - pw / 2.0f), std::round(cy - ph / 2.0f), pw, ph};
  const ui::HeaderBox& hb = ui::header_box();
  if (hb.valid) {
    float gx = std::round(0.02f * cx.w), gy = std::max(std::round(0.015f * cx.h), 3.0f);
    bool straddle = b.y < hb.line_y + 1.0f && b.y + b.h > hb.line_y;                    // lies across the hairline
    if (straddle) {  // the theme's slot cuts the hairline (3:2 layouts): use the header band, centred on the title row
      float y = std::round((hb.title_y0 + hb.title_y1) / 2.0f - b.h / 2.0f);
      b.y = std::max(0.0f, std::min(y, hb.line_y - 1.0f - b.h));
    }
    bool beside = b.x < hb.title_x1 + gx && b.x + b.w > hb.title_x0 - gx;               // overlaps the title's columns
    bool in_band = b.y < hb.line_y + gy && b.y + b.h > hb.title_y0 - gy;                // and its rows
    if ((beside && in_band) || (b.y < hb.line_y + 1.0f && b.y + b.h > hb.line_y)) b.y = hb.line_y + gy;  // no room beside the title
  }
  return b;
}

void draw_osd(Ctx& cx, std::vector<Op>& out) {
  if (!g_osd.active) return;
  TimePoint now = Clock::now();
  if (now >= g_osd.until) {
    g_osd.active = false;
    return;
  }
  // self-heal: the toast queue holds 4 items and may have dropped our marker; make sure one covers the remaining time
  bool covered = false;
  for (const Toast& t : cx.sv->toasts.items())
    if (!t.text.empty() && t.text[0] == kMarker[0] && t.until + std::chrono::milliseconds(30) >= g_osd.until) covered = true;
  if (!covered) {
    int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(g_osd.until - now).count());
    cx.sv->toasts.push(kMarker, std::max(left, 1));
  }

  Assets& a = cx.assets;
  if (g_cache.owner != a.uid()) {
    g_cache = OsdCache();
    g_cache.owner = a.uid();
  }
  const ui::Style st = ui::Style::from(cx.th);
  OsdBox ob = osd_box(cx);
  float pw = ob.w, ph = ob.h;
  float px0 = ob.x, py0 = ob.y, cy = py0 + ph / 2.0f;
  int tpx = ui::px_floor(std::floor((cx.th.has("fVol") ? cx.th.f("fVol") : 0.029f) * cx.h));

  // soft shadow, then the pill with a hairline border
  g_cache.shapes.pill(a, out, px0, py0 + std::max(std::round(ph * 0.06f), 1.0f), pw, ph, Color{0, 0, 0, uint8_t(st.dark ? 80 : 40)});
  g_cache.shapes.framed(a, out, px0, py0, pw, ph, int(ph / 2.0f), st.osd_bg, st.line, 1);

  float pad = std::round(ph * 0.34f);
  float x = px0 + pad;
  int ipx = ui::px_floor(std::round(ph * 0.52f));
  bool speaker = std::strcmp(g_osd.kind, "volume") == 0;
  bool sun = std::strcmp(g_osd.kind, "brightness") == 0;
  if (speaker) {
    if (g_cache.icon_px != ipx || !g_cache.icon_svg.alive(a)) {
      ImgId id = 0;
      g_cache.icon_svg.ok = true;
      g_cache.icon_svg.blank = !a.svg(cx.th.path("assets/menu/menu-icon-sound.svg"), uint32_t(ipx), id);
      g_cache.icon_svg.id = id;
      if (!g_cache.icon_svg.blank) g_cache.icon_svg.w = float(a.img(id).w), g_cache.icon_svg.h = float(a.img(id).h);
      g_cache.icon_px = ipx;
    }
    if (!g_cache.icon_svg.blank)
      out.push_back(Op::image(g_cache.icon_svg.id, std::round(x), std::round(cy - g_cache.icon_svg.h / 2.0f), g_cache.icon_svg.w, g_cache.icon_svg.h, st.osd_txt));
  } else if (sun) {
    g_cache.shapes.icon(a, out, ui::Icon::Sun, x, cy - float(ipx) / 2.0f, ipx, st.osd_txt);
  }
  if (speaker || sun) x += float(ipx) + pad * 0.7f;

  int pct = int(std::lround(g_osd.level * 100.0f));
  if (!g_cache.pct.alive(a) || g_cache.pct_val != pct || g_cache.pct_px != tpx) {
    g_cache.pct.set(a, std::to_string(pct) + "%", tpx);
    g_cache.pct_val = pct;
    g_cache.pct_px = tpx;
  }
  float tw_res = a.measure("100%", float(tpx));
  float right = px0 + pw - pad;
  float bar_w = std::max(right - tw_res - pad * 0.7f - x, 6.0f);
  float bh = std::max(std::round(ph * 0.14f), 3.0f);
  g_cache.shapes.pill(a, out, x, cy - bh / 2.0f, bar_w, bh, st.osd_track);
  float fw = std::round(bar_w * g_osd.level);
  if (fw > 0) g_cache.shapes.pill(a, out, x, cy - bh / 2.0f, std::max(fw, bh), bh, st.osd_bar);
  ui::put_text_right(out, g_cache.pct, right, cy, st.osd_txt);
}

}  // namespace zf
