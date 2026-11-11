#include "vpreview.hpp"

#include <cmath>

#include "screensaver.hpp"
#include "services.hpp"
#include "video.hpp"

namespace zf {
namespace vpreview {

namespace {
bool g_mine = false;        // the player plays OUR video (the screensaver may use it too)
bool g_claimed = false;     // a list drew its preview in this frame
std::string g_failed;       // a video that could not be played: not tried again every frame
video::Box g_drawn;         // where the last frame was put (the box of the theme), for `hides`
std::string g_drawn_path;
bool g_fresh = false;       // a video was just put in its box (take_fresh)

void stop_mine() {
  g_drawn = video::Box();
  g_drawn_path.clear();
  if (!g_mine) return;
  g_mine = false;
  video::stop();
}
}  // namespace

bool enabled(const EsSettings& es) { return es.get_bool("ShowVideoPreviews", true); }
bool audio(const EsSettings& es) { return es.get_bool("VideoAudio", true); }

void draw(Ctx& cx, std::vector<Op>& out, const std::string& path) {
  g_claimed = true;
  if (screensaver::mode() != screensaver::Mode::Off) {  // a screensaver covers the list: no preview behind it
    stop_mine();
    return;
  }
  const video::Box box = video::box_get();
  if (path.empty() || !box.valid() || !enabled(cx.es) || path == g_failed) {
    stop_mine();
    return;
  }
  video::Request rq;
  rq.path = path;
  rq.box_w = uint32_t(std::lround(box.w));
  rq.box_h = uint32_t(std::lround(box.h));
  rq.mute = !audio(cx.es);  // ES: libvlc_audio_set_mute unless VideoAudio
  rq.loop = true;
  rq.vlc_cmdline = cx.conf.get_or("vlc.commandline", "");
  rq.vlc_options = cx.conf.get_or("vlc.options", "");
  if (!video::play(rq)) {  // libvlc is missing: never again this run
    g_failed = path;
    stop_mine();
    return;
  }
  g_mine = true;
  if (video::take_failed()) {
    g_failed = path;
    stop_mine();
    return;
  }
  if (!video::has_frame()) return;  // the picture of the game stays until the first frame
  uint32_t fw = 0, fh = 0;
  video::frame_dims(fw, fh);
  float x, y, w, h;
  video::place(float(fw), float(fh), box.x, box.y, box.w, box.h, false, x, y, w, h);
  out.push_back(Op::image(video::frame_id(cx.assets), std::round(x), std::round(y), std::round(w), std::round(h)));
  if (g_drawn_path != path) g_fresh = true;
  g_drawn = box;
  g_drawn_path = path;
}

void end_frame(bool saver_on) {
  if (g_mine && (!g_claimed || saver_on)) stop_mine();
  g_claimed = false;
}

bool active() { return g_mine; }

bool take_fresh() {
  const bool f = g_fresh;
  g_fresh = false;
  return f;
}

bool hides(const std::string& video, float x, float y, float w, float h) {
  if (video.empty() || !g_mine || video != g_drawn_path || !g_drawn.valid() || !video::has_frame() || video::current_path() != video) return false;
  const float e = 1.0f;  // whole-pixel rounding of the themes
  return x >= g_drawn.x - e && y >= g_drawn.y - e && x + w <= g_drawn.x + g_drawn.w + e && y + h <= g_drawn.y + g_drawn.h + e;
}

void build_preview_row(MenuBuilder& m, Ctx& cx) {
  Ctx* c = &cx;
  m.toggle(tr("SHOW VIDEO PREVIEWS"), [c] { return enabled(c->es); }, [c](bool on) {
    c->es.set_bool("ShowVideoPreviews", on);
    std::string err;
    if (!c->es.save(&err)) c->sv->toasts.push(tr("Could not save the settings"));
    if (!on) stop_mine();
  });
}

void build_audio_row(MenuBuilder& m, Ctx& cx) {
  Ctx* c = &cx;
  m.toggle(tr("ENABLE VIDEO PREVIEW AUDIO"), [c] { return audio(c->es); }, [c](bool on) {
    c->es.set_bool("VideoAudio", on);
    std::string err;
    if (!c->es.save(&err)) c->sv->toasts.push(tr("Could not save the settings"));
  });
}

void reset_for_test() {
  g_mine = g_claimed = false;
  g_failed.clear();
}

}  // namespace vpreview
}  // namespace zf
