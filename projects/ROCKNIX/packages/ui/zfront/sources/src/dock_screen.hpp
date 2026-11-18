// Drawing of the bedside clock and of the plug-in notice (dock_screen.cpp). The logic is in dock.hpp; these functions only draw.
// Both append to `out` like the other overlays and allocate nothing on a warm frame.
#pragma once
#include <vector>

#include "dock.hpp"
#include "gfx.hpp"

namespace zf {

struct Ctx;

namespace dock {

/// Full-screen bedside clock (`screensaver::Mode::Clock`): a dark night screen (the Zenith dark colours, whatever the theme: it is meant for
/// a dark room) and, centred, a card with the time, the day, a thin battery bar and its percentage, moved by `off` cells (burn-in drift) inside
/// the margins. Time and battery come from the raw status (`ui_prefs::bedside_time`, `ui_prefs::raw_battery`): the SHOW CLOCK / SHOW BATTERY
/// choices of the status bar do not hide them here. Every size is a whole number of cells of `remake::Grid` or a text level (`ui::level_px`,
/// which carries TEXT SIZE); the card is reported to the layout auditor as `dock-card`.
void draw_clock(Ctx& cx, Offset off, int64_t wall_minute, std::vector<Op>& out);

/// The state of the plug-in notice, set by `App` before each frame (a plain global, like the OSD pill: one App per process).
struct PillState {
  bool on = false;
  float t = 0;          // 0..1 through the notice (Dock::anim_t)
  bool motion = true;   // false: the pill still, with no slide and no light
};
PillState& pill_state();

/// The notice: a pill "Charging  78%" (the theme's panel colours) that comes down from the top edge to `slot_top` (the first toast slot, snapped
/// down to a cell line), with a light running along its bottom edge. Called by `draw_toasts` right after the OSD pill. Returns the bottom edge
/// of the pill at rest while it is on screen (the toasts start under it) and 0 when nothing is drawn. The resting box is reported as `dock-pill`.
float draw_plug_pill(Ctx& cx, float slot_top, std::vector<Op>& out);

}  // namespace dock
}  // namespace zf
