// Device backend: SDL2 window, GPU textures (one per image), keyboard input. It only translates SDL events into
// `Action`s, drives the `App` and executes its operations. It draws only when something changed and, when idle,
// sleeps in `SDL_WaitEvent*` until the next event or the next deadline (status bar, key repeat): no polling.
// Compiled only when SDL2 is available (the aarch64 build; the PC build has no libSDL2).
#pragma once
#include <functional>
#include <optional>
#include <string>

#include "app.hpp"
#include "pad.hpp"
#include "snapshot.hpp"

namespace zf {

/// The size of the output a fullscreen window will fill, for choosing the theme before loading it; closes SDL again (`sdl_run`
/// opens it for good). Never waits: at boot the frontend starts early so that its loading overlaps Sway's start. False (Sway not
/// up yet) leaves `w`/`h` alone and `sdl_run` corrects a wrong ratio.
bool sdl_output_size(uint32_t& w, uint32_t& h);

/// Instant start: opens the window and shows `im` (the last carousel frame, snapshot.hpp) at once, before the theme and the systems are
/// loaded; `sdl_run` adopts that window and the real first frame replaces the picture. False (no display yet, SDL refused) shows
/// nothing and changes nothing: `sdl_run` opens the window as usual.
bool sdl_splash(const snapshot::Image& im, uint32_t w, uint32_t h, bool fullscreen);

/// Turns on the picture for the NEXT start: `sdl_run` takes it (through `snapshot::save` under this key) when the carousel has
/// been at rest for a few seconds. Without this call nothing is written.
void sdl_set_snapshot_key(const snapshot::Key& key);

/// Runs the loop until quit. `pad` may be null (keyboard only). `startup`, when given, is asked once, before the first input, for the
/// request to act on first (the boot game: an `Outcome::Launch`, or nothing); the POST /launch requests follow it as before.
bool sdl_run(App& app, Pad* pad, uint32_t w, uint32_t h, bool fullscreen, std::string& err,
             std::function<std::optional<Outcome>(App&)> startup = {});

}  // namespace zf
