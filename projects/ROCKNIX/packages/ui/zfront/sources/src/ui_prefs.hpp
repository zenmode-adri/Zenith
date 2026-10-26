// UI preferences that change what the always-visible chrome shows: the clock, its 12-hour format and the battery
// (es_settings.cfg `DrawClock`, `ClockMode12`, `ShowBattery`). The status source keeps delivering the raw `Status`;
// `App` hands every new one to `on_status`, which publishes the filtered copy in `cx.status`, so the scenes keep drawing
// "whatever is in cx.status" and nothing is computed per frame. A menu that changes one of these keys calls `reapply`.
//
//   ShowBattery   ""  -> no battery at all        "icon" -> icon only        "text" (default) -> icon and percentage
#pragma once
#include <string>

#include "settings.hpp"
#include "status.hpp"

namespace zf {

struct Ctx;

namespace ui_prefs {

enum class BatteryMode : uint8_t { Off, Icon, Text };

struct Prefs {
  bool clock = true;
  bool clock12 = false;
  BatteryMode battery = BatteryMode::Text;
  bool network = true;  // ShowNetworkIndicator: the WiFi icon
};

/// The keys of `es` (defaults: clock on, 24 h, icon and text, network indicator on).
Prefs read(const EsSettings& es);
/// "13:05" -> "01:05 PM", "00:30" -> "12:30 AM" (ES: strftime "%I:%M %p"). Anything that is not "HH:MM" comes back unchanged.
std::string clock12(const std::string& hhmm);
/// `raw` with the preferences applied: no clock when it is off (12 h text when asked), no battery when it is off.
Status filter(const Status& raw, const Prefs& p);

/// Stores `raw` (the source's status) and sets `cx.status` to its filtered copy. Called by `App` for every new status.
void on_status(Ctx& cx, const Status& raw);
/// Re-reads the keys from `cx.es` and republishes the last raw status. Menus call it after changing one of them.
void reapply(Ctx& cx);
/// The last raw status has a battery (the "SHOW BATTERY STATUS" entry only makes sense then).
bool has_battery();
/// The battery percentage is drawn next to the icon (`ShowBattery` = text). Read by `status_bar`.
bool battery_text();

}  // namespace ui_prefs
}  // namespace zf
