// The drawing of the bedside clock and of the plug-in notice (dock_screen.cpp) and how App wires them (poll_dock):
//  - a warm frame of the clock and of the notice allocates nothing (the clock is redrawn once a minute, the notice 30 times a second);
//  - the clock's card and the notice stay inside the margins in every size, the card moves with the drift, 12-hour time still fits;
//  - the notice sits under the OSD slot and a toast that arrives meanwhile goes under it;
//  - plugging the charger in (a fresh reading, confirmed) starts the notice and the bedside clock's timer; unplugging ends it; the setting Off
//    and a running game keep it quiet.
// No theme folder needed: the default theme and the test font (ZF_TEST_FONT, as the other tests).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "alloc_nothrow.hpp"
#include "app.hpp"
#include "check.hpp"
#include "dock_screen.hpp"
#include "oscontract.hpp"
#include "remake.hpp"
#include "status.hpp"
#include "ui_prefs.hpp"
#include "ui_style.hpp"

using namespace zf;
using std::chrono::seconds;

static size_t g_allocs = 0;
void* operator new(size_t n) {
  ++g_allocs;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

std::string g_root;

void put(const std::string& path, const std::string& content) {
  for (size_t i = g_root.size() + 1; i < path.size(); ++i)
    if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
  std::ofstream(path, std::ios::trunc) << content;
}

struct DummyScreen : Screen {
  Transition handle(Action, Ctx&) override { return Transition::none(); }
  void draw(Ctx&, std::vector<Op>&) override {}
};

std::string font_path() {
  const char* e = std::getenv("ZF_TEST_FONT");
  return e ? e : "/usr/config/emulationstation/resources/Rubik-Regular.ttf";
}

void make_cx(Ctx& cx, int w, int h) {
  std::string err;
  CHECK(cx.assets.load_font(font_path(), err));
  cx.w = float(w), cx.h = float(h);
  cx.sv->cmd.dry_run = true;
}

void raw_status(Ctx& cx, const char* time, int pct, bool plugged) {
  Status s = Status::demo(time);
  s.battery = Battery{uint8_t(pct), plugged, plugged};
  ui_prefs::on_status(cx, s);
}

// every Image / Rect op of the frame inside [x0, x1] x [y0, y1]
bool inside(const std::vector<Op>& ops, float x0, float y0, float x1, float y1) {
  for (const Op& o : ops)
    if (o.kind == Op::Image || o.kind == Op::Rect) {
      if (o.w >= x1 - x0 && o.h >= y1 - y0) continue;  // the full-screen background
      if (o.x < x0 - 1 || o.y < y0 - 1 || o.x + o.w > x1 + 1 || o.y + o.h > y1 + 1) return false;
    }
  return true;
}

}  // namespace

int main() {
  char tpl[] = "/tmp/zf-dockdraw-XXXXXX";
  g_root = mkdtemp(tpl);
  CHECK(!g_root.empty());
  CHECK(file_exists(font_path()));

  // ---- the clock: warm frames allocate nothing, the card stays inside the margins in every size, at every drift, with 12-hour time too
  const int sizes[][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 540}, {720, 720}, {800, 480}, {854, 480}, {1024, 768}, {1280, 720}};
  for (const auto& sz : sizes) {
    const int w = sz[0], h = sz[1];
    for (const ui::TextSize ts : {ui::TextSize::Normal, ui::TextSize::XLarge}) {
      ui::set_access(ts, false);
      static std::vector<std::unique_ptr<Ctx>> keep;
      keep.push_back(std::make_unique<Ctx>());
      Ctx& cx = *keep.back();
      make_cx(cx, w, h);
      const remake::Grid g{float(w), float(h)};
      raw_status(cx, "10:42", 78, true);
      std::vector<Op> ops;
      for (int i = 0; i < 3; ++i) {
        ops.clear();
        dock::draw_clock(cx, dock::burn_in_offset(0), 1000, ops);
      }
      const size_t before = g_allocs;
      for (int i = 0; i < 30; ++i) {
        ops.clear();
        dock::draw_clock(cx, dock::burn_in_offset(0), 1000, ops);
      }
      if (g_allocs != before) std::printf("MAL %dx%d: %zu reservas en 30 fotogramas del reloj\n", w, h, g_allocs - before);
      CHECK(g_allocs == before);
      for (int m = 0; m < 9; ++m) {
        ops.clear();
        dock::draw_clock(cx, dock::burn_in_offset(m), 1000 + m, ops);
        CHECK(inside(ops, float(g.mx), float(g.my), g.w - float(g.mx), g.h - float(g.my)));
      }
      // 12-hour time ("10:42 PM") is longer: it still fits
      Status pm = Status::demo("22:42");
      pm.battery = Battery{78, true, true};
      cx.es = EsSettings::from_text("<config><bool name=\"ClockMode12\" value=\"true\" /></config>");
      ui_prefs::on_status(cx, pm);
      CHECK(ui_prefs::bedside_time() == "10:42 PM");
      for (int m = 0; m < 9; ++m) {
        ops.clear();
        dock::draw_clock(cx, dock::burn_in_offset(m), 1000 + m, ops);
        CHECK(inside(ops, float(g.mx), float(g.my), g.w - float(g.mx), g.h - float(g.my)));
      }
      cx.es = EsSettings::from_text("<config></config>");
      // SHOW CLOCK / SHOW BATTERY off hide them from the bar, not from the bedside clock
      cx.es = EsSettings::from_text("<config><bool name=\"DrawClock\" value=\"false\" /><string name=\"ShowBattery\" value=\"\" /></config>");
      ui_prefs::on_status(cx, pm);
      CHECK(cx.status.time.empty() && !cx.status.battery);
      CHECK(ui_prefs::raw_battery() && !ui_prefs::bedside_time().empty());
    }
  }
  ui::set_access(ui::TextSize::Normal, false);

  // ---- the notice: warm frames allocate nothing at any point of the animation; under the OSD slot; a toast goes under it
  {
    static Ctx cx;
    make_cx(cx, 640, 480);
    raw_status(cx, "10:42", 78, true);
    std::vector<Op> ops;
    dock::pill_state() = dock::PillState{true, 0.5f, true};
    for (int i = 0; i < 3; ++i) {
      ops.clear();
      draw_toasts(cx, ops);
    }
    CHECK(!ops.empty());
    const size_t before = g_allocs;
    for (int i = 0; i < 60; ++i) {
      ops.clear();
      dock::pill_state() = dock::PillState{true, float(i) / 60.0f, true};
      draw_toasts(cx, ops);
    }
    if (g_allocs != before) std::printf("MAL: %zu reservas en 60 fotogramas de la animacion\n", g_allocs - before);
    CHECK(g_allocs == before);
    // off: nothing at all
    dock::pill_state() = dock::PillState{};
    ops.clear();
    draw_toasts(cx, ops);
    CHECK(ops.empty());
    // the notice alone, and with a toast: the toast starts under the bottom of the notice
    std::vector<remake::Box> boxes;
    remake::audit_sink() = &boxes;
    dock::pill_state() = dock::PillState{true, 0.5f, true};
    ops.clear();
    draw_toasts(cx, ops);
    remake::audit_sink() = nullptr;
    CHECK_EQ(boxes.size(), 1ul);
    const float bottom = boxes.empty() ? 0.0f : boxes[0].y + boxes[0].h;
    std::vector<Op> with;
    cx.sv->toasts.push("Saved", 4000);
    draw_toasts(cx, with);
    cx.sv->toasts.expire(std::chrono::steady_clock::now() + std::chrono::hours(1));
    float top_of_toast = 1e9f;
    for (size_t i = ops.size(); i < with.size(); ++i)
      if (with[i].kind == Op::Image || with[i].kind == Op::Rect) top_of_toast = std::min(top_of_toast, with[i].y);
    CHECK(top_of_toast >= bottom - 1.0f);  // (the ops of the notice come first, then the toast's)
    dock::pill_state() = dock::PillState{};
  }

  // ---- App: a plug-in is confirmed by fresh readings, starts the notice and the bedside clock's timer; unplugging ends it
  {
    const std::string r = g_root + "/sys";
    put(r + "/class/power_supply/battery/capacity", "60");
    put(r + "/class/power_supply/battery/status", "Discharging");
    StatusProbes pr;
    pr.wifi = [] { return false; };
    pr.time = [](std::string& o) { o = "10:00"; };
    pr.bluetooth = [] { return false; };
    pr.uevent_fd = -1;
    Ctx cx;
    make_cx(cx, 640, 480);
    cx.ui_motion = true;
    cx.es = EsSettings::from_text("<config></config>");
    oscontract::reset_for_test();
    App app(std::move(cx), std::make_unique<DummyScreen>(), make_status_source(r, pr));
    const TimePoint t0 = Clock::now();
    auto at = [&](int s) { return t0 + seconds(s); };
    app.poll_status(at(0));
    app.poll_status(at(12));
    CHECK_EQ(oscontract::bedside(), 0);  // unplugged: no bedside clock, the screensaver as always
    CHECK(!dock::pill_state().on);
    // the status source reads the battery on its own cadence (half minute, minute change), so the test steps one second at a time
    app.cx.es.set_string("BedsideClock", "5");
    put(r + "/class/power_supply/battery/status", "Charging");
    int plugged_at = -1;
    for (int s = 13; s <= 120 && plugged_at < 0; ++s) {
      app.poll_status(at(s));
      if (oscontract::bedside() != 0) plugged_at = s;
      else CHECK(!dock::pill_state().on);
    }
    CHECK(plugged_at >= 16);  // two fresh readings, 3 s apart, at least: never the first one alone
    CHECK_EQ(oscontract::bedside(), 5);  // BedsideClock 5
    CHECK(dock::pill_state().on);
    CHECK(oscontract::idle().stamped());
    app.poll_status(at(plugged_at + 1));
    CHECK(dock::pill_state().on && dock::pill_state().t > 0.3f && dock::pill_state().t < 0.5f);
    app.poll_status(at(plugged_at + 3));  // 2.4 s after it began it is over
    CHECK(!dock::pill_state().on);
    CHECK_EQ(oscontract::bedside(), 5);
    app.cx.es.set_string("BedsideClock", "off");
    app.poll_status(at(plugged_at + 4));
    CHECK_EQ(oscontract::bedside(), 0);  // clock Off: the screensaver as always, even plugged in
    app.cx.es.set_string("BedsideClock", "10");
    app.poll_status(at(plugged_at + 5));
    CHECK_EQ(oscontract::bedside(), 10);
    app.cx.es.set_string("BedsideClock", "");
    app.poll_status(at(plugged_at + 6));
    CHECK_EQ(oscontract::bedside(), 0);  // not set: off, even plugged in (the default)
    app.cx.es.set_string("BedsideClock", "10");
    app.poll_status(at(plugged_at + 7));
    CHECK_EQ(oscontract::bedside(), 10);
    // unplug: confirmed only after 15 s of fresh readings
    const int w0 = plugged_at + 10;
    put(r + "/class/power_supply/battery/status", "Discharging");
    int unplugged_at = -1;
    for (int s = w0; s <= w0 + 120 && unplugged_at < 0; ++s) {
      app.poll_status(at(s));
      if (oscontract::bedside() == 0) unplugged_at = s;
    }
    CHECK(unplugged_at >= w0 + 15);
    // a plug-in during a game: no notice
    oscontract::game_started("/r/x.gb", "X");
    put(r + "/class/power_supply/battery/status", "Charging");
    for (int s = unplugged_at + 1; s <= unplugged_at + 100; ++s) {
      app.poll_status(at(s));
      CHECK(!dock::pill_state().on);
    }
    oscontract::game_ended();
    oscontract::reset_for_test();
  }
  return zt::done();
}
