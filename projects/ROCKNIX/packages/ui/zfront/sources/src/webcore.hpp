// MODULE `webcore`: what the feature modules of the web file transfer share (QR code and paired phones, folders, BIOS, saves,
// game management, WebDAV...). A module lives in its own file and registers itself, so webtransfer.cpp does not change for it:
//
//   ZF_REGISTER_WEB_MODULE([] {
//     webtransfer::WebModule m;
//     m.name = "bios";
//     m.order = 40;
//     m.owns = [](std::string_view path) { return path == "/files/bios" || path == "/files/bios/check"; };
//     m.route = [](const http::Request& req, const webtransfer::Env& env) { ... return webtransfer::reply(200, "{}"); };
//     m.tab_id = "bi";
//     m.tab_label = "BIOS";
//     m.section = "<p id=\"bnote\"></p>";
//     m.js = "Z.ontab.bi=function(){ ... };";
//     return m;
//   });
//
// The core answers 404 to a path no module owns, then asks `authorize` (unless the module says the path is public), then calls
// the module. Streamed bodies (uploads of any size) go through `stream_owns` / `stream` on the transfer worker; a route can
// answer with `Response::file` to send a big file without loading it. Tabs follow the core ones (Upload, Games, Screenshots)
// in `order`; each module's `js` runs in its own scope, `(function(Z){ ... })(Z)`, after the core script. `Z` is the page's
// toolkit (see the end of the core script in page_html()):
//   Z.$(id)  Z.say(text, bad)  Z.why(xhr) -> error text  Z.call(method, path, done(xhr|null), blob)  Z.jerr(xhr) -> true if it
//   failed (and said so)  Z.load(done(ok))  Z.fill()  Z.systems()  Z.pic(img, path)  Z.resetpics()  Z.tab(id)  Z.ontab[id] = fn
//   Z.auth(xhr) adds the credentials to a request you build yourself  Z.nextReq() / Z.isReq(n): drop stale answers.
// Module ids, tab ids and paths are ASCII; the core escapes `tab_label`. Rows for the console's NETWORK menu go under WEB PIN
// while the transfer is listening (`network_rows`). Everything a module writes to disk takes its path from an environment
// variable in tests (never /storage).
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "httpd.hpp"
#include "registry.hpp"
#include "webtransfer.hpp"

namespace zf {
struct Ctx;
class MenuBuilder;
namespace webtransfer {

struct WebModule {
  std::string name;
  int order = 100;
  /// The plain (small body) paths this module answers (normally below /files/; WebDAV uses /dav/ and OPTIONS on /).
  std::function<bool(std::string_view path)> owns;
  /// Called after `authorize` (or without it for a public path); the method is the module's to check.
  std::function<http::Response(const http::Request& req, const Env& env)> route;
  /// Paths answered WITHOUT `authorize` (pairing itself). Default: none.
  std::function<bool(std::string_view path)> public_path;
  /// Streamed bodies (uploads): which (method, path) this module takes, and the handler (runs on the transfer worker).
  std::function<bool(std::string_view method, std::string_view path)> stream_owns;
  std::function<http::Response(const http::Request& req, http::BodyStream& body, const Env& env)> stream;
  // ---- the page
  std::string tab_id, tab_label;  // a tab button and its <section id="p-<tab_id>"> (empty: no tab)
  std::string css;                // appended to the page's <style>
  std::string section;            // inner HTML of that section
  std::string html;               // outside the tabs, before the script (dialogs, overlays)
  std::string js;                 // body of (function(Z){ ... })(Z)
  // ---- the console's NETWORK menu: rows under WEB PIN while the transfer listens (`m.refresh` rebuilds the menu)
  std::function<void(MenuBuilder& m, Ctx& cx)> network_rows;
};

/// Static registration (ZF_REGISTER_WEB_MODULE); modules are kept sorted by `order`.
void add_module(WebModule m);
/// The registered modules, sorted by `order` (filled before main(), read-only afterwards).
const std::vector<WebModule>& modules();

struct WebModuleRegistrar {
  explicit WebModuleRegistrar(WebModule (*make)()) { add_module(make()); }
};
#define ZF_REGISTER_WEB_MODULE(make) static ::zf::webtransfer::WebModuleRegistrar ZF_CAT(zf_webmodule_, __LINE__)(make)

// ---- shared helpers ----------------------------------------------------------------------------------------------------
/// The request may use the transfer: the token of a paired phone in X-Token (webpair.hpp), or the right PIN in X-PIN. On failure `deny`
/// holds the answer (403, or 429 while locked out after five wrong PINs). A wrong token counts as a wrong PIN for the lock-out.
bool authorize(const http::Request& req, http::Response& deny);
/// Only the PIN (X-PIN), with the same lock-out: for the one route that hands out tokens (`POST /files/pair`).
bool authorize_pin(const http::Request& req, http::Response& deny);
http::Response reply(int status, std::string body, const char* type = "application/json");
/// {"error":"<msg>"} with `status` (`msg` is a literal: not escaped).
http::Response err(int status, const char* msg);
long long now_ms();
std::string lower(std::string s);
/// A copy of one system of the snapshot.
bool find_system(const std::string& name, SystemInfo& out);
/// A copy of the whole snapshot.
std::vector<SystemInfo> systems_snapshot();
struct FoundGame {
  std::string name, path, system, rom_dir, gamelist;
  GameMeta meta;
};
/// The game of the snapshot whose id (`game_id`) this is.
bool find_game(const std::string& id, FoundGame& out);
/// Free bytes of the file system holding `dir` (statvfs).
bool real_free_space(const std::string& dir, uint64_t& out);
/// Numeric query parameter: `def` when absent or not a number, at most `max`.
size_t num_param(const http::Request& req, const char* key, size_t def, size_t max);

}  // namespace webtransfer
}  // namespace zf
