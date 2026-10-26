// On-screen display for a level that changed (volume, brightness): a slim pill at the top centre with an icon, a bar and
// the percentage, same geometry as the Zenith volume pill (theme variables volW volH volY fVol). It shows for a couple
// of seconds and hides by itself: it rides on a toast "marker" so the event loop wakes up at the right time and an idle
// screen costs nothing. There is no fade (an overlay gets no ticks).
//
//   osd_show_level(cx, "volume", 0.62f);       // 62 %
//   osd_show_level(cx, "brightness", 0.5f);
#pragma once
#include <vector>

#include "app.hpp"

namespace zf {

/// `kind`: "volume" (speaker icon), "brightness" (sun) or anything else (no icon). `level` 0..1 is clamped.
/// Calling it again while visible just updates the bar and extends the time.
void osd_show_level(Ctx& cx, const char* kind, float level, int ms = 1600);
/// True while the pill is on screen (tests).
bool osd_visible();
/// Hides it now.
void osd_clear();
/// Draws the pill (called by `draw_toasts`).
void draw_osd(Ctx& cx, std::vector<Op>& out);

/// Rectangle of the pill: the theme's volW/volH/volY. When it would cut the header hairline of the menu under it
/// (`ui::header_box()`) it moves into the header band, centred on the title row; when it would then touch the title (long
/// titles) it goes just below the hairline. Toasts stack below this slot.
struct OsdBox {
  float x = 0, y = 0, w = 0, h = 0;
};
OsdBox osd_box(Ctx& cx);
/// Top edge of the first toast: below the OSD slot and the header hairline, `gap` pixels away from them.
float toast_slot_top(Ctx& cx, float gap);

}  // namespace zf
