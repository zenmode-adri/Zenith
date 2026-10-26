// MODULE `osapi` (os-contract): the routes of the local control API (ES's HttpServerThread subset the OS uses).
//
//   POST /notify[?duration=ms]   body = text           -> toast (sleep.sh "Suspending...")
//   POST /notify/clear                                  -> removes the toasts (sleep.sh, after resume)
//   GET  /runningGame            200 {"name","path"} | 201 {"msg":"NO GAME RUNNING"}   (rocknix-fake-suspend branches on it)
//   GET  /isIdle                 200 "[ true ]" | 201 "[ false ]"
//   GET  /quit | /shutdown | /restart                   -> Mode::Quit | Shutdown | Reboot (ES: /restart reboots the SYSTEM)
//   GET  /emukill                                       -> killall <name in /tmp/.process-kill-data> while a game runs
//   POST /launch                 body = ROM path        -> launches that game (409 while another one runs)
//   anything else: 404 JSON; known path with the wrong method: 405.
//
// `route` runs on the HTTP thread: `/runningGame`, `/isIdle` and `/emukill` answer from thread-safe state (the main thread
// is blocked inside the game, which is exactly when they are asked); everything that touches the UI goes through `Ops`,
// whose real implementation posts to `Jobs` (osservice.cpp).
#pragma once
#include <functional>
#include <string>

#include "httpd.hpp"
#include "oscontract.hpp"

namespace zf {
namespace osapi {

constexpr int kDefaultNotifyMs = 5000;
constexpr int kMinNotifyMs = 200;
constexpr int kMaxNotifyMs = 60000;

struct Ops {
  std::function<void(std::string text, int ms)> notify;
  std::function<void()> notify_clear;
  std::function<void(oscontract::Mode)> quit;
  std::function<void(std::string rom_path)> launch;
};

http::Response route(const http::Request& req, const Ops& ops);

}  // namespace osapi
}  // namespace zf
