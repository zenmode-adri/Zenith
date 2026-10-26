// Device only (SDL): see sdlwl.cpp.
#pragma once

struct SDL_Window;

namespace zf {

/// The Wayland connection's socket of `win`, or -1 (other video driver, libwayland not loaded). The descriptor belongs to
/// SDL: it may be polled, never read nor closed.
int wayland_display_fd(SDL_Window* win);

}  // namespace zf
