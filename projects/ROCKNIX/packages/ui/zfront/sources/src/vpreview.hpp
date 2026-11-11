// MODULE `vpreview`: the video preview of the game list (ES VideoComponent / VideoVlcComponent inside the gamelist views).
//
// ES: when the cursor rests on a game that has a `video`, the video replaces the game's picture after the theme's `<video>` delay
// (VideoComponent.cpp:281 `delay`, default 0 at :87; zfront's themes have no `<video>` element, so the default applies, on top of the 120 ms
// the list waits before it loads anything for a game), plays in the picture's box, loops, and stops when the cursor moves or a game starts.
//   ShowVideoPreviews  true  (Settings.cpp:32)   menu row SHOW VIDEO PREVIEWS (GuiMenu.cpp:5708, ROCKNIX), in GAMELIST OPTIONS
//   VideoAudio         true  (Settings.cpp:307)  menu row ENABLE VIDEO PREVIEW AUDIO (GuiMenu.cpp:5878), in SOUND SETTINGS
// With ShowVideoPreviews off, a game without video or libvlc missing, nothing starts: no thread, no library, no timer.
//
// `ListScreen::draw` calls `draw` after the theme drew the list (the views offer the box of the game's picture through
// `video::box_offer`); `App::render` calls `end_frame`, which stops a preview nobody drew this frame (the list was left) and
// while a screensaver is on.
#pragma once
#include <string>
#include <vector>

#include "app.hpp"
#include "menu.hpp"

namespace zf {
namespace vpreview {

bool enabled(const EsSettings& es);  // ShowVideoPreviews
bool audio(const EsSettings& es);    // VideoAudio

/// After the list was drawn: starts / keeps / stops the preview of `video` (absolute path, empty = the game has none; only given once the
/// cursor settled) and, when frames are there, adds the picture over the box the theme offered.
void draw(Ctx& cx, std::vector<Op>& out, const std::string& video);
/// After a whole frame was built. `saver_on`: a screensaver is showing.
void end_frame(bool saver_on);
/// A preview is playing / wanted.
bool active();
/// While `video` (the game's) shows, a box of the theme inside the box the preview was drawn in last: the theme leaves out what it would
/// draw there (picture, logo card), as ES hides the image while the video plays, so nothing shows at the sides of a narrower video.
bool hides(const std::string& video, float x, float y, float w, float h);
/// True once after a video was first put in its box: the list draws one more full frame, in which `hides` leaves out the cover (the frame
/// that showed the first picture drew the cover before it, and later video frames only repaint the video).
bool take_fresh();

/// The menu rows (one each, at their ES places).
void build_preview_row(MenuBuilder& m, Ctx& cx);  // SHOW VIDEO PREVIEWS
void build_audio_row(MenuBuilder& m, Ctx& cx);    // ENABLE VIDEO PREVIEW AUDIO

void reset_for_test();

}  // namespace vpreview
}  // namespace zf
