// Device-loop services of the OS contract (started by `Registry::start_services`, never in the harness or the tests).
//   "oscontract": the bounded wait for the Wayland socket (start_es.sh exports ES_WAIT_WAYLAND_SOCKET) right before the loop
//                 opens the window; this runs after the systems were loaded, so the wait overlaps the background counts.
//   "http":       127.0.0.1:1234 (override with ZF_HTTP_PORT for development) -> osapi::route.
#include <cstdio>
#include <cstdlib>

#include "app.hpp"
#include "httpd.hpp"
#include "osapi.hpp"
#include "oscontract.hpp"
#include "registry.hpp"

namespace zf {

namespace {

http::Server& server() {
  static http::Server s;
  return s;
}

}  // namespace

ZF_REGISTER_SERVICE("oscontract", [](App&) { oscontract::wait_display(); });

ZF_REGISTER_SERVICE("http", [](App& app) {
  Services* sv = app.cx.sv.get();  // lives as long as the process: Ctx and oscontract both hold the shared_ptr
  osapi::Ops ops;
  // The UI-side effects run on the main thread (`done` of a job); the job body is empty, so nothing waits behind a slow job.
  ops.notify = [sv](std::string text, int ms) { sv->jobs.post(nullptr, [sv, t = std::move(text), ms]() mutable { sv->toasts.push(std::move(t), ms); }); };
  // Toast deadlines are CLOCK_MONOTONIC, which does not run during suspend: after waking, a notice would reappear. Dropping
  // everything that expires "before the end of time" removes them all.
  ops.notify_clear = [sv] { sv->jobs.post(nullptr, [sv] { sv->toasts.expire(TimePoint::max()); }); };
  ops.quit = [sv](oscontract::Mode m) { sv->jobs.post(nullptr, [m] { oscontract::quit(m); }); };
  ops.launch = [sv](std::string rom) { sv->jobs.post(nullptr, [r = std::move(rom)] { oscontract::request_launch(r); }); };
  long port = 1234;
  if (const char* e = std::getenv("ZF_HTTP_PORT")) {
    long v = std::strtol(e, nullptr, 10);
    if (v > 0 && v < 65536) port = v;
  }
  std::string err;
  bool ok = server().start(uint16_t(port), [ops](const http::Request& r) { return osapi::route(r, ops); }, err);
  if (!ok) {
    std::fprintf(stderr, "zfront: http service not started: %s\n", err.c_str());
    return;
  }
  oscontract::on_finish([] { server().stop(); });
});

}  // namespace zf
