// Device backend: SDL2 window, GPU textures (one per image), keyboard input. It only translates SDL events into
// `Action`s, drives the `App` and executes its operations. It draws only when something changed and, when idle,
// sleeps in `SDL_WaitEvent*` until the next event or the next deadline (status bar, key repeat): no polling.
// Compiled only when SDL2 is available (the aarch64 build; the PC build has no libSDL2).
#pragma once
#include <string>

#include "app.hpp"
#include "pad.hpp"

namespace zf {

/// Runs the loop until quit. `pad` may be null (keyboard only).
bool sdl_run(App& app, Pad* pad, uint32_t w, uint32_t h, bool fullscreen, std::string& err);

}  // namespace zf
