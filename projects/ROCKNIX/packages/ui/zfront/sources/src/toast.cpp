// Toast overlay: short messages in rounded pills centred horizontally in the free band just above the help bar (the level OSD,
// ui_osd.cpp, hosted here, stays at the top). The first one sits `kBarGap` (0.35 em) above the help pill and the next ones stack
// upwards, so a toast never covers the header (title, status) or the volume bar and, in a full list, only brushes the last row.
// At rest nothing wakes the loop; with `cx.ui_motion` a toast fades and slides for ~150 ms at each end (`Toasts::moving` keeps
// the frames coming). Toast texts are kept between frames (a few slots keyed by text), so a visible toast costs no allocation.
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "motion.hpp"
#include "ui_osd.hpp"
#include "ui_style.hpp"

namespace zf {

namespace {
ui::Shapes g_shapes;
uint64_t g_owner = 0;  // Assets::uid of the instance the slots belong to (never an address: it can be reused)

// The text image of each visible toast is kept between frames (found by text, no allocation on a warm frame).
struct Slot {
  std::string text;
  int px = 0;
  float max_w = 0;
  ui::Txt t;
};
constexpr size_t kSlots = 6;
Slot g_slots[kSlots];
size_t g_next_slot = 0;

const ui::Txt& toast_text(Assets& a, const std::string& text, int px, float max_w) {
  for (Slot& s : g_slots)
    if (s.px == px && s.max_w == max_w && s.text == text && s.t.alive(a)) return s.t;
  Slot& s = g_slots[g_next_slot];
  g_next_slot = (g_next_slot + 1) % kSlots;
  s.text = text;
  s.px = px;
  s.max_w = max_w;
  s.t.set(a, text, px, max_w);
  return s.t;
}
}  // namespace

namespace {
// The header box published by the screen below is only valid for the frame it was drawn in.
struct ClearHeaderBox {
  ~ClearHeaderBox() { ui::header_box() = ui::HeaderBox(); }
};
}  // namespace

void draw_toasts(Ctx& cx, std::vector<Op>& out) {
  ClearHeaderBox clear_after;
  draw_osd(cx, out);
  const auto& items = cx.sv->toasts.items();
  if (items.empty()) return;
  bool any = false;
  for (const Toast& t : items) any = any || (!t.text.empty() && t.text[0] != '\x1f');
  if (!any) return;

  Assets& a = cx.assets;
  if (g_owner != a.uid()) {
    g_shapes = ui::Shapes();
    for (Slot& sl : g_slots) sl = Slot();
    g_owner = a.uid();
  }
  const ui::Style st = ui::Style::from(cx.th);
  const ui::Metrics m = ui::Metrics::from(cx.th, cx.w, cx.h);
  const int px = m.em;
  float pad_x = std::round(float(px) * 1.1f);
  float h = std::round(float(px) * 2.3f);
  float gap = std::round(float(px) * 0.5f);
  float dot = std::round(float(px) * 0.55f);
  float text_x = dot + std::round(float(px) * 0.6f);
  float max_w = std::min(cx.w * 0.9f, m.col_w) - 2.0f * pad_x - text_x;
  // The stack grows upwards from the help pill; it stops under the list top (never into the header).
  float bottom = ui::help_pill_top(a, cx.th, m) - std::round(float(px) * 0.35f);
  const float limit = m.list_top;

  for (size_t i = 0; i < items.size(); ++i) {
    const Toast& t = items[i];
    if (t.text.empty() || t.text[0] == '\x1f') continue;
    if (bottom - h < limit) break;
    const size_t from = out.size();
    const ui::Txt& tx = toast_text(a, t.text, px, max_w);
    float tw = tx.w, th = tx.h;
    float w = std::round(tw + 2.0f * pad_x + text_x);
    float x = std::round((cx.w - w) / 2.0f);
    float top = bottom - h;
    int r = int(h / 2.0f);
    g_shapes.pill(a, out, x, top + std::max(std::round(h * 0.06f), 1.0f), w, h, Color{0, 0, 0, uint8_t(st.dark ? 90 : 45)});
    g_shapes.framed(a, out, x, top, w, h, r, st.panel, st.line, 1);
    g_shapes.disc(a, out, x + pad_x, top + (h - dot) / 2.0f, int(dot), st.cyan);
    if (!tx.blank) out.push_back(Op::image(tx.id, x + pad_x + text_x, std::floor(top + (h - th) / 2.0f + 0.5f), tw, th, st.txt));
    if (cx.ui_motion) {  // rises into place with a fade, leaves with a fade and a small sink
      const auto now = std::chrono::steady_clock::now();
      const float age = std::chrono::duration<float>(now - t.since).count();
      const float left = std::chrono::duration<float>(t.until - now).count();
      const float p = motion::ease_out(age / kToastIn);
      const float q = std::clamp(left / kToastOut, 0.0f, 1.0f);
      motion::shift_y(out, from, (1.0f - p) * h * 0.6f + (1.0f - q) * h * 0.2f, cx.w, cx.h);
      motion::fade(out, from, std::min(p, q));
    }
    bottom = top - gap;
  }
}

}  // namespace zf
