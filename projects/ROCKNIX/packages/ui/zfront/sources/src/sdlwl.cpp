// The Wayland connection's socket, for the idle sleep (idlewait.hpp). Separate file because SDL_syswm.h pulls in
// X11/Xlib.h, whose `None`, `Status`... macros break the rest of the code base.
#ifdef ZF_WITH_SDL
#include "sdlwl.hpp"

#include <SDL.h>
#include <SDL_syswm.h>
#include <dlfcn.h>

namespace zf {

int wayland_display_fd(SDL_Window* win) {
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WAYLAND || !info.info.wl.display) return -1;
  // libwayland is not linked: SDL already loaded it with dlopen, so RTLD_NOLOAD finds that copy
  void* lib = ::dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_NOLOAD);
  if (!lib) return -1;
  using GetFd = int (*)(struct wl_display*);
  GetFd get_fd = reinterpret_cast<GetFd>(::dlsym(lib, "wl_display_get_fd"));
  int fd = get_fd ? get_fd(info.info.wl.display) : -1;
  ::dlclose(lib);  // SDL keeps its own reference
  return fd;
}

}  // namespace zf
#endif
