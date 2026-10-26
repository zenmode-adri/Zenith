#include "osapi.hpp"

#include <algorithm>

#include "util.hpp"

namespace zf {
namespace osapi {

namespace {

http::Response reply(int status, std::string body = {}, const char* type = "application/json") {
  http::Response r;
  r.status = status;
  r.content_type = type;
  r.body = std::move(body);
  return r;
}

http::Response err(int status, const char* msg) { return reply(status, std::string("{\"error\":\"") + msg + "\"}"); }

}  // namespace

http::Response route(const http::Request& req, const Ops& ops) {
  const std::string& p = req.path;
  const bool get = req.method == "GET", post = req.method == "POST";

  struct Known {
    const char* path;
    bool want_post;
  };
  static const Known kRoutes[] = {{"/notify", true},    {"/notify/clear", true}, {"/launch", true},  {"/runningGame", false},
                                  {"/isIdle", false},   {"/quit", false},       {"/shutdown", false}, {"/restart", false},
                                  {"/emukill", false}};
  const Known* k = nullptr;
  for (const Known& r : kRoutes)
    if (p == r.path) k = &r;
  if (!k) return err(404, "not found");
  if ((k->want_post && !post) || (!k->want_post && !get)) return err(405, "method not allowed");

  if (p == "/notify") {
    std::string text = oscontract::sanitize_notice(req.body);
    if (text.empty()) return err(400, "body is missing");
    long ms = kDefaultNotifyMs;
    std::string d;
    if (http::query_param(req, "duration", d)) {
      long v = 0;
      if (parse_int(d, v)) ms = std::clamp<long>(v, kMinNotifyMs, kMaxNotifyMs);
    }
    if (ops.notify) ops.notify(std::move(text), int(ms));
    return reply(200);
  }
  if (p == "/notify/clear") {
    if (ops.notify_clear) ops.notify_clear();
    return reply(200);
  }
  if (p == "/runningGame") {
    std::string j = oscontract::running_game_json();
    if (j.empty()) return reply(201, "{\"msg\":\"NO GAME RUNNING\"}");
    return reply(200, std::move(j));
  }
  if (p == "/isIdle") {
    bool idle = !oscontract::game_running() && !oscontract::busy();
    return reply(idle ? 200 : 201, idle ? "[ true ]" : "[ false ]");
  }
  if (p == "/quit" || p == "/shutdown" || p == "/restart") {
    std::string c;
    if (p == "/quit" && http::query_param(req, "confirm", c)) return err(501, "confirm is not supported");  // never quit without the asked-for confirmation
    if (ops.quit) ops.quit(p == "/quit" ? oscontract::Mode::Quit : p == "/shutdown" ? oscontract::Mode::Shutdown : oscontract::Mode::Reboot);
    return reply(200);
  }
  if (p == "/emukill") {
    bool done = oscontract::emukill();
    return reply(200, done ? "{\"killed\":true}" : "{\"killed\":false}");
  }
  // /launch
  std::string rom(trim(req.body));
  if (rom.empty()) return err(400, "body is missing");
  if (rom[0] != '/') return err(400, "absolute path expected");
  if (oscontract::game_running()) return err(409, "a game is running");
  if (ops.launch) ops.launch(std::move(rom));
  return reply(200);
}

}  // namespace osapi
}  // namespace zf
