// MODULE `webtransfer`: upload ROMs from a phone / PC browser on the same network (opt-in, default OFF).
//
//   GET  /files                  one self-contained HTML page (inline CSS/JS, no external resources): asks for the PIN,
//                                lists the systems, uploads one or more files to the chosen system
//   GET  /files/systems          JSON list of the systems (header X-PIN)
//   POST /files/upload?system=<name>&name=<file>[&replace=1]    body = the file, STREAMED (header X-PIN)
//   GET  /files/games?system=<name>&q=<text>[&offset=&limit=]   JSON: id, name, system, cover flag (header X-PIN)
//   POST /files/launch?id=<game id>   launch a game of the snapshot like the control API's /launch (409 while a game runs)
//   POST /files/quit                  close the running game (like /emukill)
//   GET  /files/shots                 JSON list of the screenshots folder, newest first
//   GET  /files/shot?id=<file name>   one screenshot (a plain file name inside that folder only)
//   GET  /files/cover?id=<game id>    the gamelist picture of a game (only when it lies inside the ROM folder)
// Game ids are a hash of the ROM path, mapped back to the path from the snapshot: the client can never name a path.
//
// The routes live on their OWN listener (`start_listener`: port 8080, every interface), started when the switch turns on and
// stopped when it turns off, so nothing listens on the network while the feature is off. The control server (127.0.0.1:1234)
// never serves /files and the listener never serves the control routes.
// Everything answers 404 while the switch (system.cfg `zfront.webtransfer`) is off. The PIN is 4 digits, random, made once
// per zfront start and never stored; five wrong PINs in a row lock every request for 30 s (answer 429), so the PIN cannot be
// brute-forced from the network. The upload goes to `<rom_dir>/<name>.part` (fsync) and is renamed on success; names with
// '/', '\\', "..", control characters or a leading '.' and extensions the system does not list are refused; an existing file
// is never overwritten unless `replace=1`. The routes run on the HTTP threads; the list of systems is a snapshot published by
// the main thread (`publish`), never `cx.systems` itself. Nothing here deletes or lists files.
// More routes and page tabs come from feature modules registered through webcore.hpp, each documented in its own file:
// webput.cpp (folders, resumable uploads), webbios.cpp (BIOS files and their md5), websaves.cpp (save backups).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "httpd.hpp"
#include "systems.hpp"

namespace zf {
class App;
namespace webtransfer {

struct GameInfo {
  std::string name, path;  // path: absolute, as in the gamelist; never sent to clients
  GameMeta meta;           // for `gamelist_details` (the cover)
};

struct SystemInfo {
  std::string name, fullname, rom_dir;
  std::vector<std::string> exts;  // ".gb" ...
  size_t count = 0;
  std::string gamelist;           // gamelist.xml of the system
  bool loaded = false;            // `games` is filled (the main thread opened the system)
  std::vector<GameInfo> games;
};

/// Seams: tests inject `free_space`; the real service posts to the main thread for `reload` and `refresh`.
struct Env {
  /// Free bytes of the file system holding `dir`; false when unknown. Default (empty): statvfs.
  std::function<bool(const std::string& dir, uint64_t& free_bytes)> free_space;
  std::function<void()> reload;   // after a successful upload: read the game folders again
  std::function<void()> refresh;  // before listing: ask the main thread for a fresh snapshot of the systems
  std::function<void(const std::string& rom_path)> launch;  // POST /files/launch: same effect as the control API's /launch (posts to the main thread)
  std::function<void(const std::string& system)> open_system;  // GET /files/games of a system not loaded yet: open it on the main thread, then publish
  std::string shots_dir;          // screenshots folder; empty = ssmedia::kScreenshotsDir
  /// Runs `f` on the main thread, later (dialogs, toasts, gamelist edits). Empty in tests that do not need it.
  std::function<void(std::function<void(App&)> f)> on_main;
  /// The switch was turned on and the listener runs (the caller's thread: the main one). What only the transfer needs is prepared here.
  std::function<void()> on_enable;
};

// ---- state (thread-safe) -----------------------------------------------------------------------------------------------
void set_enabled(bool on);
bool enabled();
/// 4 digits, random, chosen on first use in this process.
std::string pin();
constexpr uint16_t kWebPort = 8080;
/// The seams the listener uses (set once at start-up, before `start_listener`).
void set_env(Env env);
/// Port to listen on (default `kWebPort`; 0 = any, tests).
void set_listen_port(uint16_t port);
/// Switch turned on / off: `set_enabled` plus start / stop of the listener. False when it could not listen (`listen_error`).
bool apply(bool on);
bool start_listener();
void stop_listener();
bool listening();
uint16_t listen_port();  // actual port while listening, else the wanted one
std::string listen_error();
/// Main thread: snapshot of the systems for the HTTP threads.
/// `also`: the systems of es_systems.cfg with no games (not in the carousel): they are upload targets too, so the first game of a
/// system can be sent from the phone. Those already in `systems` are skipped.
void publish(const std::vector<System>& systems, const std::vector<System>* also = nullptr);
void publish_info(std::vector<SystemInfo> info);
/// Tests: fixed PIN, fresh lock-out counters.
void reset_for_test(const std::string& fixed_pin);

// ---- pure helpers (tests) ----------------------------------------------------------------------------------------------
/// Id of a game: 16 hex digits (FNV-1a of its path).
std::string game_id(const std::string& path);
/// A plain file name: not empty, at most 200 bytes, no '/', '\\', control characters, no "..", no leading '.'.
bool valid_name(const std::string& name);
/// The extension of `name` (with the dot, compared without case) is in `exts`.
bool ext_allowed(const std::string& name, const std::vector<std::string>& exts);

// ---- routes ------------------------------------------------------------------------------------------------------------
/// GET /files, /files/, /files/systems and the phone-remote routes above (anything else below /files: 404).
http::Response route(const http::Request& req, const Env& env);
/// POST /files/upload (called by the stream worker; reads the body from `body`).
http::Response upload(const http::Request& req, http::BodyStream& body, const Env& env);
/// The listener's streamed requests: POST /files/upload and those of the modules (webcore.hpp).
bool stream_pred(std::string_view method, std::string_view path);
http::Response stream_route(const http::Request& req, http::BodyStream& body, const Env& env);
/// The HTML page (tests look for external references in it).
const char* page_html();

}  // namespace webtransfer
}  // namespace zf
