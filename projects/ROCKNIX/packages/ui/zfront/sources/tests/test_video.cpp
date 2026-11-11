// `video` + `vpreview`: geometry, the frame store, the player with a FAKE decoder (no libvlc, no thread: `set_engine_for_test(..., false)`),
// and the rules of the list preview: nothing starts when ShowVideoPreviews is off, the game has no video, libvlc is missing or a screensaver
// is on; the same request twice starts once; stop on every way out; ES defaults of the two settings.
#include <memory>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "screensaver.hpp"
#include "video.hpp"
#include "vpreview.hpp"

using namespace zf;
using std::string;

namespace {

struct Fake : video::Engine {
  int usable_calls = 0, starts = 0, stops = 0, restarts = 0, releases = 0;
  bool can_load = true, can_play = true;
  State st = State::Playing;
  video::Request last;
  video::FrameStore* store = nullptr;
  bool usable() override {
    ++usable_calls;
    return can_load;
  }
  bool start(const video::Request& r, video::FrameStore& s) override {
    if (!can_play) return false;
    ++starts;
    last = r;
    store = &s;
    st = State::Playing;
    s.configure(4, 2);
    return true;
  }
  State state() override { return st; }
  bool restart() override {
    ++restarts;
    st = State::Playing;
    return true;
  }
  void stop() override { ++stops; }
  void release() override { ++releases; }
  void frame() {  // what a decoder thread does for one picture
    uint8_t* p = store->begin_write();
    if (p)
      for (int i = 0; i < 4 * 2 * 4; ++i) p[i] = uint8_t(i);
    store->publish();
  }
};

Fake* install() {
  auto f = std::make_unique<Fake>();
  Fake* raw = f.get();
  video::set_engine_for_test(std::move(f), false);
  vpreview::reset_for_test();
  return raw;
}

video::Request req(const string& path, bool loop = true, bool mute = true) {
  video::Request r;
  r.path = path;
  r.box_w = 200;
  r.box_h = 100;
  r.loop = loop;
  r.mute = mute;
  return r;
}

void test_geometry() {
  uint32_t w, h;
  video::fit_dims(1920, 1080, 200, 100, false, w, h);  // fits inside: height bound
  CHECK(w == 178 && h == 100);
  video::fit_dims(1920, 1080, 200, 100, true, w, h);   // covers: width bound
  CHECK(w == 200 && (h == 112 || h == 113));
  video::fit_dims(100, 50, 400, 400, false, w, h);     // never enlarged: the GPU does that
  CHECK(w == 100 && h == 50);
  video::fit_dims(0, 0, 10, 10, false, w, h);
  CHECK(w >= 2 && h >= 2);
  float x, y, pw, ph;
  video::place(100, 50, 10, 20, 200, 200, false, x, y, pw, ph);  // centred, fitted
  CHECK(pw == 200.0f && ph == 100.0f && x == 10.0f && y == 70.0f);
  video::place(100, 50, 0, 0, 200, 200, true, x, y, pw, ph);     // cover: overflows the box, cropped by a clip
  CHECK(ph == 200.0f && pw == 400.0f && x == -100.0f);
}

void test_store() {
  video::FrameStore s;
  video::FrameStore::Frame f;
  CHECK(s.begin_write() == nullptr);  // before configure
  CHECK(!s.acquire(f) && !s.has_frame());
  int wakes = 0;
  s.set_on_frame([&wakes] { ++wakes; });
  s.configure(2, 2);
  for (int round = 0; round < 6; ++round) {  // the writer never gets the buffer the reader holds or the newest
    uint8_t* p = s.begin_write();
    CHECK(p != nullptr);
    p[0] = uint8_t(round + 1);
    s.publish();
    CHECK(s.acquire(f));
    CHECK(f.px[0] == round + 1 && f.w == 2 && f.h == 2);
    CHECK(!s.acquire(f));    // no NEW frame the second time
    CHECK(f.px != nullptr);  // but the pixels are still there
    s.take_pending();
  }
  CHECK(wakes == 6);
  // one wake-up per frame the loop has not seen: three publishes without take_pending wake once
  wakes = 0;
  for (int i = 0; i < 3; ++i) {
    s.begin_write();
    s.publish();
  }
  CHECK(wakes == 1);
  CHECK(s.take_pending() && !s.take_pending());
  s.reset(7);  // a new session: every frame forgotten
  CHECK(!s.has_frame() && s.session() == 7 && !s.acquire(f));
}

void test_player() {
  Fake* f = install();
  std::vector<bool> duck;
  video::set_duck_hook([&duck](bool d) { duck.push_back(d); });
  int wakes = 0;
  video::set_wake([&wakes] { ++wakes; });
  CHECK(!video::wanted() && !video::has_frame() && f->usable_calls == 0 && f->starts == 0);  // nothing before the first play

  CHECK(video::play(req("/v/a.mp4")));
  CHECK(f->starts == 1 && f->usable_calls == 1 && video::wanted() && video::current_path() == "/v/a.mp4");
  CHECK(f->last.mute && f->last.box_w == 200);
  CHECK(duck.empty());                  // muted: the music is not ducked
  CHECK(video::play(req("/v/a.mp4")));  // the same request: nothing happens
  CHECK(f->starts == 1 && f->stops == 0);
  CHECK(!video::has_frame());  // the picture of the game stays until the first frame
  f->frame();
  CHECK(wakes == 1 && video::has_frame() && video::take_frame() && !video::take_frame());
  video::FrameStore::Frame fr;
  CHECK(video::acquire(fr) && fr.w == 4 && fr.h == 2);

  CHECK(video::play(req("/v/b.mp4", true, false)));  // another file replaces the first; audio on: the music ducks
  CHECK(f->stops == 1 && f->starts == 2 && duck.size() == 1 && duck.back());
  CHECK(!video::has_frame());  // frames of the old video are never shown
  f->frame();
  CHECK(video::has_frame());
  video::stop();
  CHECK(f->stops == 2 && !video::wanted() && !video::has_frame() && duck.size() == 2 && !duck.back());
  video::stop();  // nothing to stop
  CHECK(f->stops == 2);

  // loop: the end starts it again; without loop the end is reported once
  CHECK(video::play(req("/v/c.mp4", true)));
  f->st = Fake::State::Ended;
  video::poll_for_test();
  CHECK(f->restarts == 1 && !video::take_ended());
  CHECK(video::play(req("/v/d.mp4", false)));
  f->st = Fake::State::Ended;
  video::poll_for_test();
  CHECK(f->restarts == 1 && video::take_ended() && !video::take_ended());
  video::poll_for_test();  // reported once, not again
  CHECK(!video::take_ended());
  // an error: failed, once
  CHECK(video::play(req("/v/e.mp4")));
  f->st = Fake::State::Error;
  video::poll_for_test();
  CHECK(video::take_failed() && !video::take_failed());
  // a file that does not start
  f->can_play = false;
  CHECK(video::play(req("/v/f.mp4")));
  CHECK(video::take_failed());
  // refused requests
  video::Request bad = req("");
  CHECK(!video::play(bad));
  bad = req("/v/x.mp4");
  bad.box_w = 0;
  CHECK(!video::play(bad));
  video::shutdown();
  CHECK(f->releases >= 1 && !duck.back());
  video::set_duck_hook(nullptr);
  video::set_wake(nullptr);

  // libvlc missing: unavailable, failed, and no second try
  Fake* g = install();
  g->can_load = false;
  CHECK(video::play(req("/v/a.mp4")));
  CHECK(video::unavailable() && video::take_failed() && g->starts == 0);
  CHECK(!video::play(req("/v/b.mp4")) && g->usable_calls == 1);
  video::set_engine_for_test(nullptr, false);
  video::reset_for_test();
}

void test_box() {
  video::box_reset();
  CHECK(!video::box_get().valid());
  video::box_offer(1, 2, 4, 4);  // too small
  CHECK(!video::box_get().valid());
  video::box_offer(10, 20, 100, 80);
  video::box_offer(0, 0, 50, 50);  // the biggest offer wins
  const video::Box b = video::box_get();
  CHECK(b.valid() && b.x == 10 && b.w == 100 && b.h == 80);
  video::box_reset();
  CHECK(!video::box_get().valid());
}

void test_placeholder() {
  Assets a;
  const ImgId id = video::frame_id(a);
  CHECK(video::is_frame(a, id) && video::frame_id(a) == id);
}

void test_preview() {
  Ctx cx;
  CHECK(vpreview::enabled(cx.es) && vpreview::audio(cx.es));  // ES defaults: ShowVideoPreviews true, VideoAudio true
  std::vector<Op> out;
  Fake* f = install();

  // off: nothing starts, nothing is loaded
  cx.es.set_bool("ShowVideoPreviews", false);
  video::box_reset();
  video::box_offer(0, 0, 200, 100);
  vpreview::draw(cx, out, "/v/a.mp4");
  vpreview::end_frame(false);
  CHECK(f->usable_calls == 0 && f->starts == 0 && !video::wanted() && out.empty() && !vpreview::active());
  cx.es.set_bool("ShowVideoPreviews", true);

  // no video for the game, or no box to put it in
  vpreview::draw(cx, out, "");
  video::box_reset();
  vpreview::draw(cx, out, "/v/a.mp4");
  vpreview::end_frame(false);
  CHECK(f->usable_calls == 0 && f->starts == 0 && out.empty());

  // a screensaver covers the list
  video::box_offer(0, 0, 200, 100);
  screensaver::set_mode(screensaver::Mode::Black);
  vpreview::draw(cx, out, "/v/a.mp4");
  CHECK(f->starts == 0);
  screensaver::set_mode(screensaver::Mode::Off);

  // on: starts muted when VideoAudio is off, shows the picture only once a frame is there
  cx.es.set_bool("VideoAudio", false);
  vpreview::draw(cx, out, "/v/a.mp4");
  CHECK(f->starts == 1 && f->last.mute && f->last.loop && f->last.box_w == 200 && f->last.box_h == 100 && out.empty() && vpreview::active());
  vpreview::draw(cx, out, "/v/a.mp4");  // the next frame of the same list: no second start
  CHECK(f->starts == 1);
  CHECK(!vpreview::hides("/v/a.mp4", 0, 0, 200, 100));  // no frame yet: the picture of the game stays
  f->frame();
  vpreview::draw(cx, out, "/v/a.mp4");
  CHECK(out.size() == 1 && out[0].kind == Op::Image && video::is_frame(cx.assets, out[0].id));
  // while its video shows, what the theme draws inside the video's box (the picture, a narrower logo card) is not drawn: it showed at
  // the sides of a narrower video on the device (9-nov); a box elsewhere (XMB's row icon) or another game's video keep theirs
  CHECK(vpreview::hides("/v/a.mp4", 0, 0, 200, 100));
  CHECK(vpreview::hides("/v/a.mp4", 40, 0, 120, 100));
  // the first picture of a video asks the list for one more full frame: the theme drew the cover before the video in this one, and
  // the next video frames only repaint the video (on the device the cover stayed at its sides)
  CHECK(vpreview::take_fresh());
  CHECK(!vpreview::take_fresh());
  vpreview::draw(cx, out, "/v/a.mp4");
  CHECK(!vpreview::take_fresh());  // the same video again: no extra frame
  CHECK(!vpreview::hides("/v/a.mp4", 250, 0, 100, 100));
  CHECK(!vpreview::hides("/v/b.mp4", 0, 0, 200, 100));
  CHECK(!vpreview::hides("", 0, 0, 200, 100));
  vpreview::end_frame(false);  // drawn this frame: it keeps playing
  CHECK(video::wanted());
  vpreview::end_frame(false);  // nobody drew it (the list was left): it stops
  CHECK(!video::wanted() && !vpreview::active() && f->stops == 1);
  CHECK(!vpreview::hides("/v/a.mp4", 0, 0, 200, 100));  // stopped: the picture comes back

  // the cursor moved to a game without video: stops at once
  video::box_reset();
  video::box_offer(0, 0, 200, 100);
  out.clear();
  vpreview::draw(cx, out, "/v/a.mp4");
  CHECK(video::wanted());
  vpreview::draw(cx, out, "");
  CHECK(!video::wanted());
  // a screensaver comes while it plays
  vpreview::draw(cx, out, "/v/a.mp4");
  CHECK(video::wanted());
  vpreview::end_frame(true);
  CHECK(!video::wanted());

  // libvlc missing: never retried for that file
  video::shutdown();
  Fake* g = install();
  g->can_load = false;
  out.clear();
  vpreview::draw(cx, out, "/v/a.mp4");
  vpreview::draw(cx, out, "/v/a.mp4");
  vpreview::draw(cx, out, "/v/a.mp4");
  CHECK(g->starts == 0 && g->usable_calls == 1 && out.empty());
  video::set_engine_for_test(nullptr, false);
  video::reset_for_test();
  vpreview::reset_for_test();
}

}  // namespace

int main() {
  test_geometry();
  test_store();
  test_player();
  test_box();
  test_placeholder();
  test_preview();
  return zt::done();
}
