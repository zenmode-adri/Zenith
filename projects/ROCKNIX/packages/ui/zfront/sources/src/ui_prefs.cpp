#include "ui_prefs.hpp"

#include "app.hpp"

namespace zf {
namespace ui_prefs {

namespace {
// One App per process: the raw status and the preferences it was filtered with.
Status g_raw;
Prefs g_prefs;
bool g_have_raw = false;
}  // namespace

Prefs read(const EsSettings& es) {
  Prefs p;
  p.clock = es.get_bool("DrawClock", true);
  p.clock12 = es.get_bool("ClockMode12", false);
  p.network = es.get_bool("ShowNetworkIndicator", true);
  if (const std::string* b = es.get("ShowBattery")) {
    if (b->empty()) p.battery = BatteryMode::Off;
    else if (*b == "text") p.battery = BatteryMode::Text;
    else p.battery = BatteryMode::Icon;
  }
  return p;
}

std::string clock12(const std::string& t) {
  if (t.size() != 5 || t[2] != ':') return t;
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  if (!digit(t[0]) || !digit(t[1]) || !digit(t[3]) || !digit(t[4])) return t;
  int h = (t[0] - '0') * 10 + (t[1] - '0');
  if (h > 23) return t;
  bool pm = h >= 12;
  int h12 = h % 12 == 0 ? 12 : h % 12;
  std::string o;
  o += char('0' + h12 / 10);
  o += char('0' + h12 % 10);
  o += t.substr(2);
  o += pm ? " PM" : " AM";
  return o;
}

Status filter(const Status& raw, const Prefs& p) {
  Status s = raw;
  if (!p.clock) s.time.clear();
  else if (p.clock12) s.time = clock12(s.time);
  if (p.battery == BatteryMode::Off) s.battery.reset();
  if (!p.network) s.wifi = false;
  return s;
}

void on_status(Ctx& cx, const Status& raw) {
  g_raw = raw;
  g_have_raw = true;
  g_prefs = read(cx.es);
  cx.status = filter(raw, g_prefs);
}

void reapply(Ctx& cx) {
  if (!g_have_raw) return;
  g_prefs = read(cx.es);
  cx.status = filter(g_raw, g_prefs);
}

bool has_battery() { return g_have_raw && g_raw.battery.has_value(); }

bool battery_text() { return g_prefs.battery != BatteryMode::Icon; }

}  // namespace ui_prefs
}  // namespace zf
