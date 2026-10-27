// Carousel glide: it answers the instant a key is pressed, never overshoots, never goes backwards while a held key moves the
// target on, ends in a bounded time and accepts A in the soft tail.
#include <cmath>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "screens.hpp"

using namespace zf;

static void fill(Ctx& cx, int n) {
  for (int i = 0; i < n; ++i) cx.items.push_back(SystemItem{"sys" + std::to_string(i), "Sys", 1});
}

int main() {
  const float frame = 1.0f / 60.0f;

  {  // one step: the first frame already moves a visible part of the distance, the glide is monotonic and ends under 0.5 s
    Ctx cx;
    fill(cx, 8);
    CarouselScreen c(0);
    c.tick(frame, cx);  // first tick reads the "continue playing" row
    CHECK(!c.tick(frame, cx));  // at rest nothing to draw
    c.handle(Action::Right, cx);
    CHECK(c.tick(frame, cx));
    const float first = c.camera();
    CHECK(first > 0.2f && first < 0.3f);  // 1 - exp(-16/60) = 0.234: no ease-in, the key is felt on the first frame
    float prev = first;
    int frames = 1;
    while (c.tick(frame, cx) && frames < 120) {
      CHECK(c.camera() >= prev && c.camera() <= 1.0f);  // monotonic, never past the target
      prev = c.camera();
      ++frames;
    }
    CHECK(frames < 30);  // 0.5 s at 60 fps
    CHECK(c.camera() == 1.0f);
  }

  {  // frame-rate independent: 30 fps and 120 fps cover the same distance in the same time
    Ctx a, b;
    fill(a, 8);
    fill(b, 8);
    CarouselScreen ca(0), cb(0);
    ca.handle(Action::Right, a);
    cb.handle(Action::Right, b);
    for (int i = 0; i < 6; ++i) ca.tick(1.0f / 30.0f, a);   // 0.2 s
    for (int i = 0; i < 24; ++i) cb.tick(1.0f / 120.0f, b);  // 0.2 s
    CHECK(std::fabs(ca.camera() - cb.camera()) < 0.001f);
  }

  {  // a held key (a press every 0.15 s): the camera only advances, with no restart of the speed at each press
    Ctx cx;
    fill(cx, 12);
    CarouselScreen c(0);
    float prev = 0;
    float max_step = 0;
    for (int press = 0; press < 8; ++press) {
      c.handle(Action::Right, cx);
      for (int f = 0; f < 9; ++f) {  // 0.15 s
        c.tick(frame, cx);
        CHECK(c.camera() >= prev);
        max_step = std::fmax(max_step, c.camera() - prev);
        prev = c.camera();
      }
    }
    CHECK(max_step < 0.27f);  // peak of a single press is 0.234 per frame; a held key adds a little lag, never a restart
    for (int f = 0; f < 60; ++f) c.tick(frame, cx);
    CHECK(c.camera() == 8.0f);
  }

  {  // A is taken in the soft tail of the glide (about 7 px from the end) but not while the camera is still far
    Ctx cx;
    fill(cx, 8);
    CarouselScreen c(0);
    c.handle(Action::Right, cx);
    c.tick(frame, cx);
    CHECK(c.handle(Action::A, cx).kind == Transition::None);  // far from the target: ignored
    for (int f = 0; f < 18; ++f) c.tick(frame, cx);
    CHECK(std::fabs(1.0f - c.camera()) < kGlideAcceptA);  // ~0.3 s: inside the zone where A is accepted
  }

  {  // ES: a held direction on the carousel steps every 200 ms after 0.5 s and never accelerates
    CarouselScreen c(0);
    const Profile p = c.repeat_profile();
    CHECK(p.stages == kSlow.stages);
    CHECK_EQ(repeat_delay_at(p, 0), 500u);
    CHECK_EQ(repeat_delay_at(p, 600), 200u);
    CHECK_EQ(repeat_delay_at(p, 20000), 200u);
  }

  {  // the text under the carousel: fades out, changes system while invisible, fades back in as the camera settles
    Ctx cx;
    fill(cx, 8);
    cx.ui_motion = true;
    CarouselScreen c(0);
    c.tick(frame, cx);
    CHECK_EQ(c.info_item(), 0l);
    CHECK_EQ(c.info_alpha(), 1.0f);
    c.handle(Action::Right, cx);
    float prev = 1.0f;
    bool went_dark = false;
    int frames = 0;
    for (; frames < 90; ++frames) {
      c.tick(frame, cx);
      const float a = c.info_alpha();
      if (!went_dark) {
        CHECK(a <= prev);  // going down
        if (a == 0.0f) went_dark = true;
      } else {
        CHECK(a >= prev);  // then only up
      }
      if (!went_dark) CHECK_EQ(c.info_item(), 0l);  // the old name stays until it is invisible
      prev = a;
      if (went_dark && a == 1.0f) break;
    }
    CHECK(went_dark);
    CHECK_EQ(c.info_alpha(), 1.0f);
    CHECK_EQ(c.info_item(), 1l);
    CHECK(frames < 40);  // under ~0.65 s for the whole fade out + in
    // without motion the text is always the cursor's, fully opaque
    Ctx still;
    fill(still, 8);
    CarouselScreen d(0);
    d.tick(frame, still);
    d.handle(Action::Right, still);
    d.tick(frame, still);
    CHECK_EQ(d.info_item(), 1l);
    CHECK_EQ(d.info_alpha(), 1.0f);
  }

  // wrapping to the left works too
  {
    Ctx cx;
    fill(cx, 5);
    CarouselScreen c(0);
    c.handle(Action::Left, cx);
    for (int f = 0; f < 60; ++f) c.tick(frame, cx);
    CHECK(c.camera() == -1.0f);
  }
  return zt::done();
}
