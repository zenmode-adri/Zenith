// MODULE `video`: a video player for the list preview and the random-video screensaver (ES's VideoVlcComponent, in process libvlc).
//
// libvlc is loaded with dlopen on the first play (`libvlc.so.5`, in the image; no headers on the PC): when it is missing the feature is
// simply off (`unavailable()`), no log, no crash. Nothing exists before the first `play` (no thread, no library) and nothing runs
// after `stop` but one idle timer that unloads libvlc 20 s later.
//
// Threads and the path of a frame:
//   main thread      `play` / `stop` / `shutdown` / `take_frame`: they only write a command and signal; libvlc is never called here
//                    (`libvlc_new` scans plugins and the 3.x `stop` joins its threads: 100+ ms each).
//   control thread   created by the first `play`: `Engine::start/stop`, polls `Engine::state` every 250 ms WHILE a video plays (loop,
//                    end, error). Asleep (no timer) otherwise.
//   VLC threads      decode, convert and scale (swscale) straight into one of three RGBA buffers of `FrameStore` (lock / unlock /
//                    display callbacks, `libvlc_video_set_format_callbacks` makes VLC scale to the box). `publish` swaps an index
//                    under a mutex and calls the wake-up of the loop once per unconsumed frame.
//   draw thread      `sdlr.cpp`: on a wake-up redraws the LAST ops (no scene rebuild) and uploads the newest buffer with ONE
//                    `SDL_UpdateTexture` into a streaming texture. Per frame: one write by swscale (VLC's thread), one memcpy
//                    to the texture, no other copy; the buffer hand-over is an index swap.
// The picture goes into the scene as an ordinary `Op::image` of a 1x1 transparent placeholder (`frame_id`); the SDL backend draws its
// streaming texture when it meets that id (`is_frame`), so the CPU renderer and the tests need nothing special.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gfx.hpp"

namespace zf {
namespace video {

struct Request {
  std::string path;
  uint32_t box_w = 0, box_h = 0;  // pixels the picture is shown in; VLC decodes at most this big (never above the file's own size)
  bool cover = false;             // fill the box (the overflow is cropped) instead of fitting inside it (ES: min size / max size)
  bool mute = true;               // no audio stream is opened at all (`:no-audio`)
  bool loop = true;               // restart at the end (ES `handleLooping`); false: `take_ended` reports it
  std::string vlc_cmdline, vlc_options;  // system.cfg `vlc.commandline` (when libvlc is created) and `vlc.options` (per media), as ES
  bool same(const Request& o) const {
    return path == o.path && box_w == o.box_w && box_h == o.box_h && cover == o.cover && mute == o.mute && loop == o.loop;
  }
};

/// Size VLC is asked to decode a `sw x sh` source at for a `bw x bh` box: shrunk to fit (or to cover) the box, never enlarged (the
/// GPU does that), at least 2x2. ES: `ImageIO::adjustPictureSize` in `VideoVlcComponent::onMediaParsed`.
void fit_dims(uint32_t sw, uint32_t sh, uint32_t bw, uint32_t bh, bool cover, uint32_t& ow, uint32_t& oh);
/// Rectangle of a `fw x fh` picture in the box (bx, by, bw, bh): scaled up or down, centred; `cover` fills the box (cropped by a clip).
void place(float fw, float fh, float bx, float by, float bw, float bh, bool cover, float& x, float& y, float& w, float& h);

// ---- frames -----------------------------------------------------------------------------------------------------------
/// Three RGBA buffers handed from the decoder thread to the draw thread: the writer always finds a buffer that is neither the
/// newest nor the one the reader holds, so nobody waits and nothing is copied. The mutex guards only the indexes.
class FrameStore {
 public:
  struct Frame {
    std::shared_ptr<const std::vector<uint8_t>> keep;  // keeps the pixels alive while the reader uses them
    const uint8_t* px = nullptr;
    uint32_t w = 0, h = 0;
  };
  /// Decoder thread, before the first frame (VLC's format callback): the size of the pictures. Replaces the buffers.
  void configure(uint32_t w, uint32_t h);
  /// Decoder thread: the buffer to fill with `w * h * 4` bytes (null before `configure`).
  uint8_t* begin_write();
  /// Decoder thread: the buffer last given by `begin_write` is now the newest; wakes the loop once per consumed frame.
  void publish();
  /// Draw thread: true when a newer frame than the last `acquire` is there; `f` is set whenever there is any frame.
  bool acquire(Frame& f);
  /// Control thread: forgets every frame and tags the next ones with `session`.
  void reset(uint32_t session);
  uint32_t session() const { return session_.load(); }
  bool has_frame() const { return ready_flag_.load(); }
  /// Size of the pictures (0 before the first `configure`).
  void dims(uint32_t& w, uint32_t& h) const;
  /// A frame arrived since the last call (the loop's "something to draw").
  bool take_pending() { return pending_.exchange(false); }
  void set_on_frame(std::function<void()> f) { on_frame_ = std::move(f); }

 private:
  mutable std::mutex m_;
  std::shared_ptr<std::vector<uint8_t>> buf_[3];
  uint32_t w_ = 0, h_ = 0;
  int writing_ = -1, ready_ = -1, held_ = -1;
  bool fresh_ = false;
  std::atomic<bool> ready_flag_{false}, pending_{false};
  std::atomic<uint32_t> session_{0};
  std::function<void()> on_frame_;
};

// ---- the decoder behind the module (libvlc; a fake in the tests) ---------------------------------------------------------
class Engine {
 public:
  enum class State { Playing, Ended, Error };
  virtual ~Engine() = default;
  /// Control thread. Loads the library the first time; false = cannot play at all (`unavailable`).
  virtual bool usable() = 0;
  /// Starts decoding `r.path` into `store` (it calls `configure`, `begin_write` and `publish` from its own threads). False: this file failed.
  virtual bool start(const Request& r, FrameStore& store) = 0;
  virtual State state() = 0;
  /// The loop: play the same media again.
  virtual bool restart() = 0;
  /// Stops and releases the player; when it returns no thread of the engine touches the store any more.
  virtual void stop() = 0;
  /// Unloads libvlc (the next `start` loads it again).
  virtual void release() = 0;
};
/// The libvlc engine (video_vlc.cpp).
std::unique_ptr<Engine> make_vlc_engine();

// ---- the player ---------------------------------------------------------------------------------------------------------
/// Main thread. Plays `r` (replacing what plays); the same request again does nothing. False when libvlc is known to be missing.
bool play(const Request& r);
/// Main thread. Stops (asynchronously); the picture is gone at once.
void stop();
/// Main thread, before a game starts / at exit: stops, unloads libvlc and ends the control thread; returns when VLC is gone (its decoder
/// and audio must not be alive when the emulator starts).
void shutdown();
/// A request is in force (frames may not have come yet) and the path of it.
bool wanted();
std::string current_path();
/// A picture of the current request is there to draw.
bool has_frame();
/// Size of the pictures.
void frame_dims(uint32_t& w, uint32_t& h);
/// The loop's "a new frame came" (clears itself).
bool take_frame();
/// Draw thread: the newest frame (see `FrameStore::acquire`); false when no NEW one.
bool acquire(FrameStore::Frame& f);
/// A non-looping request reached its end / the file could not be played (each clears itself).
bool take_ended();
bool take_failed();
/// libvlc is missing (known after the first `play`).
bool unavailable();
/// Called (from any thread) when a frame, an end or a failure needs the loop. `sdlr.cpp` gives it the loop's wake-up.
void set_wake(std::function<void()> wake);
/// Called on the MAIN thread with true when an unmuted video starts and false when it stops: the music player lowers / restores itself.
/// Without a hook the weak symbol `zf::music::duck(bool)` is called when the music module is linked; the hook replaces it (tests).
void set_duck_hook(std::function<void(bool)> hook);

/// The placeholder image the scene draws a video frame with (made once per `Assets`, pinned) and the test for it.
ImgId frame_id(Assets& a);
bool is_frame(const Assets& a, ImgId id);

// ---- where the preview goes ---------------------------------------------------------------------------------------------
// The list views of the themes offer the box of the game's picture while they draw (`box_offer`); the biggest offer wins (XMB offers
// its row icon before the cover). `ListScreen` resets before and reads after drawing.
struct Box {
  float x = 0, y = 0, w = 0, h = 0;
  bool valid() const { return w >= 8 && h >= 8; }
};
void box_reset();
void box_offer(float x, float y, float w, float h);
Box box_get();

// ---- tests ------------------------------------------------------------------------------------------------------------------
/// Replaces the decoder (null: libvlc again) and the thread: `threaded=false` runs every command inline in `play`/`stop` and the test calls `poll_for_test`.
void set_engine_for_test(std::unique_ptr<Engine> e, bool threaded);
/// What the control thread does every 250 ms while a video plays: loop / end / error.
void poll_for_test();
/// The store (so the fake engine of a test can write into it).
FrameStore& store_for_test();
/// Forgets everything (after `shutdown`).
void reset_for_test();

}  // namespace video
}  // namespace zf
