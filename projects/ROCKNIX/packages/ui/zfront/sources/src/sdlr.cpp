#include "power.hpp"
#include "sdlr.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>
#if defined(__GLIBC__)
#include <malloc.h>  // malloc_trim
#endif

#include "autoperf.hpp"
#include "covers.hpp"
#include "detailed.hpp"
#include "homemenu.hpp"
#include "idlewait.hpp"
#include "launch.hpp"
#include "music.hpp"
#include "bootargs.hpp"
#include "autocover.hpp"
#include "oscontract.hpp"
#include "resume_last.hpp"
#include "snapshot.hpp"
#include "sdlwl.hpp"
#include "sound.hpp"
#include "video.hpp"
#include "wakegate.hpp"

namespace zf {

namespace {

std::optional<Action> key_action(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP: return Action::Up;
    case SDLK_DOWN: return Action::Down;
    case SDLK_LEFT: return Action::Left;
    case SDLK_RIGHT: return Action::Right;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return Action::A;
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE: return Action::B;
    case SDLK_x: return Action::X;
    case SDLK_y: return Action::Y;
    case SDLK_s: return Action::Start;
    case SDLK_TAB: return Action::Select;
    case SDLK_PAGEUP: return Action::L;
    case SDLK_PAGEDOWN: return Action::R;
    default: return std::nullopt;
  }
}

// Both edges rounded (gfx.hpp `px_rect`, like cpu.cpp): rounding x and w apart left 1 px gaps / overlaps between neighbours.
SDL_Rect to_rect(const Op& op) {
  const PxRect p = px_rect(op);
  return SDL_Rect{p.x, p.y, p.w, p.h};
}

/// Resident memory of this process in kB (/proc/self/status VmRSS); -1 when it cannot be read.
long rss_kb() {
  std::FILE* f = std::fopen("/proc/self/status", "r");
  if (!f) return -1;
  char line[256];
  long kb = -1;
  while (std::fgets(line, sizeof line, f))
    if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
  std::fclose(f);
  return kb;
}

// SDL_WaitEvent/WaitEventTimeout poll every 1 ms while a joystick is open (SDL_events_need_polling): ~1000 wake-ups and
// ~3.3 % of a core while idle. We sleep in `IdleWaiter` (idlewait.hpp: poll(2) on the pad evdev nodes, a wake-up
// eventfd and, on Wayland, the display socket).

/// The evdev nodes of the joysticks SDL currently knows.
std::vector<std::string> joystick_paths() {
  std::vector<std::string> paths;
  int n = SDL_NumJoysticks();
  for (int i = 0; i < n; ++i)
    if (const char* p = SDL_JoystickPathForIndex(i)) paths.emplace_back(p);
  return paths;
}

/// Re-open the evdev nodes of the joysticks SDL currently knows.
void rebuild_pads(IdleWaiter& iw) { iw.set_pad_paths(joystick_paths()); }

// timeout_ms < 0 waits for ever. Returns true with an event.
bool wait_event(IdleWaiter& iw, SDL_Event* e, int timeout_ms) {
  TimePoint end = timeout_ms < 0 ? TimePoint::max() : Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    SDL_PumpEvents();
    if (SDL_PollEvent(e)) {
      iw.note_event();
      return true;
    }
    int left = 1 << 20;
    if (timeout_ms >= 0) {
      TimePoint now = Clock::now();
      if (now >= end) return false;
      left = int(std::chrono::duration_cast<std::chrono::microseconds>(end - now).count() / 1000) + 1;
    }
    if (iw.sleep(left)) return false;  // the status bar's uevents: the loop polls the status now (nothing else would end this wait)
    if (oscontract::resume_pending()) return false;  // the machine was suspended meanwhile: let the loop end the screensaver
  }
}

// Milliseconds until `t` (at least 1, at most `cap`).
int ms_until(TimePoint t, TimePoint now, int cap) {
  if (t <= now) return 1;
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(t - now).count();
  return int(std::min<long long>(us / 1000 + 1, cap));
}


// Navigation sounds (sound.hpp): one SDL audio device, opened on demand on a worker thread (opening a PipeWire stream can take
// 100+ ms; the cue that asked for it is skipped, as ES does) and closed after a few idle seconds, so that no audio stream stays
// alive while nothing is heard. The wavs of the Zenith theme are 48 kHz mono 16 bit: the device is asked for exactly that and
// SDL converts for the real output.
//
// Clean output: the device runs a callback that MIXES the cues that are playing (never cuts one short: a truncated waveform is
// an audible click), every clip gets a 2 ms fade at both ends (the wavs start and end off zero), the sum is clamped, and a cue
// that repeats (the scroll tick) is spaced at least 45 ms from the previous one, so holding a direction gives a steady tick
// instead of a smear.
class AudioOut {
 public:
  ~AudioOut() { shutdown(); }

  void play(const std::string& wav, bool repeating) {
    const int st = state_.load();
    if (st == kClosed) {
      if (Clock::now() >= retry_at_) start_open();
      return;
    }
    if (st != kOpen) return;
    Clip* c = clip(wav);
    if (!c) return;
    const TimePoint now = Clock::now();
    if (repeating && now - c->last < kMinGap) return;
    c->last = now;
    {
      std::lock_guard<std::mutex> g(mu_);
      if (voices_.size() >= kMaxVoices) voices_.erase(voices_.begin());  // only past 4 cues at once
      voices_.push_back({&c->pcm, 0});
    }
    idle_at_ = now + kIdle;
  }

  // when the idle close is due (none while the device is closed)
  std::optional<TimePoint> deadline() const {
    if (state_.load() != kOpen) return std::nullopt;
    return idle_at_;
  }
  void tick(TimePoint now) {
    if (state_.load() == kOpen && now >= idle_at_ && !playing()) close_now();
  }

  // The game is about to start and SDL video goes away. True when the device must outlive it: the launch sound is still
  // playing and is not cut; `close_later` closes the device once it is heard (at most `max_ms`).
  bool outlives_video() {
    join_opener();
    return state_.load() == kOpen;
  }
  void close_later(int max_ms) {
    join_closer();
    closer_ = std::thread([this, max_ms] {
      const TimePoint end = Clock::now() + std::chrono::milliseconds(max_ms);
      while (playing() && Clock::now() < end) SDL_Delay(15);
      drop_device();
    });
  }
  void join_closer() {
    if (closer_.joinable()) closer_.join();
  }
  void shutdown() {
    join_closer();
    join_opener();
    if (state_.load() == kOpen) drop_device();
  }

 private:
  enum { kClosed, kOpening, kOpen };
  static constexpr std::chrono::seconds kIdle{4};
  static constexpr std::chrono::milliseconds kMinGap{45};
  static constexpr size_t kMaxVoices = 4;
  static constexpr size_t kFade = 96;  // samples: 2 ms at 48 kHz

  struct Clip {
    std::vector<int16_t> pcm;
    TimePoint last{};
  };
  struct Voice {
    const std::vector<int16_t>* pcm;
    size_t pos;
  };

  bool playing() {
    std::lock_guard<std::mutex> g(mu_);
    return !voices_.empty();
  }

  // SDL audio thread: the sum of the cues that are playing (silence when there are none)
  static void callback(void* self, Uint8* stream, int len) { static_cast<AudioOut*>(self)->mix(reinterpret_cast<int16_t*>(stream), size_t(len) / 2); }
  void mix(int16_t* out, size_t n) {
    std::lock_guard<std::mutex> g(mu_);
    std::fill(out, out + n, int16_t(0));
    for (Voice& v : voices_) {
      const size_t take = std::min(n, v.pcm->size() - v.pos);
      for (size_t i = 0; i < take; ++i) {
        const int sum = int(out[i]) + int((*v.pcm)[v.pos + i]);
        out[i] = int16_t(std::max(-32768, std::min(32767, sum)));
      }
      v.pos += take;
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.pos >= v.pcm->size(); }), voices_.end());
  }

  void join_opener() {
    if (opener_.joinable()) opener_.join();
  }
  void start_open() {
    join_opener();
    state_.store(kOpening);
    opener_ = std::thread([this] {
      if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "zfront: sound: SDL audio: %s\n", SDL_GetError());
        retry_at_ = Clock::now() + std::chrono::seconds(30);
        state_.store(kClosed);
        return;
      }
      SDL_AudioSpec want;
      SDL_zero(want);
      want.freq = 48000;
      want.format = AUDIO_S16SYS;
      want.channels = 1;
      want.samples = 1024;  // 21 ms: room for a busy frame without an underrun (an underrun is a crackle)
      want.callback = &AudioOut::callback;
      want.userdata = this;
      SDL_AudioSpec have;
      dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
      if (!dev_) {
        std::fprintf(stderr, "zfront: sound: no audio device: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        retry_at_ = Clock::now() + std::chrono::seconds(30);
        state_.store(kClosed);
        return;
      }
      if (!logged_) std::fprintf(stderr, "zfront: sound: audio driver %s\n", SDL_GetCurrentAudioDriver());
      logged_ = true;
      SDL_PauseAudioDevice(dev_, 0);
      idle_at_ = Clock::now() + kIdle;
      state_.store(kOpen);
    });
  }
  void drop_device() {
    if (dev_) SDL_CloseAudioDevice(dev_);
    dev_ = 0;
    {
      std::lock_guard<std::mutex> g(mu_);
      voices_.clear();
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    state_.store(kClosed);
  }
  void close_now() {
    join_opener();
    drop_device();
  }
  Clip* clip(const std::string& path) {
    auto it = clips_.find(path);
    if (it != clips_.end()) return it->second.pcm.empty() ? nullptr : &it->second;
    Clip c;
    SDL_AudioSpec spec;
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (SDL_LoadWAV(path.c_str(), &spec, &buf, &len)) {
      if (spec.freq == 48000 && spec.format == AUDIO_S16LSB && spec.channels == 1) {
        c.pcm.resize(len / 2);
        std::memcpy(c.pcm.data(), buf, c.pcm.size() * 2);
        const size_t f = std::min(kFade, c.pcm.size() / 4);  // 2 ms ramps: the clip starts and ends at zero
        for (size_t i = 0; i < f; ++i) {
          const float g = float(i) / float(f);
          c.pcm[i] = int16_t(float(c.pcm[i]) * g);
          c.pcm[c.pcm.size() - 1 - i] = int16_t(float(c.pcm[c.pcm.size() - 1 - i]) * g);
        }
      } else {
        std::fprintf(stderr, "zfront: sound: %s is not 48 kHz mono 16 bit: skipped\n", path.c_str());
      }
      SDL_FreeWAV(buf);
    } else {
      std::fprintf(stderr, "zfront: sound: %s: %s\n", path.c_str(), SDL_GetError());
    }
    Clip& slot = clips_[path];
    slot = std::move(c);
    return slot.pcm.empty() ? nullptr : &slot;
  }

  std::atomic<int> state_{kClosed};
  SDL_AudioDeviceID dev_ = 0;
  TimePoint idle_at_{}, retry_at_{};
  std::map<std::string, Clip> clips_;
  std::mutex mu_;
  std::vector<Voice> voices_;
  std::thread opener_, closer_;
  bool logged_ = false;
};

}  // namespace

bool sdl_output_size(uint32_t& w, uint32_t& h) {
  if (!oscontract::display_ready()) return false;
  oscontract::before_video();
  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return false;
  SDL_DisplayMode dm;
  const bool ok = SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0;
  if (ok) {
    w = uint32_t(dm.w);
    h = uint32_t(dm.h);
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);  // released here: closing the video before a game counts on `open_video` holding the only reference
  return ok;
}

namespace {

// The window of `sdl_splash`, waiting for the first `open_video` of `sdl_run`.
SDL_Window* g_pre_win = nullptr;
SDL_Renderer* g_pre_rd = nullptr;
snapshot::Key g_snap_key;
bool g_snap_on = false;

// SDL video + joystick start, window and renderer (what `open_video` does at start and after every game), shared with `sdl_splash`.
bool create_window(uint32_t w, uint32_t h, bool fullscreen, SDL_Window*& win, SDL_Renderer*& rd, std::string& e) {
  oscontract::before_video();  // Wayland app_id must be "emulationstation" (see oscontract.hpp)
  // GAMECONTROLLER only for its mapping database (reserve mapping of pads without an es_input.cfg entry); the pads are
  // never opened as controllers, so ignore its events
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
    e = std::string("SDL_Init: ") + SDL_GetError();
    return false;
  }
  SDL_GameControllerEventState(SDL_IGNORE);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  // Fullscreen: the window is created at the output's mode. Under Sway the fullscreen configure arrives after
  // SDL_CreateWindow, so the renderer still reported the requested size (720x720 without --size) and a 640x480 screen
  // got the 1:1 layout, cut off.
  int ww = int(w), wh = int(h);
  SDL_DisplayMode dm;
  if (fullscreen && SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    ww = dm.w;
    wh = dm.h;
  }
  win = SDL_CreateWindow("zfront", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, ww, wh, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  if (!win) {
    e = std::string("SDL_CreateWindow: ") + SDL_GetError();
    SDL_Quit();
    return false;
  }
  rd = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!rd) {
    e = std::string("SDL_CreateRenderer: ") + SDL_GetError();
    SDL_DestroyWindow(win);
    win = nullptr;
    SDL_Quit();
    return false;
  }
  SDL_SetRenderDrawBlendMode(rd, SDL_BLENDMODE_BLEND);
  return true;
}

}  // namespace

bool sdl_splash(const snapshot::Image& im, uint32_t w, uint32_t h, bool fullscreen) {
  if (g_pre_win || im.w == 0 || im.h == 0 || im.px.size() != size_t(im.w) * im.h) return false;
  if (!oscontract::display_ready()) return false;  // never waits: the normal start does
  SDL_Window* win = nullptr;
  SDL_Renderer* rd = nullptr;
  std::string e;
  if (!create_window(w, h, fullscreen, win, rd, e)) {
    std::fprintf(stderr, "zfront: snapshot: %s\n", e.c_str());
    return false;
  }
  g_pre_win = win;  // from here `sdl_run` owns it, shown or not
  g_pre_rd = rd;
  SDL_Texture* tex = SDL_CreateTexture(rd, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC, int(im.w), int(im.h));
  if (!tex || SDL_UpdateTexture(tex, nullptr, im.px.data(), int(im.w) * 2) != 0) {
    if (tex) SDL_DestroyTexture(tex);
    return false;
  }
  // two presents with the events pumped between: under Sway the fullscreen configure arrives after the window exists
  for (int i = 0; i < 2; ++i) {
    SDL_PumpEvents();
    SDL_SetRenderDrawColor(rd, 0, 0, 0, 255);
    SDL_RenderClear(rd);
    SDL_RenderCopy(rd, tex, nullptr, nullptr);
    SDL_RenderPresent(rd);
  }
  SDL_DestroyTexture(tex);
  std::fprintf(stderr, "zfront: snapshot shown at uptime %s\n", uptime_text().c_str());
  return true;
}

void sdl_set_snapshot_key(const snapshot::Key& key) {
  g_snap_key = key;
  g_snap_on = true;
}

bool sdl_run(App& app, Pad* pad, uint32_t w, uint32_t h, bool fullscreen, std::string& err, std::function<std::optional<Outcome>(App&)> startup) {
  SDL_Window* win = nullptr;
  SDL_Renderer* rd = nullptr;
  std::vector<SDL_Texture*> textures;
  std::unordered_set<ImgId> tex_failed;  // images whose texture the GPU refused (too big): not retried every frame
  std::vector<ImgId> evicted_ids;
  app.cx.assets.track_evictions(true);  // the renderer destroys the textures of what Assets evicts, without walking every id per frame
  SDL_Texture* vtex = nullptr;  // the streaming texture of the playing video (video.hpp)
  uint32_t vtw = 0, vth = 0;
  bool first_open = true;
  IdleWaiter iw;
  iw.set_watch_fd(app.status_wake_fd());  // a charger plugged or pulled wakes the loop: the bar changes at once
  AudioOut audio;
  const std::shared_ptr<WakeGate> wake_gate = std::make_shared<WakeGate>();  // open only while SDL is up (workers may wake the loop during a game)
  // Window + renderer + joystick subsystem. Called at start and again after every game (the window must NOT exist
  // while the emulator runs: it needs the display).
  auto open_video = [&](std::string& e) -> bool {
    audio.join_closer();  // the launch sound of the last game must be gone before SDL starts again
    if (first_open && g_pre_win) {  // the window of the instant start (`sdl_splash`): same SDL state, already showing the last frame
      win = g_pre_win;
      rd = g_pre_rd;
      g_pre_win = nullptr;
      g_pre_rd = nullptr;
    } else if (!create_window(w, h, fullscreen, win, rd, e)) {
      return false;
    }
    int cw = 0, ch = 0;
    SDL_GetRendererOutputSize(rd, &cw, &ch);
    app.cx.w = float(cw);
    app.cx.h = float(ch);
    if (first_open && std::string(ratio_name(w, h)) != ratio_name(unsigned(cw), unsigned(ch))) {
      // `sdl_output_size` could not tell (or the output changed since): the theme was loaded for another ratio
      Theme t;
      std::string terr;
      if (Theme::load(app.cx.th.dir(), ratio_name(unsigned(cw), unsigned(ch)), t, terr)) {
        app.cx.th = std::move(t);
        boot::apply_user_theme(app.cx.th, [&](const std::string& k) { return app.cx.conf.get_or(k, ""); });  // as at start
        if (!load_common(app.cx.th, app.cx.assets, app.cx.common, terr)) std::fprintf(stderr, "zfront: theme reload: %s\n", terr.c_str());
        std::fprintf(stderr, "zfront: output %dx%d: layout %s (was %s)\n", cw, ch, ratio_name(unsigned(cw), unsigned(ch)), ratio_name(w, h));
      } else {
        std::fprintf(stderr, "zfront: theme reload for %dx%d failed: %s\n", cw, ch, terr.c_str());
      }
    }
    if (pad) pad->attach();
    rebuild_pads(iw);
    iw.set_display_fd(wayland_display_fd(win));  // after every window: SDL_Quit closes the old connection
    if (first_open) {
      SDL_RendererInfo info;
      SDL_GetRendererInfo(rd, &info);
      std::fprintf(stderr, "zfront: video driver %s, renderer %s, output %dx%d, wayland display fd: %s\n", SDL_GetCurrentVideoDriver(), info.name, cw, ch,
                   iw.display_fd() >= 0 ? "yes (idle sleep up to 1 s)" : "no (idle sleep capped at 100 ms)");
    }
    first_open = false;
    wake_gate->set_up(true);
    return true;
  };
  auto close_video = [&]() {
    wake_gate->set_up(false);  // waits for a push in flight; no worker touches SDL's event queue after this
    iw.set_display_fd(-1);  // the socket is SDL's: forget it before SDL_Quit closes it
    iw.close_pads();
    for (SDL_Texture*& t : textures) {
      if (t) SDL_DestroyTexture(t);
      t = nullptr;
    }
    textures.clear();
    tex_failed.clear();  // a new renderer may take what the old one refused
    if (vtex) SDL_DestroyTexture(vtex);
    vtex = nullptr;
    app.cx.assets.forget_released();  // images that only lived in those textures are rebuilt by key on the next frame
    if (rd) SDL_DestroyRenderer(rd);
    if (win) SDL_DestroyWindow(win);
    rd = nullptr;
    win = nullptr;
    if (audio.outlives_video()) {  // a cue is still playing (the launch sound): only video and joysticks go
      SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
      audio.close_later(500);
    } else {
      SDL_Quit();
    }
  };
  if (!open_video(err)) return false;
  sound::set_player([&audio](sound::Cue cue, const std::string& wav) { audio.play(wav, cue == sound::Cue::Scroll); });
  // background counts wake the loop with a user event (SDL_PushEvent is thread-safe)
  Uint32 wake_type = SDL_RegisterEvents(1);
  // The event goes first and the eventfd second: `poll` returns on the eventfd and the loop must then find the event.
  // (The eventfd is shared: a worker calling this while we shut down never touches a closed descriptor.)
  const std::function<void()> wake_loop = [wake_type, wake_gate, wake_iw = iw.wake_function()] {
    wake_gate->notify([wake_type] {  // not while SDL is shut down (a game is running)
      SDL_Event ev;
      SDL_zero(ev);
      ev.type = wake_type;
      SDL_PushEvent(&ev);
    });
    wake_iw();
  };
  app.set_wake(wake_loop);
  video::set_wake(wake_loop);  // a video frame / end wakes the loop the same way
  bool ok = true;
  // ES `ShowControllerNotifications`: a toast when a pad is plugged in or removed while we run
  if (pad)
    pad->set_plug_sink([&app](bool added, const std::string& name) {
      if (!app.cx.es.get_bool("ShowControllerNotifications", true)) return;
      std::string text = (added ? tr("%s connected") : tr("%s disconnected"));
      size_t p = text.find("%s");
      if (p != std::string::npos) text.replace(p, 2, name);
      app.cx.sv->toasts.push(text);
    });
  launch::LaunchPrep prep;
  int keys_down = 0;  // keyboard keys physically held (SDL makes their repeats inside SDL_PumpEvents)
  bool have_pending = false;
  launch::Outcome pending;
  std::string pending_sys_name, pending_path;  // the launched game by name/path: indexes can shift if the lists reload during the game

  std::vector<Action> acts;
  acts.reserve(8);
  bool dirty = true, first_logged = false, quit = false;
  // instant start: the picture of the next start, taken once the carousel has rested a few seconds (snapshot.hpp)
  const char* snap_env = std::getenv("ZF_SNAPSHOT");
  const bool snap_on = g_snap_on && !(snap_env && std::strcmp(snap_env, "0") == 0);
  snapshot::Gate snap_gate;
  bool snap_capture = false;
  const TimePoint snap_t0 = Clock::now();
  auto snap_secs = [&snap_t0](TimePoint t) { return std::chrono::duration<double>(t - snap_t0).count(); };
  TimePoint last = Clock::now();
  // ZF_FRAMELOG=<file>: one line per presented frame (diagnostics for animation smoothness, off by default)
  std::FILE* flog = nullptr;
  if (const char* fp = std::getenv("ZF_FRAMELOG"); fp && *fp) flog = std::fopen(fp, "w");
  TimePoint flog_prev = Clock::now();
  // The main menu and the screens it opens are built and their textures made once, in idle moments shortly after start (see
  // `App::warm`): the first opening of each used to cost 50-100 ms (text rendering + texture upload). Two steps per screen
  // (images, then textures), `warm_step` counts them; done when it passes the list.
  static const char* const kWarm[] = {"menu.main", "menu.g.screen", "menu.g.power", "menu.game", "menu.g.library", "menu.g.system"};
  constexpr int kWarmSteps = int(sizeof(kWarm) / sizeof(kWarm[0])) * 2;
  int warm_step = app.cx.ui_motion && !std::getenv("ZF_NOWARM") ? 0 : kWarmSteps;  // ZF_NOWARM=1: off (to measure the cold opening)
  TimePoint warm_at = Clock::now() + std::chrono::milliseconds(2500);
  std::vector<Op> warm_ops;
  const std::vector<Op>* last_ops = nullptr;  // the ops of the last frame built; a video frame alone redraws them
  bool last_video = false;
  // The texture of image `id`, made from its pixels the first time it is needed (null: evicted or not available).
  auto texture_for = [&](ImgId id) -> SDL_Texture* {
    Assets& as = app.cx.assets;
    if (textures.size() <= id) textures.resize(std::max<size_t>(as.count(), id + 1), nullptr);
    if (as.evicted(id)) {
      if (textures[id]) SDL_DestroyTexture(textures[id]);
      textures[id] = nullptr;
      return nullptr;
    }
    if (!textures[id]) {
      if (!as.has_pixels(id)) return nullptr;  // cannot happen: textures are only dropped together with `forget_released`
      if (tex_failed.count(id)) return nullptr;
      const Img& im = as.img(id);
      SDL_Texture* t = SDL_CreateTexture(rd, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, int(im.w), int(im.h));
      if (!t) {  // e.g. larger than the GPU's maximum texture size: say so once and do not try again every frame
        std::fprintf(stderr, "zfront: no texture for image %u (%ux%u): %s\n", unsigned(id), unsigned(im.w), unsigned(im.h), SDL_GetError());
        tex_failed.insert(id);
        return nullptr;
      }
      SDL_UpdateTexture(t, nullptr, im.rgba.data(), int(im.w * 4));
      SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
      textures[id] = t;
      as.release_pixels(id);  // the texture holds them now: do not keep a second copy in RAM (pinned images keep theirs)
    }
    return textures[id];
  };

  autocover::Controller autocov(app.cx);  // background covers while the frontend is left alone (off unless ZfAutoCovers)
  while (!quit) {
    if (pad) pad->set_profile(app.repeat_profile());
    // sleep until an event or the next deadline (status bar, key repeat); while animating, one frame at a time
    SDL_Event e;
    bool got = false;
    if (dirty) {
      got = SDL_PollEvent(&e) != 0;  // something to draw already: do not sleep
    } else if (app.animating() && !oscontract::idle().active()) {
      got = SDL_WaitEventTimeout(&e, 1) != 0;
    } else {
      TimePoint now = Clock::now();
      std::optional<TimePoint> dl;
      if (oscontract::idle().active()) {
        // screensaver on: the UI underneath is frozen; the only wake-up is the clock of a dimmed screen (none when black)
        dl = oscontract::saver_deadline(app.cx.es, now);
        if (std::optional<TimePoint> cd = autocov.deadline(); cd && (!dl || *cd < *dl)) dl = cd;
      } else {
        dl = app.status_deadline();
        if (std::optional<TimePoint> sd = app.screen_deadline(); sd && (!dl || *sd < *dl)) dl = sd;
        if (pad) {
          std::optional<TimePoint> pd = pad->next_deadline();
          if (pd && (!dl || *pd < *dl)) dl = pd;
        }
        std::optional<TimePoint> qd = prep.next_deadline();  // cursor rest time of the idle-time preparation
        if (qd && (!dl || *qd < *dl)) dl = qd;
        if (warm_step < kWarmSteps && (!dl || warm_at < *dl)) dl = warm_at;
        std::optional<TimePoint> sd2 = oscontract::idle_deadline(app.cx.es);  // screensaver timer
        if (sd2 && (!dl || *sd2 < *dl)) dl = sd2;
        if (snap_on) {  // the carousel has rested long enough: draw once more to take the picture
          if (std::optional<double> sd = snap_gate.deadline()) {
            const TimePoint d = snap_t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(*sd));
            if (!dl || d < *dl) dl = d;
          }
        }
        std::optional<TimePoint> ad = audio.deadline();  // closes the audio device after a few silent seconds
        if (ad && (!dl || *ad < *dl)) dl = ad;
        if (std::optional<TimePoint> cd = autocov.deadline(); cd && (!dl || *cd < *dl)) dl = cd;
      }
      iw.set_keys_held(keys_down > 0);
      got = wait_event(iw, &e, dl ? ms_until(*dl, now, 60000) : -1);
    }
    bool raw_input = false;  // a button went down (mapped or not): wakes / postpones the screensaver
    while (got) {
      switch (e.type) {
        case SDL_QUIT:
          quit = true;
          break;
        case SDL_WINDOWEVENT:
          if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) keys_down = 0;
          dirty = true;
          break;
        case SDL_RENDER_TARGETS_RESET:
        case SDL_RENDER_DEVICE_RESET:  // the GPU lost its textures: rebuild from the pixels we still have (see forget_released)
          for (SDL_Texture*& t : textures) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
          }
          if (vtex) SDL_DestroyTexture(vtex);
          vtex = nullptr;
          tex_failed.clear();
          app.cx.assets.forget_released();
          dirty = true;
          break;
        case SDL_KEYUP:
          if (keys_down > 0) --keys_down;
          break;
        case SDL_KEYDOWN: {
          raw_input = true;
          if (!e.key.repeat) ++keys_down;
          auto a = key_action(e.key.keysym.sym);
          if (!a || (e.key.repeat && !action_repeats(*a))) break;
          acts.push_back(*a);
          break;
        }
        case SDL_JOYDEVICEADDED:
        case SDL_JOYDEVICEREMOVED:
          rebuild_pads(iw);
          if (pad) pad->on_event(&e, Clock::now(), acts);
          break;
        default:
          if (e.type == wake_type) break;  // handled by drain_counts below
          if (e.type == SDL_JOYBUTTONDOWN || e.type == SDL_JOYHATMOTION) raw_input = true;
          if (pad) pad->on_event(&e, Clock::now(), acts);
          break;
      }
      got = SDL_PollEvent(&e) != 0;
    }
    if (pad) pad->repeat_due(Clock::now(), acts);
    if (pad && pad->take_unconfigured() && app.open_controller_detect()) dirty = true;  // pad without mapping: suggest configuring it (ES)
    audio.tick(Clock::now());
    if (raw_input || !acts.empty()) autocov.input(Clock::now());  // any input stops the background covers at once
    oscontract::before_acts(app.cx.es, Clock::now(), acts, raw_input);  // screensaver start; the key that wakes it is dropped
    if (oscontract::take_redraw()) dirty = true;  // overlay on / off
    // jobs first: POST /launch arrives as a job `done` (request_launch); without this it would wait for the next event
    if (app.drain_jobs(Clock::now())) dirty = true;
    std::optional<Outcome> ext;
    if (startup) {  // the boot game (ES launchStartupGame): the first request of the loop, once
      ext = startup(app);
      startup = nullptr;
      dirty = true;  // it may have pushed the countdown of "resume last game"
    }
    if (!ext) ext = oscontract::take_external(app);  // POST /launch
    if (!ext) ext = app.take_pending_launch();       // a screen's own countdown ended (resume last game)
    const bool had_acts = !acts.empty();
    for (size_t ai = 0; ai < acts.size() + (ext ? 1 : 0); ++ai) {
      Outcome o = ai < acts.size() ? app.handle(acts[ai]) : std::move(*ext);
      if (o.kind == Outcome::Quit) {
        quit = true;
        break;
      }
      if (o.kind == Outcome::Launch) {
        launch::Request rq;
        rq.cmd = o.cmd;
        bool known = o.sys >= 0 && o.game >= 0 && size_t(o.sys) < app.cx.systems.size() && size_t(o.game) < app.cx.systems[size_t(o.sys)].games.size();
        if (known) {
          const Game& g = app.cx.systems[size_t(o.sys)].games[size_t(o.game)];
          rq.rom_raw = g.path;
          rq.name = g.name;
        }
        std::string launched_sys, launched_path;
        if (known) {
          launched_sys = app.cx.systems[size_t(o.sys)].name;
          launched_path = app.cx.systems[size_t(o.sys)].games[size_t(o.game)].path;
        }
        launch::Env env;
        env.prep = &prep;
        // In-game HOME menu (experimental, off by default): the chord watcher runs inside `launch::run`, on the evdev nodes SDL knows
        // NOW (they cannot be asked once SDL is gone, so `release` stores them). With the setting off `supervise` stays empty and
        // `run` is the plain blocking wait.
        std::vector<std::string> game_pads;
        std::unique_ptr<homemenu::PadWatcher> home;
        if (homemenu::enabled(app.cx.conf) && homemenu::panel_args_set()) {
          env.supervise = [&](int) {
            if (!home) {
              home = std::make_unique<homemenu::PadWatcher>(homemenu::ComboDetector(homemenu::combo_codes(homemenu::combo(app.cx.conf))));
              home->open(game_pads);
            }
            if (!home->wait(100)) return;
            std::fprintf(stderr, "zfront: HOME menu chord held: pausing the game\n");
            homemenu::install_safety();
            const homemenu::Outcome out = homemenu::trigger(homemenu::real_io(*home));
            homemenu::remove_safety();
            std::fprintf(stderr, "zfront: HOME menu closed (outcome %d)\n", int(out));
          };
        }
        power::game_started(rq.name.empty() ? std::string("Game") : rq.name);  // what the game uses is measured from here to its end
        env.release = [&] {
          const long rss0 = rss_kb();
          if (env.supervise) game_pads = joystick_paths();  // before the joysticks and SDL go
          // our evdev descriptors and the display socket, then the joysticks, window/renderer/SDL (no audio device is ever opened)
          music::pause_for_game();  // the mixer releases the sound card (ES: AudioManager::deinit) before SDL goes away
          iw.close_pads();
          iw.set_display_fd(-1);
          if (pad) pad->detach();
          video::shutdown();  // libvlc's decoder and audio must be gone before the emulator starts (releases the engine)
          close_video();
          // The emulator needs the RAM more than we need our caches: give back every image that is not pinned (CPU copies too),
          // the cached gamelist details, the failed-cover set and the texture bookkeeping. All of it is rebuilt lazily, only what is
          // visible, after the game.
          const size_t dropped = app.cx.assets.drop_unpinned();
          detailed_cache_clear();
          covers::clear_failed();
          covers::cancel_pending();  // queued cover decodes would hold their pictures during the game
          app.cx.assets.take_evicted(evicted_ids);  // their textures are already gone
          std::vector<ImgId>().swap(evicted_ids);
          std::vector<SDL_Texture*>().swap(textures);
#if defined(__GLIBC__)
          malloc_trim(0);  // hand the freed heap back to the kernel (free() keeps most of it inside the process)
#endif
          std::fprintf(stderr, "zfront: release for game: VmRSS %ld kB -> %ld kB (images dropped %zu kB, pinned kept %zu kB)\n", rss0, rss_kb(), dropped / 1024,
                       app.cx.assets.bytes() / 1024);
        };
        env.restore = [&] {
          power::game_ended();
          if (!open_video(err)) {
            ok = false;
            quit = true;
            return;
          }
          SDL_PumpEvents();
          SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);  // discard what was typed while the game ran
          music::resume_after_game();
          if (pad) pad->clear();
          keys_down = 0;
          rebuild_pads(iw);  // the joysticks opened by the new window (open_video also acquired the display socket)
        };
        autocov.game_started();
        oscontract::game_started(rq.rom_raw.empty() ? rq.cmd : rq.rom_raw, rq.name);
        if (known) autoperf::begin_session(app.cx, launched_sys, launched_path);  // PERFORMANCE SUGGESTIONS: arms the sampler of launch::run
        else autoperf::disarm();
        pending = launch::execute(rq, env);
        {
          const std::string perf_toast = known ? autoperf::end_session(app.cx, launched_sys, launched_path, pending.run.secs, pending.run.exit_code) : std::string();
          if (!perf_toast.empty() && !launch::term_requested()) app.cx.sv->toasts.push(perf_toast, 6000);
        }
        resume::note_session(resume::default_paths().session, pending.run.secs, rq.rom_raw);  // a game that ran under 10 s is not resumed next boot
        oscontract::game_ended();
        if (!pending.message.empty()) {
          std::fprintf(stderr, "zfront: AN ERROR OCCURRED: %s\n", pending.message.c_str());
          app.cx.sv->toasts.push(launch::error_toast(pending), 7000);
        }
        have_pending = known;
        pending_sys_name = launched_sys;
        pending_path = launched_path;
        if (launch::term_requested()) {  // SIGTERM during the game: the window was not rebuilt, bookkeeping now and out
          if (have_pending) launch::apply_after_path(app.cx, pending_sys_name, pending_path, pending);
          have_pending = false;
          quit = true;
        }
        if (quit) break;
        // The rest of the batch (the second A of "A, A", a queued POST /launch) was typed before the game ran: it must not start
        // another one nor act on the screen we come back to.
        acts.clear();
        ext.reset();
        dirty = true;
        last = Clock::now();
        break;
      }
      dirty = true;
      last = Clock::now();
    }
    if (had_acts) prep.set_target(app.launch_target(), Clock::now());  // arms the dwell timer of the preparation
    if (had_acts || raw_input) oscontract::input(Clock::now());  // after the actions: a game may have run for hours inside
    acts.clear();
    if (quit) break;
    if (app.drain_counts()) dirty = true;
    if (app.drain_jobs(Clock::now())) dirty = true;
    prep.poll(Clock::now());
    const bool vframe = video::take_frame();  // a decoded video frame is waiting
    TimePoint now = Clock::now();
    // screensaver on: the screens underneath are frozen (no animation frames, no timers); `last` follows so that the first
    // tick after waking does not see the whole sleep as one huge dt
    const float tick_dt = std::chrono::duration<float>(now - last).count();
    if (!oscontract::idle().active() && app.tick(tick_dt)) dirty = true;
    last = now;
    if (app.poll_status(now)) dirty = true;
    if (autocov.poll(now)) dirty = true;  // a finished run pushes its toast
    if (snap_on && snap_gate.poll(first_logged && !oscontract::idle().active() && app.snapshot_eligible(), snap_secs(now))) {
      snap_capture = true;
      dirty = true;  // the next frame is read back before it is presented
    }
    if (warm_step < kWarmSteps) {  // one step per idle moment, never while the person is pressing or something moves
      if (raw_input || keys_down > 0 || app.animating()) {
        warm_at = std::max(warm_at, now + std::chrono::milliseconds(600));
      } else if (!dirty && now >= warm_at) {
        const TimePoint w0 = Clock::now();
        if (warm_step % 2 == 0) {
          warm_ops = app.warm(kWarm[warm_step / 2]);
        } else {
          for (const Op& op : warm_ops)
            if (op.kind == Op::Image) texture_for(op.id);
          warm_ops.clear();
          warm_ops.shrink_to_fit();
        }
        if (flog) {  // how long the loop was blocked by this step (ms) and the images held afterwards (MB)
          std::fprintf(flog, "#WARM%d %.3f %.2f\n", warm_step, std::chrono::duration<double, std::milli>(Clock::now() - w0).count(),
                       double(app.cx.assets.bytes()) / 1048576.0);
          std::fflush(flog);
        }
        ++warm_step;
        warm_at = Clock::now() + std::chrono::milliseconds(150);
      }
    }
    if (!dirty && !vframe) continue;
    // a video frame alone only redraws the last ops (the scene is not rebuilt) once they hold the video; anything else rebuilds
    const bool rebuild = dirty || !last_ops || !last_video;
    dirty = false;

    const TimePoint f_t0 = Clock::now();
    Assets& as = app.cx.assets;
    if (rebuild) {
      last_ops = &app.render();
      last_video = false;
      for (const Op& o : *last_ops)
        if (o.kind == Op::Image && video::is_frame(as, o.id)) {
          last_video = true;
          break;
        }
    }
    const std::vector<Op>& ops = *last_ops;
    const TimePoint f_t1 = Clock::now();
    SDL_SetRenderDrawColor(rd, 0, 0, 0, 255);
    SDL_RenderClear(rd);
    for (const Op& op : ops) {
      switch (op.kind) {
        case Op::Rect: {
          SDL_SetRenderDrawColor(rd, op.color[0], op.color[1], op.color[2], op.color[3]);
          SDL_Rect r = to_rect(op);
          SDL_RenderFillRect(rd, &r);
          break;
        }
        case Op::Clip: {
          SDL_Rect r = to_rect(op);
          r.w = std::max(r.w, 1);
          r.h = std::max(r.h, 1);
          SDL_RenderSetClipRect(rd, &r);
          break;
        }
        case Op::Unclip:
          SDL_RenderSetClipRect(rd, nullptr);
          break;
        case Op::Image: {
          if (video::is_frame(as, op.id)) {  // the playing video: its streaming texture, one SDL_UpdateTexture per new frame
            video::FrameStore::Frame vf;
            if (video::acquire(vf)) {
              if (!vtex || vtw != vf.w || vth != vf.h) {
                if (vtex) SDL_DestroyTexture(vtex);
                vtex = SDL_CreateTexture(rd, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, int(vf.w), int(vf.h));
                vtw = vf.w;
                vth = vf.h;
              }
              if (vtex) SDL_UpdateTexture(vtex, nullptr, vf.px, int(vf.w * 4));
            }
            if (vtex) {
              SDL_SetTextureBlendMode(vtex, op.color[3] == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
              SDL_SetTextureAlphaMod(vtex, op.color[3]);
              SDL_Rect r = to_rect(op);
              r.w = std::max(r.w, 1);
              r.h = std::max(r.h, 1);
              SDL_RenderCopy(rd, vtex, nullptr, &r);
            }
            break;
          }
          SDL_Texture* t = texture_for(op.id);
          if (!t) break;
          SDL_SetTextureColorMod(t, op.color[0], op.color[1], op.color[2]);
          SDL_SetTextureAlphaMod(t, op.color[3]);
          SDL_Rect r = to_rect(op);
          r.w = std::max(r.w, 1);
          r.h = std::max(r.h, 1);
          SDL_RenderCopy(rd, t, nullptr, &r);
          break;
        }
      }
    }
    const TimePoint f_t2 = Clock::now();
    if (snap_capture) {
      snap_capture = false;
      int cw = 0, ch = 0;
      SDL_GetRendererOutputSize(rd, &cw, &ch);
      // only the carousel at rest, no video, and the size the key was made for (a changed output is saved by the next start)
      if (cw > 0 && ch > 0 && uint32_t(cw) == g_snap_key.w && uint32_t(ch) == g_snap_key.h && !last_video && !oscontract::idle().active() &&
          app.snapshot_eligible()) {
        std::vector<uint8_t> buf(size_t(cw) * size_t(ch) * 4);
        if (SDL_RenderReadPixels(rd, nullptr, SDL_PIXELFORMAT_RGBA32, buf.data(), cw * 4) == 0) {
          snapshot::Image im = snapshot::downscale_half(buf.data(), uint32_t(cw), uint32_t(ch));
          if (!im.px.empty())  // the disk write (fsync) stays off the draw thread
            app.cx.sv->jobs.post([key = g_snap_key, im = std::move(im)] {
              std::string serr;
              if (!snapshot::save(snapshot::dir(), key, im, serr)) std::fprintf(stderr, "zfront: %s\n", serr.c_str());
            });
        }
      }
    }
    SDL_RenderPresent(rd);
    if (flog) {  // tick dt, scene build, draw calls, present (blocks on vsync), gap since the previous present returned (all ms)
      const TimePoint f_t3 = Clock::now();
      auto ms = [](TimePoint a, TimePoint b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
      std::fprintf(flog, "%.3f %.3f %.3f %.3f %.3f\n", double(tick_dt) * 1000.0, ms(f_t0, f_t1), ms(f_t1, f_t2), ms(f_t2, f_t3), ms(flog_prev, f_t3));
      std::fflush(flog);
      flog_prev = f_t3;
    }
    if (have_pending) {  // disk bookkeeping only once the first frame is on screen
      have_pending = false;
      launch::apply_after_path(app.cx, pending_sys_name, pending_path, pending);
    }
    if (vtex && !video::has_frame()) {  // the video is gone: give its texture back
      SDL_DestroyTexture(vtex);
      vtex = nullptr;
    }
    // textures of evicted images are not drawn again, free them now (only the ids Assets evicted since the last frame)
    as.take_evicted(evicted_ids);
    for (ImgId id : evicted_ids) {
      if (id < textures.size() && textures[id]) {
        SDL_DestroyTexture(textures[id]);
        textures[id] = nullptr;
      }
    }
    if (!first_logged) {
      first_logged = true;
      std::fprintf(stderr, "zfront: first frame, uptime %s\n", uptime_text().c_str());
      if (!homemenu::panel_mode()) {  // the HOME panel is a guest of the frontend: no ready file, no `start` event, no music over the paused game
        oscontract::on_first_frame();  // /tmp/emulationstation.ready + `start` event
        music::start(app.cx);          // background music (ES: AudioManager::init + changePlaylist in main())
      }
    }
  }
  app.set_wake(nullptr);
  if (pad) pad->set_plug_sink(nullptr);
  sound::set_player(nullptr);
  music::shutdown();
  video::shutdown();
  video::set_wake(nullptr);
  close_video();
  audio.shutdown();
  SDL_Quit();
  return ok;
}

}  // namespace zf
