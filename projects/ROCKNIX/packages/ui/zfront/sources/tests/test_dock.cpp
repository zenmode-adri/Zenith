// tests/test_dock.cpp: logic of the plug-in notice and of the bedside clock's helpers (dock.hpp). Pure: the clock is injected, nothing sleeps.
// Times are in ms. The wall clock starts 17 s into a minute, so a minute boundary falls on whole seconds.
#include <cstdint>

#include "check.hpp"
#include "dock.hpp"

using namespace zf::dock;

namespace {
constexpr int64_t kWall0 = 60LL * 1000 * 1000 + 17000;  // epoch ms, 17 s into its minute

Clock at(int64_t ms) { return Clock{ms, kWall0 + ms}; }

struct H {
  Dock d;
  explicit H(bool motion = true, bool anim = true) {
    Config c;
    c.motion = motion;
    c.charge_anim = anim;
    d.configure(c);
  }
  bool u(int64_t ms, bool plugged, bool quiet = false) { return d.update(at(ms), Env{plugged, quiet}); }
};
}  // namespace

int main() {
  // ---- settings: Off / 1 / 5 / 10, default 5
  CHECK_EQ(parse_minutes("off"), 0);
  CHECK_EQ(parse_minutes("Off"), 0);
  CHECK_EQ(parse_minutes("1"), 1);
  CHECK_EQ(parse_minutes("5"), 5);
  CHECK_EQ(parse_minutes("10"), 10);
  CHECK_EQ(parse_minutes(""), 0);  // not set: off
  CHECK_EQ(parse_minutes("7"), 0);
  CHECK_EQ(parse_minutes("junk"), 0);

  // ---- starts already plugged in: no plug event, ever
  {
    H h;
    h.u(0, true);
    for (int64_t t = 1000; t <= 60000; t += 1000) h.u(t, true);
    CHECK(!h.d.take_plug_event());
    CHECK(!h.d.anim_active(at(20000)));
    CHECK(h.d.plugged());
    CHECK(!h.d.next_deadline(at(60000)));  // at rest: nothing wakes the loop
  }

  // ---- a real plug-in: only after 3 s steady, and once
  {
    H h;
    h.u(0, false);
    for (int64_t t = 1000; t <= 30000; t += 1000) h.u(t, false);
    CHECK(!h.d.plugged());
    h.u(30000, true);
    h.u(32000, true);
    CHECK(!h.d.take_plug_event());
    CHECK(h.u(33000, true));  // confirmed: the first frame
    CHECK(h.d.take_plug_event());
    CHECK(!h.d.take_plug_event());  // taken
    CHECK(h.d.plugged());
    CHECK(h.d.anim_active(at(33000)));
    const float t = h.d.anim_t(at(33000 + 1200));
    CHECK(t > 0.45f && t < 0.55f);
    CHECK(!h.d.anim_active(at(33000 + 2400)));
  }

  // ---- a plug that bounces at first: one event, and no second one when the status wanders afterwards
  {
    H h;
    h.u(0, false);
    for (int64_t t = 1000; t <= 30000; t += 1000) h.u(t, false);
    h.u(30000, true);
    h.u(31000, true);
    h.u(31500, false);  // bounce before 3 s: nothing yet
    CHECK(!h.d.take_plug_event());
    h.u(32500, true);
    h.u(35000, true);  // only 2.5 s since the bounce
    CHECK(!h.d.take_plug_event());
    h.u(36000, true);  // 3.5 s
    CHECK(h.d.take_plug_event());
    // Full -> Discharging -> Full inside the next seconds: still plugged, still no new event
    h.u(37000, false);
    h.u(40000, false);
    h.u(45000, true);
    h.u(60000, true);
    CHECK(!h.d.take_plug_event());
    CHECK(h.d.plugged());
  }

  // ---- the 8 s flicker seen on the R36S (Full -> Discharging -> Full): it stays plugged, nothing is restarted
  {
    H h;
    h.u(0, true);
    h.u(100000, true);
    h.u(110000, false);
    for (int64_t t = 111000; t <= 117000; t += 1000) h.u(t, false);
    h.u(118000, true);
    CHECK(h.d.plugged());
    h.u(130000, true);
    CHECK(h.d.plugged());
    CHECK(!h.d.take_plug_event());
  }

  // ---- unplugging: confirmed after 15 s steady, never announced
  {
    H h;
    h.u(0, true);
    h.u(400000, false);
    h.u(414000, false);
    CHECK(h.d.plugged());
    h.u(415000, false);
    CHECK(!h.d.plugged());
    CHECK(!h.d.take_plug_event());
  }

  // ---- plugging in during a game: no notice
  {
    H h;
    h.u(0, false, true);
    h.u(30000, true, true);
    h.u(40000, true, true);
    CHECK(h.d.plugged());
    CHECK(!h.d.take_plug_event());
    CHECK(!h.d.anim_active(at(40000)));
  }

  // ---- two plug-ins less than a minute apart: one notice
  {
    H h;
    h.u(0, false);
    h.u(20000, true);
    h.u(23000, true);
    CHECK(h.d.take_plug_event());
    h.u(40000, false);
    h.u(55000, false);  // unplugged (15 s)
    CHECK(!h.d.plugged());
    h.u(56000, true);
    h.u(59000, true);  // plugged again 36 s after the first notice
    CHECK(h.d.plugged());
    CHECK(!h.d.take_plug_event());
  }

  // ---- animation timeline: with motion it asks for frames, without it only the start and the end
  {
    H h(false);
    h.u(0, false);
    h.u(30000, true);
    CHECK(!h.u(32000, true));
    CHECK(h.u(33000, true));
    CHECK(h.d.take_plug_event());
    CHECK(!h.u(34000, true));
    CHECK_EQ(h.d.next_deadline(at(34000)).value(), 33000 + kAnimMs);
    CHECK(h.u(33000 + kAnimMs, true));
    CHECK(!h.d.anim_active(at(33000 + kAnimMs)));
    CHECK(!h.d.next_deadline(at(33000 + kAnimMs + 1)));  // back to rest
    H m(true);
    m.u(0, false);
    m.u(30000, true);
    m.u(33000, true);
    CHECK(m.d.take_plug_event());
    CHECK(m.u(33000 + kFrameMs, true));
    CHECK_EQ(m.d.next_deadline(at(33000 + kFrameMs)).value(), 33000 + 2 * kFrameMs);
    // animation off: the event is taken but nothing draws
    H n(true, false);
    n.u(0, false);
    n.u(30000, true);
    n.u(33000, true);
    CHECK(!n.d.anim_active(at(33000)));
  }

  // ---- the minute: deadline aligned to the change
  {
    CHECK_EQ(ms_to_next_minute(kWall0), 43000 + kMinuteSlackMs);
    CHECK_EQ(ms_to_next_minute(kWall0 + 43000), 60000 + kMinuteSlackMs);
    CHECK_EQ(ms_to_next_minute(kWall0 + 42999), 1 + kMinuteSlackMs);
  }

  // ---- anti burn-in offsets (in grid cells): bounded, different every minute, repeat every 9
  {
    for (int64_t m = 0; m < 200; ++m) {
      Offset o = burn_in_offset(m);
      CHECK(o.dx >= -kShiftX && o.dx <= kShiftX);
      CHECK(o.dy >= -kShiftY && o.dy <= kShiftY);
      Offset p = burn_in_offset(m + 1);
      CHECK(o.dx != p.dx || o.dy != p.dy);
      Offset q = burn_in_offset(m + 9);
      CHECK(o.dx == q.dx && o.dy == q.dy);
    }
    for (int64_t m = 0; m < 9; ++m) {
      Offset o = burn_in_offset(m);
      for (int64_t k = 0; k < m; ++k) {
        Offset e = burn_in_offset(k);
        CHECK(!(e.dx == o.dx && e.dy == o.dy));
      }
    }
    CHECK(burn_in_offset(-5).dx >= -kShiftX);  // a clock before 1970 does not crash
  }

  // ---- stale data: the status source re-reads the battery only every ~30 s, so `update` is called many times with the same old reading.
  // An 8 s blip sampled once must not become "unplugged for 15 s", and the plug must not be announced again afterwards.
  {
    H h;
    auto us = [&](int64_t t, bool plugged, int64_t sample) { return h.d.update(at(t), Env{plugged, false, sample}); };
    us(0, true, 0);
    us(299000, true, 299000);
    us(310000, false, 310000);  // the one reading that caught the blip
    CHECK(h.d.wants_fresh_sample(at(310000)) == false);
    for (int64_t t = 315000; t <= 345000; t += 5000) us(t, false, 310000);  // 35 s of the same stale value
    CHECK(h.d.plugged());
    CHECK(h.d.wants_fresh_sample(at(346000)));  // it is due: the caller must re-read
    CHECK(h.d.next_deadline(at(346000)).value() == 346000 + kResampleMs);  // and the loop does not spin on a past deadline
    us(360000, true, 360000);  // fresh: Full again
    CHECK(h.d.plugged());
    CHECK(!h.d.take_plug_event());
    CHECK(!h.d.wants_fresh_sample(at(361000)));
    // a genuine unplug is confirmed by fresh readings 15 s apart
    us(400000, false, 400000);
    us(410000, false, 400000);
    CHECK(h.d.plugged());
    us(416000, false, 416000);
    CHECK(!h.d.plugged());
  }

  // ---- a charger with its own `online` file (R36 Ultra): the cable out for 1.4 s and back in IS a new plug (no notice was seen)
  {
    H h;
    auto ur = [&](int64_t t, bool plugged, int64_t sample) { return h.d.update(at(t), Env{plugged, false, sample, true}); };
    ur(0, true, 0);  // already charging at start
    ur(300000, true, 300000);
    ur(314800, false, 314800);  // online 1 -> 0
    CHECK(!h.d.plugged());      // at once: nothing to filter
    ur(316200, true, 316200);   // online 0 -> 1, 1.4 s later
    CHECK(!h.d.take_plug_event());  // a plug still needs its 3 s
    CHECK(h.d.wants_fresh_sample(at(319300)));
    ur(319300, true, 319300);
    CHECK(h.d.plugged());
    CHECK(h.d.take_plug_event());  // the notice comes
  }
  // ---- the same 1.4 s without the charger file (R36S, guessed from the gauge): still filtered, no notice
  {
    H h;
    auto ug = [&](int64_t t, bool plugged, int64_t sample) { return h.d.update(at(t), Env{plugged, false, sample, false}); };
    ug(0, true, 0);
    ug(300000, true, 300000);
    ug(314800, false, 314800);
    CHECK(h.d.plugged());
    ug(316200, true, 316200);
    ug(319300, true, 319300);
    CHECK(h.d.plugged());
    CHECK(!h.d.take_plug_event());
  }

  // ---- a plug needs TWO fresh readings at least 3 s apart: one reading, however long it stays the latest, confirms nothing
  {
    H h;
    auto us = [&](int64_t t, bool plugged, int64_t sample) { return h.d.update(at(t), Env{plugged, false, sample}); };
    us(0, false, 0);
    us(29000, false, 29000);
    us(30000, true, 30000);  // the first reading that shows the charger
    for (int64_t t = 31000; t <= 80000; t += 1000) us(t, true, 30000);  // 50 s of the same old value
    CHECK(!h.d.plugged());
    CHECK(!h.d.take_plug_event());
    CHECK(h.d.wants_fresh_sample(at(80000)));
    CHECK(h.d.next_deadline(at(80000)).value() == 80000 + kResampleMs);
    us(81000, true, 81000);  // a second, fresh one
    CHECK(h.d.plugged());
    CHECK(h.d.take_plug_event());
  }

  // ---- the notice is not repeated by the flicker of a console that stays plugged in (60 s), but a real second plug-in later is announced
  {
    H h;
    h.u(0, false);
    h.u(20000, true);
    h.u(23000, true);
    CHECK(h.d.take_plug_event());
    h.u(100000, false);  // unplugged for real (15 s steady)
    h.u(115000, false);
    CHECK(!h.d.plugged());
    h.u(130000, true);
    h.u(133000, true);  // 110 s after the first notice
    CHECK(h.d.take_plug_event());
  }

  return zt::done();
}
