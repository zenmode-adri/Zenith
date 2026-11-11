// Device-loop services of the OS contract (started by `Registry::start_services`, never in the harness or the tests).
//   "oscontract": the bounded wait for the Wayland socket (start_es.sh exports ES_WAIT_WAYLAND_SOCKET) right before the loop
//                 opens the window; this runs after the systems were loaded, so the wait overlaps the background counts.
//   "http":       127.0.0.1:1234 (override with ZF_HTTP_PORT for development) -> osapi::route.
//                 Web file transfer (opt-in) has its own listener, see webtransfer.hpp; it is started here when enabled.
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "app.hpp"
#include "collections.hpp"
#include "httpd.hpp"
#include "osapi.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "webtransfer.hpp"

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
  ops.reload = [sv] { sv->jobs.post(nullptr, [] { collections::request_rescan(); }); };
  long port = 1234;
  if (const char* e = std::getenv("ZF_HTTP_PORT")) {
    long v = std::strtol(e, nullptr, 10);
    if (v > 0 && v < 65536) port = v;
  }
  // Web file transfer: opt-in (system.cfg zfront.webtransfer), on its own listener started / stopped by the NETWORK menu switch.
  // The HTTP threads only see a snapshot of the systems, refreshed by the main thread; a finished upload asks for a rescan.
  // The systems with no games are read once, in the background (the same loader as the carousel's, with empty ones kept): the
  // phone can send the first game of a system. That is the first time the switch is on, not at every start: a console that never
  // uses the transfer does not read them. Only the main thread touches `empties` (the job's `done` and the publishes).
  auto empties = std::make_shared<std::vector<System>>();
  webtransfer::publish(app.cx.systems);
  auto asked = std::make_shared<bool>(false);
  webtransfer::Env wenv;
  wenv.on_enable = ([sv, &app, empties, asked] {
    if (*asked || !app.cx.system_loader) return;
    *asked = true;
    auto all = std::make_shared<std::vector<System>>();
    sv->jobs.post([all, load = app.cx.system_loader] {
      std::string e;
      if (!load(true, *all, e)) all->clear();
    },
                  [&app, all, empties] {
                    *empties = std::move(*all);
                    webtransfer::publish(app.cx.systems, empties.get());
                  });
  });
  wenv.reload = ops.reload;
  wenv.launch = ops.launch;  // the same path as POST /launch of the control API
  // a system whose games are not read yet: open it on the main thread (the phone asks again in a moment)
  wenv.open_system = ([sv, &app, empties](const std::string& name) {
    sv->jobs.post(nullptr, [&app, name, empties] {
      for (size_t i = 0; i < app.cx.systems.size(); ++i)
        if (app.cx.systems[i].name == name && !app.cx.systems[i].loaded) app.cx.open_system(i);
      webtransfer::publish(app.cx.systems, empties.get());
    });
  });
  wenv.refresh = ([sv, &app, empties] { sv->jobs.post(nullptr, [&app, empties] { webtransfer::publish(app.cx.systems, empties.get()); }); });  // parentheses: a comma in a capture list would split the macro argument
  wenv.on_main = ([sv, &app](std::function<void(App&)> f) { sv->jobs.post(nullptr, [&app, f = std::move(f)] { f(app); }); });  // parentheses: see above
  webtransfer::set_env(wenv);
  oscontract::on_finish([] { webtransfer::stop_listener(); });
  if (app.cx.conf.get_bool("zfront.webtransfer", false)) {
    webtransfer::apply(true);
    if (!webtransfer::listening()) std::fprintf(stderr, "zfront: web file transfer not started: %s\n", webtransfer::listen_error().c_str());
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
