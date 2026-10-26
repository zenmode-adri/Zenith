#include "sdlr.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "idlewait.hpp"
#include "launch.hpp"
#include "oscontract.hpp"
#include "sdlwl.hpp"

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

SDL_Rect to_rect(const Op& op) {
  return SDL_Rect{int(std::lround(op.x)), int(std::lround(op.y)), int(std::lround(op.w)), int(std::lround(op.h))};
}

// SDL_WaitEvent/WaitEventTimeout poll every 1 ms while a joystick is open (SDL_events_need_polling): ~1000 wake-ups and
// ~3.3 % of a core while idle. We sleep in `IdleWaiter` (idlewait.hpp: poll(2) on the pad evdev nodes, a wake-up
// eventfd and, on Wayland, the display socket).

/// Re-open the evdev nodes of the joysticks SDL currently knows.
void rebuild_pads(IdleWaiter& iw) {
  std::vector<std::string> paths;
  int n = SDL_NumJoysticks();
  for (int i = 0; i < n; ++i)
    if (const char* p = SDL_JoystickPathForIndex(i)) paths.emplace_back(p);
  iw.set_pad_paths(paths);
}

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
    iw.sleep(left);
    if (oscontract::resume_pending()) return false;  // the machine was suspended meanwhile: let the loop end the screensaver
  }
}

// Milliseconds until `t` (at least 1, at most `cap`).
int ms_until(TimePoint t, TimePoint now, int cap) {
  if (t <= now) return 1;
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(t - now).count();
  return int(std::min<long long>(us / 1000 + 1, cap));
}

}  // namespace

bool sdl_run(App& app, Pad* pad, uint32_t w, uint32_t h, bool fullscreen, std::string& err) {
  SDL_Window* win = nullptr;
  SDL_Renderer* rd = nullptr;
  std::vector<SDL_Texture*> textures;
  bool first_open = true;
  IdleWaiter iw;
  // Window + renderer + joystick subsystem. Called at start and again after every game (the window must NOT exist
  // while the emulator runs: it needs the display).
  auto open_video = [&](std::string& e) -> bool {
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
    win = SDL_CreateWindow("zfront", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, int(w), int(h), fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
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
    int cw = 0, ch = 0;
    SDL_GetRendererOutputSize(rd, &cw, &ch);
    app.cx.w = float(cw);
    app.cx.h = float(ch);
    if (first_open && std::string(ratio_name(w, h)) != ratio_name(unsigned(cw), unsigned(ch))) {
      // the theme was loaded for the size on the command line (720x720 when there is none); the real output has another ratio
      Theme t;
      std::string terr;
      if (Theme::load(app.cx.th.dir(), ratio_name(unsigned(cw), unsigned(ch)), t, terr)) {
        app.cx.th = std::move(t);
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
    return true;
  };
  auto close_video = [&]() {
    iw.set_display_fd(-1);  // the socket is SDL's: forget it before SDL_Quit closes it
    iw.close_pads();
    for (SDL_Texture*& t : textures) {
      if (t) SDL_DestroyTexture(t);
      t = nullptr;
    }
    textures.clear();
    app.cx.assets.forget_released();  // images that only lived in those textures are rebuilt by key on the next frame
    if (rd) SDL_DestroyRenderer(rd);
    if (win) SDL_DestroyWindow(win);
    rd = nullptr;
    win = nullptr;
    SDL_Quit();
  };
  if (!open_video(err)) return false;
  // background counts wake the loop with a user event (SDL_PushEvent is thread-safe)
  Uint32 wake_type = SDL_RegisterEvents(1);
  // The event goes first and the eventfd second: `poll` returns on the eventfd and the loop must then find the event.
  // (The eventfd is shared: a worker calling this while we shut down never touches a closed descriptor.)
  app.set_wake([wake_type, wake_iw = iw.wake_function()] {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = wake_type;
    SDL_PushEvent(&ev);
    wake_iw();
  });
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
  size_t pending_sys = 0, pending_game = 0;

  std::vector<Action> acts;
  acts.reserve(8);
  bool dirty = true, first_logged = false, quit = false;
  TimePoint last = Clock::now();

  while (!quit) {
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
      } else {
        dl = app.status_deadline();
        if (std::optional<TimePoint> sd = app.screen_deadline(); sd && (!dl || *sd < *dl)) dl = sd;
        if (pad) {
          std::optional<TimePoint> pd = pad->next_deadline();
          if (pd && (!dl || *pd < *dl)) dl = pd;
        }
        std::optional<TimePoint> qd = prep.next_deadline();  // cursor rest time of the idle-time preparation
        if (qd && (!dl || *qd < *dl)) dl = qd;
        std::optional<TimePoint> sd2 = oscontract::idle_deadline(app.cx.es);  // screensaver timer
        if (sd2 && (!dl || *sd2 < *dl)) dl = sd2;
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
    oscontract::before_acts(app.cx.es, Clock::now(), acts, raw_input);  // screensaver start; the key that wakes it is dropped
    if (oscontract::take_redraw()) dirty = true;  // overlay on / off
    // jobs first: POST /launch arrives as a job `done` (request_launch); without this it would wait for the next event
    if (app.drain_jobs(Clock::now())) dirty = true;
    std::optional<Outcome> ext = oscontract::take_external(app);  // POST /launch
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
        launch::Env env;
        env.prep = &prep;
        env.release = [&] {
          // our evdev descriptors and the display socket, then the joysticks, window/renderer/SDL (no audio device is ever opened)
          iw.close_pads();
          iw.set_display_fd(-1);
          if (pad) pad->detach();
          close_video();
        };
        env.restore = [&] {
          if (!open_video(err)) {
            ok = false;
            quit = true;
            return;
          }
          SDL_PumpEvents();
          SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);  // discard what was typed while the game ran
          if (pad) pad->clear();
          keys_down = 0;
          rebuild_pads(iw);  // the joysticks opened by the new window (open_video also acquired the display socket)
        };
        oscontract::game_started(rq.rom_raw.empty() ? rq.cmd : rq.rom_raw, rq.name);
        pending = launch::execute(rq, env);
        oscontract::game_ended();
        if (!pending.message.empty()) {
          std::fprintf(stderr, "zfront: AN ERROR OCCURRED: %s\n", pending.message.c_str());
          app.cx.sv->toasts.push(launch::error_toast(pending), 7000);
        }
        have_pending = known;
        pending_sys = known ? size_t(o.sys) : 0;
        pending_game = known ? size_t(o.game) : 0;
        if (launch::term_requested()) {  // SIGTERM during the game: the window was not rebuilt, bookkeeping now and out
          if (have_pending) launch::apply_after(app.cx, pending_sys, pending_game, pending);
          have_pending = false;
          quit = true;
        }
        if (quit) break;
      }
      dirty = true;
      last = Clock::now();
    }
    if (!acts.empty()) prep.set_target(app.launch_target(), Clock::now());  // arms the dwell timer of the preparation
    if (!acts.empty() || raw_input) oscontract::input(Clock::now());  // after the actions: a game may have run for hours inside
    acts.clear();
    if (quit) break;
    if (app.drain_counts()) dirty = true;
    if (app.drain_jobs(Clock::now())) dirty = true;
    prep.poll(Clock::now());
    TimePoint now = Clock::now();
    // screensaver on: the screens underneath are frozen (no animation frames, no timers); `last` follows so that the first
    // tick after waking does not see the whole sleep as one huge dt
    if (!oscontract::idle().active() && app.tick(std::chrono::duration<float>(now - last).count())) dirty = true;
    last = now;
    if (app.poll_status(now)) dirty = true;
    if (!dirty) continue;
    dirty = false;

    const std::vector<Op>& ops = app.render();
    Assets& as = app.cx.assets;
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
          if (textures.size() <= op.id) textures.resize(std::max<size_t>(as.count(), op.id + 1), nullptr);
          const Img& im = as.img(op.id);
          if (as.evicted(op.id)) {
            if (textures[op.id]) SDL_DestroyTexture(textures[op.id]);
            textures[op.id] = nullptr;
            break;
          }
          if (!textures[op.id]) {
            if (!as.has_pixels(op.id)) break;  // cannot happen: textures are only dropped together with `forget_released`
            SDL_Texture* t = SDL_CreateTexture(rd, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, int(im.w), int(im.h));
            if (!t) break;
            SDL_UpdateTexture(t, nullptr, im.rgba.data(), int(im.w * 4));
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
            textures[op.id] = t;
            as.release_pixels(op.id);  // the texture holds them now: do not keep a second copy in RAM (pinned images keep theirs)
          }
          SDL_Texture* t = textures[op.id];
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
    SDL_RenderPresent(rd);
    if (have_pending) {  // disk bookkeeping only once the first frame is on screen
      have_pending = false;
      launch::apply_after(app.cx, pending_sys, pending_game, pending);
    }
    // textures of evicted images are not drawn again, free them now
    for (size_t id = 0; id < textures.size(); ++id) {
      if (textures[id] && (id >= as.count() || as.evicted(ImgId(id)))) {
        SDL_DestroyTexture(textures[id]);
        textures[id] = nullptr;
      }
    }
    if (!first_logged) {
      first_logged = true;
      std::fprintf(stderr, "zfront: first frame, uptime %s\n", uptime_text().c_str());
      oscontract::on_first_frame();  // /tmp/emulationstation.ready + `start` event
    }
  }
  app.set_wake(nullptr);
  if (pad) pad->set_plug_sink(nullptr);
  close_video();
  return ok;
}

}  // namespace zf
