#include "webtransfer.hpp"

#include <fcntl.h>
#include <dirent.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

#include "gamelist.hpp"
#include "webcore.hpp"
#include "webpair.hpp"
#include "oscontract.hpp"
#include "ssmedia.hpp"

namespace zf {
namespace webtransfer {

namespace {

constexpr int kMaxWrongPins = 5;
constexpr long long kLockMs = 30000;
constexpr size_t kMaxNameBytes = 200;  // NAME_MAX is 255 and ".part" is added
constexpr long long kLaunchGapMs = 4000;  // a second launch request this soon after one is refused (a double tap)
constexpr size_t kMaxImageBytes = 8u << 20;
constexpr size_t kMaxShots = 100;

struct State {
  std::atomic<bool> on{false};
  std::mutex m;  // everything below
  std::string pin;
  int wrong = 0;
  long long locked_until_ms = 0;
  long long launch_block_until_ms = 0;
  std::vector<SystemInfo> systems;
  std::vector<SystemInfo> empties;  // the systems with no games last read (publish with `also`), kept by a publish without them
};

State& st() {
  static State s;
  return s;
}

struct Listener {
  std::mutex m;
  http::Server srv;
  Env env;
  uint16_t want = kWebPort;
  std::string error;
};

Listener& ls() {
  static Listener l;
  return l;
}

}  // namespace

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

http::Response reply(int status, std::string body, const char* type) {
  http::Response r;
  r.status = status;
  r.content_type = type;
  r.body = std::move(body);
  return r;
}

http::Response err(int status, const char* msg) { return reply(status, std::string("{\"error\":\"") + msg + "\"}"); }

static std::string make_pin() {
  uint32_t v = 0;
  if (::getrandom(&v, sizeof v, 0) != ssize_t(sizeof v)) v = uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^ uint32_t(::getpid()) * 2654435761u;
  char b[8];
  std::snprintf(b, sizeof b, "%04u", unsigned(v % 10000u));
  return b;
}

// The PIN of this request is right (and nobody is locked out). On failure `deny` holds the answer.
static bool check_pin(const http::Request& req, http::Response& deny) {
  State& s = st();
  std::string given;
  const bool have = http::header_value(req, "x-pin", given);
  std::lock_guard<std::mutex> lk(s.m);
  if (s.pin.empty()) s.pin = make_pin();
  const long long t = now_ms();
  if (t < s.locked_until_ms) {
    deny = err(429, "too many wrong PINs, wait a little");
    return false;
  }
  // compare every byte whatever the input, so the time says nothing about how many digits matched
  unsigned diff = given.size() == s.pin.size() ? 0u : 1u;
  for (size_t i = 0; i < s.pin.size(); ++i) diff |= unsigned(static_cast<unsigned char>(i < given.size() ? given[i] : 0) ^ static_cast<unsigned char>(s.pin[i]));
  if (!have || diff != 0) {
    if (++s.wrong >= kMaxWrongPins) {
      s.wrong = 0;
      s.locked_until_ms = t + kLockMs;
    }
    deny = err(403, "wrong PIN");
    return false;
  }
  s.wrong = 0;
  return true;
}

bool authorize_pin(const http::Request& req, http::Response& deny) { return check_pin(req, deny); }

bool authorize(const http::Request& req, http::Response& deny) {
  std::string token;
  // a paired device gets in whatever the PIN lock-out says: someone guessing PINs must not be able to shut the owner's phone out
  if (http::header_value(req, "x-token", token) && webpair::token_valid(token)) return true;
  return check_pin(req, deny);  // no token, or a wrong one: the PIN decides, and a miss counts for the lock-out
}

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return s;
}

bool find_system(const std::string& name, SystemInfo& out) {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.m);
  for (const SystemInfo& i : s.systems)
    if (i.name == name) {
      out = i;
      return true;
    }
  return false;
}

bool real_free_space(const std::string& dir, uint64_t& out) {
  struct statvfs sv;
  if (::statvfs(dir.c_str(), &sv) != 0) return false;
  out = uint64_t(sv.f_bavail) * uint64_t(sv.f_frsize);
  return true;
}

std::vector<SystemInfo> systems_snapshot() {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.m);
  return s.systems;
}

namespace {

std::string systems_json() {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.m);
  std::string j = "[";
  for (const SystemInfo& i : s.systems) {
    if (j.size() > 1) j += ",";
    j += "{\"name\":\"" + oscontract::json_escape(i.name) + "\",\"fullname\":\"" + oscontract::json_escape(i.fullname) + "\",\"dir\":\"" +
         oscontract::json_escape(i.rom_dir) + "\",\"exts\":[";
    for (size_t k = 0; k < i.exts.size(); ++k) j += (k ? ",\"" : "\"") + oscontract::json_escape(i.exts[k]) + "\"";
    j += "],\"count\":" + (i.count == kCountPending ? std::string("null") : std::to_string(i.count)) + "}";  // null: not counted yet
  }
  return j + "]";
}

}  // namespace

// ---- feature modules (webcore.hpp) -------------------------------------------------------------------------------------

namespace {

std::vector<WebModule>& mods() {
  static std::vector<WebModule> v;
  return v;
}

bool is_public(std::string_view path) {
  for (const WebModule& m : mods())
    if (m.public_path && m.public_path(path)) return true;
  return false;
}

http::Response module_route(const http::Request& req, const Env& env) {
  for (const WebModule& m : mods()) {
    if (!m.owns || !m.owns(req.path) || !m.route) continue;
    http::Response deny;
    if (!is_public(req.path) && !authorize(req, deny)) return deny;
    return m.route(req, env);
  }
  return err(404, "not found");
}

}  // namespace

void add_module(WebModule m) {
  std::vector<WebModule>& v = mods();
  auto at = std::upper_bound(v.begin(), v.end(), m.order, [](int o, const WebModule& w) { return o < w.order; });
  v.insert(at, std::move(m));
}

const std::vector<WebModule>& modules() { return mods(); }

bool stream_pred(std::string_view method, std::string_view path) {
  if (method == "POST" && path == http::kStreamPath) return true;
  for (const WebModule& w : mods())
    if (w.stream_owns && w.stream && w.stream_owns(method, path)) return true;
  return false;
}

http::Response stream_route(const http::Request& req, http::BodyStream& body, const Env& env) {
  if (req.method == "POST" && req.path == http::kStreamPath) return upload(req, body, env);
  for (const WebModule& w : mods()) {
    if (!w.stream_owns || !w.stream || !w.stream_owns(req.method, req.path)) continue;
    if (!enabled()) return err(404, "not found");
    http::Response deny;
    if (!is_public(req.path) && !authorize(req, deny)) return deny;
    return w.stream(req, body, env);
  }
  return err(404, "not found");
}

// ---- state ------------------------------------------------------------------------------------------------------------

void set_enabled(bool on) { st().on = on; }

void set_env(Env env) {
  std::lock_guard<std::mutex> lk(ls().m);
  ls().env = std::move(env);
}
void set_listen_port(uint16_t p) {
  std::lock_guard<std::mutex> lk(ls().m);
  ls().want = p;
}
bool start_listener() {
  Listener& l = ls();
  std::lock_guard<std::mutex> lk(l.m);
  if (l.srv.running()) return true;
  l.error.clear();
  const Env env = l.env;
  l.srv.set_stream_handler([env](const http::Request& r, http::BodyStream& b) { return stream_route(r, b, env); }, stream_pred);
  std::string err;
  // route() answers 404 to everything that is not /files*: the control API does not exist here
  bool ok = l.srv.start(l.want, [env](const http::Request& r) { return route(r, env); }, err, "0.0.0.0");
  if (!ok) l.error = err;
  return ok;
}
void stop_listener() {
  Listener& l = ls();
  std::lock_guard<std::mutex> lk(l.m);
  l.srv.stop();
}
bool listening() {
  Listener& l = ls();
  std::lock_guard<std::mutex> lk(l.m);
  return l.srv.running();
}
uint16_t listen_port() {
  Listener& l = ls();
  std::lock_guard<std::mutex> lk(l.m);
  return l.srv.running() ? l.srv.port() : l.want;
}
std::string listen_error() {
  Listener& l = ls();
  std::lock_guard<std::mutex> lk(l.m);
  return l.error;
}
bool apply(bool on) {
  set_enabled(on);
  if (!on) {
    stop_listener();
    return true;
  }
  if (!start_listener()) return false;
  std::function<void()> f;
  {
    std::lock_guard<std::mutex> lk(ls().m);
    f = ls().env.on_enable;
  }
  if (f) f();
  return true;
}
bool enabled() { return st().on.load(); }

std::string pin() {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.m);
  if (s.pin.empty()) s.pin = make_pin();
  return s.pin;
}

void publish_info(std::vector<SystemInfo> info) {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.m);
  s.systems = std::move(info);
}

void publish(const std::vector<System>& systems, const std::vector<System>* also) {
  std::vector<SystemInfo> v;
  auto have = [&v](const std::string& name) { return std::any_of(v.begin(), v.end(), [&](const SystemInfo& i) { return i.name == name; }); };
  for (const System& sy : systems) {
    if (sy.rom_dir.empty() || sy.name.empty()) continue;
    SystemInfo i;
    i.name = sy.name;
    i.fullname = sy.fullname;
    i.rom_dir = sy.rom_dir;
    while (i.rom_dir.size() > 1 && i.rom_dir.back() == '/') i.rom_dir.pop_back();
    i.exts = sy.exts;
    i.count = sy.loaded ? sy.games.size() : sy.count;
    i.gamelist = sy.gamelist;
    i.loaded = sy.loaded;
    if (sy.loaded) {
      i.games.reserve(sy.games.size());
      for (const Game& g : sy.games) i.games.push_back(GameInfo{g.name, g.path, g.meta});
    }
    v.push_back(std::move(i));
  }
  std::vector<SystemInfo> empties;
  if (also)
    for (const System& sy : *also) {
      if (sy.rom_dir.empty() || sy.name.empty()) continue;
      SystemInfo i;
      i.name = sy.name;
      i.fullname = sy.fullname;
      i.rom_dir = sy.rom_dir;
      while (i.rom_dir.size() > 1 && i.rom_dir.back() == '/') i.rom_dir.pop_back();
      i.exts = sy.exts;
      i.count = sy.count;
      i.gamelist = sy.gamelist;
      empties.push_back(std::move(i));
    }
  State& s = st();
  std::lock_guard<std::mutex> lk(s.m);
  // a publish without them (the hot reload of the carousel) keeps the empty systems read last time: otherwise a system with no
  // games vanished from the phone until the next refresh, and its first game could not be sent
  if (also) s.empties = std::move(empties);
  for (const SystemInfo& e : s.empties)
    if (!have(e.name)) v.push_back(e);
  s.systems = std::move(v);
}

void reset_for_test(const std::string& fixed_pin) {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.m);
  s.pin = fixed_pin;
  s.wrong = 0;
  s.locked_until_ms = 0;
  s.launch_block_until_ms = 0;
  s.systems.clear();
  s.empties.clear();
  s.on = false;
}

// ---- helpers ----------------------------------------------------------------------------------------------------------

bool valid_name(const std::string& n) {
  if (n.empty() || n.size() > kMaxNameBytes || n[0] == '.') return false;
  if (n.find("..") != std::string::npos) return false;
  for (char c : n) {
    unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f || c == '/' || c == '\\') return false;
  }
  return true;
}

bool ext_allowed(const std::string& name, const std::vector<std::string>& exts) {
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot + 1 >= name.size()) return false;
  const std::string e = lower(name.substr(dot));
  for (const std::string& x : exts)
    if (lower(x) == e) return true;
  return false;
}

std::string game_id(const std::string& path) {
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (char c : path) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  char b[20];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
  return b;
}

namespace {

bool valid_id(const std::string& id) {
  if (id.size() != 16) return false;
  for (char c : id)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

}  // namespace

// The game of the snapshot whose id this is (copy; the snapshot belongs to the main thread's publishing).
bool find_game(const std::string& id, FoundGame& out) {
  if (!valid_id(id)) return false;
  State& s = st();
  std::lock_guard<std::mutex> lk(s.m);
  for (const SystemInfo& si : s.systems)
    for (const GameInfo& g : si.games)
      if (game_id(g.path) == id) {
        out = FoundGame{g.name, g.path, si.name, si.rom_dir, si.gamelist, g.meta};
        return true;
      }
  return false;
}

namespace {

const char* image_mime(const std::string& name) {
  size_t dot = name.rfind('.');
  if (dot == std::string::npos) return nullptr;
  const std::string e = lower(name.substr(dot));
  if (e == ".png") return "image/png";
  if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
  if (e == ".gif") return "image/gif";
  if (e == ".webp") return "image/webp";
  return nullptr;
}

// A regular file (no symlink at the end) of at most kMaxImageBytes.
bool read_image(const std::string& path, std::string& out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat sb;
  if (::fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size <= 0 || size_t(sb.st_size) > kMaxImageBytes) {
    ::close(fd);
    return false;
  }
  out.resize(size_t(sb.st_size));
  size_t got = 0;
  while (got < out.size()) {
    ssize_t n = ::read(fd, &out[got], out.size() - got);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += size_t(n);
  }
  ::close(fd);
  out.resize(got);
  return got > 0;
}

std::string real_path(const std::string& p) {
  char buf[PATH_MAX];
  return ::realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}

}  // namespace

size_t num_param(const http::Request& req, const char* key, size_t def, size_t max) {
  std::string v;
  if (!http::query_param(req, key, v) || v.empty() || v.size() > 7) return def;
  for (char c : v)
    if (c < '0' || c > '9') return def;
  return std::min(max, size_t(std::strtoul(v.c_str(), nullptr, 10)));
}

namespace {

http::Response games_route(const http::Request& req, const Env& env) {
  std::string sys, q;
  http::query_param(req, "system", sys);
  http::query_param(req, "q", q);
  if (q.size() > 100) q.resize(100);
  const std::string lq = lower(q);
  const size_t offset = num_param(req, "offset", 0, 1000000);
  const size_t limit = std::max<size_t>(1, num_param(req, "limit", 100, 300));
  bool loaded = true, known = sys.empty();
  std::vector<std::string> to_open;  // systems not read yet that this answer covers
  size_t total = 0;
  std::string items;
  {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    for (const SystemInfo& si : s.systems) {
      if (!sys.empty() && si.name != sys) continue;
      known = true;
      if (!si.loaded) {
        loaded = false;
        to_open.push_back(si.name);  // a search over every system names each one not read yet (it asked to open "" before)
      }
      for (const GameInfo& g : si.games) {
        if (!lq.empty() && lower(g.name).find(lq) == std::string::npos) continue;
        if (total >= offset && total < offset + limit) {
          if (!items.empty()) items += ",";
          items += "{\"id\":\"" + game_id(g.path) + "\",\"name\":\"" + oscontract::json_escape(g.name) + "\",\"system\":\"" +
                   oscontract::json_escape(si.name) + "\",\"cover\":" + (g.meta.span_end != 0 ? "true" : "false") + "}";
        }
        ++total;
      }
    }
  }
  if (!known) return err(404, "unknown system");
  if (env.open_system)
    for (const std::string& n : to_open) env.open_system(n);  // the main thread reads them; the page asks again in a moment
  return reply(200, std::string("{\"loaded\":") + (loaded ? "true" : "false") + ",\"total\":" + std::to_string(total) + ",\"offset\":" +
                        std::to_string(offset) + ",\"more\":" + (total > offset + limit ? "true" : "false") + ",\"games\":[" + items + "]}");
}

http::Response launch_route(const http::Request& req, const Env& env) {
  std::string id;
  if (!http::query_param(req, "id", id)) {
    id = req.body;
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
  }
  if (!valid_id(id)) return err(400, "invalid game id");
  if (oscontract::game_running()) return err(409, "a game is running");
  FoundGame f;
  if (!find_game(id, f)) return err(404, "unknown game");
  if (!env.launch) return err(500, "launch is not available");
  {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.m);
    const long long t = now_ms();
    if (t < s.launch_block_until_ms) return err(409, "a launch was just requested");
    s.launch_block_until_ms = t + kLaunchGapMs;
  }
  env.launch(f.path);
  return reply(200, "{\"ok\":true,\"name\":\"" + oscontract::json_escape(f.name) + "\"}");
}

http::Response shots_route(const Env& env) {
  const std::string dir = env.shots_dir.empty() ? std::string(ssmedia::kScreenshotsDir) : env.shots_dir;
  struct Shot {
    std::string name;
    long long mtime;
    long long size;
  };
  std::vector<Shot> v;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d)) {
      const std::string n = e->d_name;
      if (!valid_name(n) || !image_mime(n)) continue;
      struct stat sb;
      if (::lstat((dir + "/" + n).c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
      v.push_back({n, static_cast<long long>(sb.st_mtime), static_cast<long long>(sb.st_size)});
    }
    ::closedir(d);
  }
  std::sort(v.begin(), v.end(), [](const Shot& a, const Shot& b) { return a.mtime != b.mtime ? a.mtime > b.mtime : a.name > b.name; });
  if (v.size() > kMaxShots) v.resize(kMaxShots);
  std::string j = "[";
  for (size_t i = 0; i < v.size(); ++i)
    j += std::string(i ? "," : "") + "{\"id\":\"" + oscontract::json_escape(v[i].name) + "\",\"mtime\":" + std::to_string(v[i].mtime) + ",\"size\":" + std::to_string(v[i].size) + "}";
  return reply(200, j + "]");
}

http::Response shot_route(const http::Request& req, const Env& env) {
  std::string id;
  if (!http::query_param(req, "id", id) || !valid_name(id)) return err(400, "invalid id");
  const char* mime = image_mime(id);
  if (!mime) return err(400, "invalid id");
  const std::string dir = env.shots_dir.empty() ? std::string(ssmedia::kScreenshotsDir) : env.shots_dir;
  std::string body;
  if (!read_image(dir + "/" + id, body)) return err(404, "not found");
  return reply(200, std::move(body), mime);
}

http::Response cover_route(const http::Request& req) {
  std::string id;
  http::query_param(req, "id", id);
  FoundGame f;
  if (!find_game(id, f)) return err(404, "not found");
  GameDetails d = gamelist_details(f.gamelist, f.rom_dir, f.path, f.meta);
  const std::string& pic = !d.thumbnail.empty() ? d.thumbnail : d.image;
  if (pic.empty() || !image_mime(pic)) return err(404, "no picture");
  const std::string rp = real_path(pic), rr = real_path(f.rom_dir);  // symlinks and ".." resolved before the check
  if (rp.empty() || rr.empty() || rp.size() <= rr.size() + 1 || rp.compare(0, rr.size(), rr) != 0 || rp[rr.size()] != '/') return err(404, "no picture");
  std::string body;
  if (!read_image(rp, body)) return err(404, "no picture");
  return reply(200, std::move(body), image_mime(rp));
}

}  // namespace

// ---- routes -----------------------------------------------------------------------------------------------------------

http::Response route(const http::Request& req, const Env& env) {
  const std::string& p = req.path;
  const bool page = p == "/files" || p == "/files/";
  const bool list = p == "/files/systems";
  const bool get_api = p == "/files/games" || p == "/files/shots" || p == "/files/shot" || p == "/files/cover";
  const bool post_api = p == "/files/launch" || p == "/files/quit";
  const bool core = page || list || get_api || post_api;
  if (!enabled()) return err(404, "not found");  // off = as if it did not exist
  if (p == "/" && req.method == "GET") {  // the address the menu shows has no path: send the browser to the page
    http::Response r = reply(302, "", "text/plain");
    r.headers.emplace_back("Location", "/files");
    return r;
  }
  if (!core) return module_route(req, env);  // a module's path (any prefix, e.g. /dav/), else 404
  if (req.method != (post_api ? "POST" : "GET")) return err(405, "method not allowed");
  if (page) {
    http::Response r = reply(200, page_html(), "text/html; charset=utf-8");
    // the page is one file with its own script and style and talks to this console only: nothing else may be loaded into it or frame it
    r.headers.emplace_back("Content-Security-Policy",
                           "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' blob: data:; font-src 'self'; "
                           "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
    r.headers.emplace_back("Referrer-Policy", "no-referrer");
    r.headers.emplace_back("X-Frame-Options", "DENY");
    return r;
  }
  http::Response deny;
  if (!authorize(req, deny)) return deny;
  if (p == "/files/games") return games_route(req, env);
  if (p == "/files/launch") return launch_route(req, env);
  if (p == "/files/quit") return reply(200, oscontract::emukill() ? "{\"killed\":true}" : "{\"killed\":false}");
  if (p == "/files/shots") return shots_route(env);
  if (p == "/files/shot") return shot_route(req, env);
  if (p == "/files/cover") return cover_route(req);
  if (env.refresh) env.refresh();  // the next list is fresher (counts finish after the snapshot)
  return reply(200, systems_json());
}

http::Response upload(const http::Request& req, http::BodyStream& body, const Env& env) {
  if (!enabled()) return err(404, "not found");
  http::Response deny;
  if (!authorize(req, deny)) return deny;

  std::string sysname, name, rep;
  if (!http::query_param(req, "system", sysname) || !http::query_param(req, "name", name)) return err(400, "system and name are required");
  const bool replace = http::query_param(req, "replace", rep) && rep == "1";
  if (!valid_name(name)) return err(400, "invalid file name");
  SystemInfo si;
  if (!find_system(sysname, si) || si.rom_dir.empty()) return err(400, "unknown system");
  if (!ext_allowed(name, si.exts)) return err(400, "this system does not accept that file type");
  const uint64_t len = body.total();
  if (len == 0) return err(400, "empty file");

  struct stat sb;
  if (::stat(si.rom_dir.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) return err(400, "the ROM folder does not exist");
  const std::string final_path = si.rom_dir + "/" + name;
  const std::string part_path = final_path + ".part";
  if (::lstat(final_path.c_str(), &sb) == 0) {
    if (!replace) return err(409, "a file with that name already exists");
    if (!S_ISREG(sb.st_mode)) return err(400, "not a regular file");
  }
  uint64_t avail = 0;
  const bool known = env.free_space ? env.free_space(si.rom_dir, avail) : real_free_space(si.rom_dir, avail);
  if (!known || avail < len) return err(413, "not enough free space");

  ::unlink(part_path.c_str());  // leftover of an interrupted upload (only one runs at a time)
  int fd = ::open(part_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd < 0) return err(500, "cannot create the file");
  body.accept();  // only now does a client waiting for "100 Continue" send the data
  std::vector<char> buf(64 * 1024);
  uint64_t written = 0;
  for (;;) {
    ptrdiff_t n = body.read(buf.data(), buf.size());
    if (n == 0) break;
    if (n < 0) {
      ::close(fd);
      ::unlink(part_path.c_str());
      return err(body.timed_out() ? 408 : 400, "upload interrupted");
    }
    const char* p = buf.data();
    size_t left = size_t(n);
    while (left > 0) {
      ssize_t w = ::write(fd, p, left);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        const bool full = errno == ENOSPC;
        ::close(fd);
        ::unlink(part_path.c_str());
        return err(full ? 413 : 500, full ? "not enough free space" : "write failed");
      }
      p += w;
      left -= size_t(w);
    }
    written += uint64_t(n);
  }
  const bool synced = ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  if (!synced || !closed || written != len) {
    ::unlink(part_path.c_str());
    return err(500, "could not save the file");
  }
  if (!replace && ::lstat(final_path.c_str(), &sb) == 0) {  // appeared while we were receiving
    ::unlink(part_path.c_str());
    return err(409, "a file with that name already exists");
  }
  if (::rename(part_path.c_str(), final_path.c_str()) != 0) {
    ::unlink(part_path.c_str());
    return err(500, "could not save the file");
  }
  if (env.reload) env.reload();
  return reply(200, "{\"ok\":true,\"name\":\"" + oscontract::json_escape(name) + "\",\"bytes\":" + std::to_string(written) + "}");
}

// ---- the page ---------------------------------------------------------------------------------------------------------

namespace {

const char* core_page() {
  return R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Zenith Remote</title>
<meta name="theme-color" content="#26262a">
<style>
@font-face{font-family:"Rubik";src:url("/files/font.ttf") format("truetype");font-display:swap}
:root{color-scheme:light dark;
 --bg:#e4e4e8;--bg2:#f2f2f4;--fg:#2d2d32;--dim:#55555e;--card:rgba(255,255,255,.72);--card2:rgba(255,255,255,.9);--line:rgba(45,45,50,.14);
 --acc:#00708a;--accbg:#00c3e3;--accfg:#04262e;--glow:rgba(0,195,227,.22);--bad:#b3261e;--badbg:#e5484d;--ok:#1a7f37;--shadow:0 10px 30px rgba(20,30,40,.10)}
@media (prefers-color-scheme:dark){:root{
 --bg:#1b1b1f;--bg2:#26262a;--fg:#f0f0f2;--dim:#bebec6;--card:rgba(255,255,255,.045);--card2:rgba(38,38,42,.92);--line:rgba(255,255,255,.09);
 --acc:#4fd8f0;--accbg:#00c3e3;--accfg:#04262e;--glow:rgba(0,195,227,.16);--bad:#ff8a80;--badbg:#d9443f;--ok:#6fdc8c;--shadow:0 12px 34px rgba(0,0,0,.35)}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{background:var(--bg)}
body{margin:0;min-height:100vh;color:var(--fg);font:16px/1.5 "Rubik",system-ui,-apple-system,"Segoe UI",sans-serif;
 background:radial-gradient(120% 70% at 50% -10%,var(--glow),transparent 60%),linear-gradient(180deg,var(--bg2),var(--bg) 60%);background-attachment:fixed}
main{max-width:36rem;margin:0 auto;padding:18px 16px 96px}
h1{display:flex;align-items:center;gap:12px;font-size:1.05rem;font-weight:500;margin:4px 2px 18px;letter-spacing:.02em}
h1::before{content:"";flex:none;width:30px;height:30px;border-radius:50%;border:3px solid var(--accbg);box-shadow:0 0 0 4px var(--glow),inset 0 0 0 4px transparent}
h1 b{font-weight:600;letter-spacing:.14em;text-transform:uppercase}
h1 span{color:var(--dim);font-weight:400}
section{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:18px;margin-bottom:16px;box-shadow:var(--shadow);
 -webkit-backdrop-filter:blur(14px);backdrop-filter:blur(14px)}
label{display:block;margin:.9rem 0 .35rem;font-size:.82rem;font-weight:500;color:var(--dim);letter-spacing:.04em;text-transform:uppercase}
section>label:first-child{margin-top:0}
label:has(input[type=checkbox]){display:flex;align-items:center;text-transform:none;letter-spacing:0;font-size:.95rem;color:var(--fg);margin-top:1rem}
input,select,button{font:inherit;color:inherit;width:100%;padding:.7rem .85rem;border-radius:12px;border:1px solid var(--line);background:var(--card2);outline:none;
 transition:border-color .15s,box-shadow .15s,background .15s,transform .08s}
input:focus,select:focus{border-color:var(--accbg);box-shadow:0 0 0 3px var(--glow)}
input[type=checkbox]{width:20px;height:20px;margin:0 .6rem 0 0;accent-color:var(--accbg);flex:none}
input[type=file]{padding:.45rem;font-size:.9rem;color:var(--dim)}
input[type=file]::file-selector-button{font:inherit;font-weight:500;color:var(--fg);background:transparent;border:1px solid var(--line);border-radius:9px;padding:.45rem .8rem;margin-right:.7rem;cursor:pointer}
button{background:var(--accbg);color:var(--accfg);border:0;margin-top:.9rem;font-weight:600;letter-spacing:.02em;cursor:pointer;box-shadow:0 6px 18px var(--glow)}
button:active{transform:scale(.985)}
button:focus-visible{box-shadow:0 0 0 3px var(--glow),0 0 0 1px var(--accbg)}
button:disabled{opacity:.45;cursor:default;box-shadow:none}
button.alt{background:transparent;color:var(--fg);border:1px solid var(--line);box-shadow:none}
button.alt:hover{border-color:var(--accbg)}
button.bad{background:transparent;color:var(--bad);border:1px solid color-mix(in srgb,var(--bad) 45%,transparent);box-shadow:none}
#pinbox{text-align:center;padding:28px 20px}
#pinbox label{text-align:center}
#pin{letter-spacing:.6em;text-indent:.6em;text-align:center;font-size:2rem;font-weight:500;max-width:15rem;margin:0 auto;display:block;padding:.6rem}
#pinbox button{max-width:15rem}
#pairbox{border-color:color-mix(in srgb,var(--accbg) 55%,transparent)}
#pairbox p{margin:0 0 .2rem}
#info,#drop,#ovr,#gnote,#snote{font-size:.88rem;color:var(--dim);margin:.6rem 0 0;word-break:break-word}
#drop{border:1.5px dashed var(--line);border-radius:12px;padding:.9rem;text-align:center;margin-top:.9rem}
#p-up.drag #drop{border-color:var(--accbg);color:var(--acc);background:var(--glow)}
#msg{position:fixed;left:50%;bottom:22px;transform:translateX(-50%);max-width:min(32rem,calc(100% - 32px));z-index:9;margin:0;
 padding:.7rem 1.1rem;border-radius:14px;background:var(--card2);border:1px solid var(--line);box-shadow:var(--shadow);font-weight:500;font-size:.95rem;
 -webkit-backdrop-filter:blur(14px);backdrop-filter:blur(14px);animation:zin .2s ease-out}
#msg:empty{display:none}
#msg.bad{color:var(--bad);border-color:color-mix(in srgb,var(--bad) 45%,transparent)}
@keyframes zin{from{opacity:0;transform:translate(-50%,8px)}to{opacity:1;transform:translate(-50%,0)}}
ul{list-style:none;padding:0;margin:.9rem 0 0}
li{padding:.6rem 0;border-top:1px solid var(--line);word-break:break-word;font-size:.95rem}
li:first-child{border-top:0}
li.ok{color:var(--ok)}li.bad{color:var(--bad)}
#tabs{position:sticky;top:8px;z-index:4;display:flex;gap:4px;margin:0 0 16px;padding:5px;border-radius:16px;background:var(--card2);border:1px solid var(--line);
 box-shadow:var(--shadow);-webkit-backdrop-filter:blur(14px);backdrop-filter:blur(14px);overflow-x:auto;scrollbar-width:none}
#tabs::-webkit-scrollbar{display:none}
#tabs button{flex:1 1 0;width:auto;min-width:58px;margin:0;padding:.45rem .35rem .4rem;display:flex;flex-direction:column;align-items:center;gap:3px;
 background:transparent;color:var(--dim);border:1.5px solid transparent;border-radius:12px;font-size:.72rem;font-weight:500;box-shadow:none}
#tabs button::before{content:"";width:22px;height:22px;background:currentColor;-webkit-mask-position:center;mask-position:center;-webkit-mask-size:contain;mask-size:contain;-webkit-mask-repeat:no-repeat;mask-repeat:no-repeat}
#tabs button.on{color:var(--acc);border-color:var(--accbg);background:var(--glow)}
#t-up::before{-webkit-mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M12 16V4'/%3E%3Cpath d='M7 9l5-5 5 5'/%3E%3Cpath d='M4 16v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3'/%3E%3C/svg%3E");mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M12 16V4'/%3E%3Cpath d='M7 9l5-5 5 5'/%3E%3Cpath d='M4 16v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3'/%3E%3C/svg%3E")}
#t-ga::before{-webkit-mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Crect x='2.5' y='7' width='19' height='11' rx='5.5'/%3E%3Cpath d='M7.5 10.5v4M5.5 12.5h4'/%3E%3Ccircle cx='15.5' cy='11.5' r='.6'/%3E%3Ccircle cx='17.5' cy='13.5' r='.6'/%3E%3C/svg%3E");mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Crect x='2.5' y='7' width='19' height='11' rx='5.5'/%3E%3Cpath d='M7.5 10.5v4M5.5 12.5h4'/%3E%3Ccircle cx='15.5' cy='11.5' r='.6'/%3E%3Ccircle cx='17.5' cy='13.5' r='.6'/%3E%3C/svg%3E")}
#t-sh::before{-webkit-mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Crect x='3' y='5' width='18' height='14' rx='3'/%3E%3Ccircle cx='9' cy='10' r='1.6'/%3E%3Cpath d='M21 16l-5-5-7 8'/%3E%3C/svg%3E");mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Crect x='3' y='5' width='18' height='14' rx='3'/%3E%3Ccircle cx='9' cy='10' r='1.6'/%3E%3Cpath d='M21 16l-5-5-7 8'/%3E%3C/svg%3E")}
#t-bi::before{-webkit-mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Crect x='6' y='6' width='12' height='12' rx='2'/%3E%3Cpath d='M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4'/%3E%3C/svg%3E");mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Crect x='6' y='6' width='12' height='12' rx='2'/%3E%3Cpath d='M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4'/%3E%3C/svg%3E")}
#t-sv::before{-webkit-mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M5 3h11l3 3v13a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2z'/%3E%3Cpath d='M8 3v5h7V3'/%3E%3Crect x='8' y='13' width='8' height='5' rx='1'/%3E%3C/svg%3E");mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M5 3h11l3 3v13a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2z'/%3E%3Cpath d='M8 3v5h7V3'/%3E%3Crect x='8' y='13' width='8' height='5' rx='1'/%3E%3C/svg%3E")}
#glist li{display:flex;gap:12px;align-items:center;cursor:pointer;min-height:64px}
#glist button,#tabs button{box-shadow:none}
#glist .cv{flex:none;width:52px;height:52px;border-radius:10px;background:var(--card2);border:1px solid var(--line);object-fit:cover}
#glist .nm{flex:1;min-width:0;font-weight:500}
#glist .sy{display:block;font-size:.78rem;color:var(--dim);font-weight:400;text-transform:uppercase;letter-spacing:.06em}
#grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:10px;margin-top:.9rem}
#grid .sh{width:100%;aspect-ratio:4/3;object-fit:cover;border-radius:12px;border:1px solid var(--line);background:var(--card2);cursor:pointer;transition:transform .15s}
#grid .sh:hover{transform:scale(1.02)}
#big{position:fixed;inset:0;background:rgba(0,0,0,.9);display:flex;align-items:center;justify-content:center;z-index:8;padding:12px}
#big img{max-width:100%;max-height:100%;border-radius:12px}
[hidden]{display:none!important}
@media (min-width:640px){main{padding-top:28px}#tabs button{font-size:.78rem}}
@media (prefers-reduced-motion:reduce){*{transition:none!important;animation:none!important}}
</style></head><body><main>
<h1><b>Zenith</b><span>Remote</span></h1>
<section id="pinbox">
<label for="pin">PIN shown on the console</label>
<input id="pin" inputmode="numeric" pattern="[0-9]*" maxlength="4" autocomplete="off">
<button id="go" type="button">Continue</button>
</section>
<div id="work" hidden>
<section id="pairbox" hidden>
<p>Remember this device? It will not need the PIN next time.</p>
<button id="pairgo" type="button">Remember this device</button>
<button id="pairno" type="button" class="alt">Not now</button>
</section>
<div id="tabs">
<button type="button" id="t-up" class="on">Upload</button>
<button type="button" id="t-ga">Games</button>
<button type="button" id="t-sh">Screenshots</button>
</div>
<section id="p-up">
<label for="sys">System</label>
<select id="sys"></select>
<p id="info"></p>
<label for="files">Files</label>
<input id="files" type="file" multiple>
<label for="dir">Folder</label>
<input id="dir" type="file" webkitdirectory>
<p id="drop">Or drop files or a folder here.</p>
<label><input id="rep" type="checkbox">Replace files that already exist</label>
<button id="send" type="button">Upload</button>
<p id="ovr"></p>
<ul id="log"></ul>
</section>
<section id="p-ga" hidden>
<label for="gq">Search</label>
<input id="gq" type="search" autocomplete="off">
<label for="gsys">System</label>
<select id="gsys"></select>
<ul id="glist"></ul>
<p id="gnote"></p>
<button id="gmore" type="button" class="alt" hidden>More</button>
<button id="quit" type="button" class="bad">Close game</button>
</section>
<section id="p-sh" hidden>
<button id="shre" type="button" class="alt">Refresh</button>
<div id="grid"></div>
<p id="snote"></p>
</section>
</div>
<p id="msg" role="status"></p>
<div id="big" hidden><img id="bigimg" alt=""></div>
<script>
(function(){
var pin="",token="",systems=[],busy=false,gen=0,rq=0,gtimer=0,goff=0,urls=[],queue=[],active=0,io=null;
function $(i){return document.getElementById(i)}
function say(t,bad){var m=$("msg");clearTimeout(say.t);m.textContent=t;m.className=bad?"bad":"";if(t&&!/(\.\.\.|…)$/.test(t))say.t=setTimeout(function(){m.textContent=""},bad?9000:3500)}
function why(x){var e="";try{e=JSON.parse(x.responseText).error}catch(_){}
 if(x.status===403)return x.zfTok?"This device is no longer paired. Enter the PIN again.":"Wrong PIN.";
 if(x.status===429)return"Too many wrong PINs. Wait 30 seconds.";
 if(x.status===404&&!e)return"File transfer is switched off on the console.";
 return e||("Error "+x.status)}
function auth(x){if(token){x.zfTok=1;x.setRequestHeader("X-Token",token);
 x.addEventListener("load",function(){if(x.status===403&&token)relogin()})}else x.setRequestHeader("X-PIN",pin)}
function call(m,path,done,blob){var x=new XMLHttpRequest();x.open(m,path);auth(x);
 if(blob)x.responseType="blob";
 x.onload=function(){done(x)};x.onerror=function(){say("Cannot reach the console.",true);done(null)};x.send();return x}
function jerr(x){if(!x)return true;if(x.status!==200){say(why(x),true);return true}return false}
function fill(){var s=$("sys"),g=$("gsys"),keep=s.value,gk=g.value;s.textContent="";g.textContent="";
 var o=document.createElement("option");o.value="";o.textContent="All loaded systems";g.appendChild(o);
 systems.forEach(function(y){var t=y.fullname||y.name;
  o=document.createElement("option");o.value=y.name;o.textContent=t;s.appendChild(o);
  o=document.createElement("option");o.value=y.name;o.textContent=t;g.appendChild(o)});
 if(keep)s.value=keep;if(gk)g.value=gk;show()}
function show(){var y=null,v=$("sys").value;systems.forEach(function(z){if(z.name===v)y=z});
 $("info").textContent=y?("Folder: "+y.dir+" - accepts: "+y.exts.join(" ")+(y.count==null?"":" - "+y.count+" games")):""}
function load(done){var x=new XMLHttpRequest();x.open("GET","/files/systems");auth(x);
 x.onload=function(){if(x.status===200){try{systems=JSON.parse(x.responseText)}catch(_){systems=[]}done(true)}else{say(why(x),true);done(false)}};
 x.onerror=function(){say("Cannot reach the console.",true);done(false)};x.send()}
var tabs=["up","ga","sh"/*@TABS@*/],ontab={};
function tab(n){tabs.forEach(function(k){$("p-"+k).hidden=k!==n;$("t-"+k).className=k===n?"on":""});
 if(n==="ga"&&!$("glist").children.length)games(true);
 if(n==="sh")shots();
 if(ontab[n])ontab[n]()}
tabs.forEach(function(k){$("t-"+k).onclick=function(){tab(k)}});
/* a paired phone keeps a token (never the PIN) and sends it instead; the console forgetting it brings the PIN box back */
function keep(t){try{localStorage.setItem("zf_token",t);return true}catch(_){return false}}
function stored(){try{return localStorage.getItem("zf_token")||""}catch(_){return""}}
function canKeep(){try{localStorage.setItem("zf_t","1");localStorage.removeItem("zf_t");return true}catch(_){return false}}
function relogin(){token="";pin="";try{localStorage.removeItem("zf_token")}catch(_){}
 $("work").hidden=true;$("pairbox").hidden=true;$("pinbox").hidden=false;$("pin").value="";
 say("This device is no longer paired. Enter the PIN again.",true)}
/* offer: 1 = after typing the PIN (a button offers to remember the device), 2 = opened from the console's QR code (the console
   is asked at once: scanning the code is the request) */
function enter(offer){load(function(ok){if(!ok)return;$("pinbox").hidden=true;$("work").hidden=false;fill();
 if(offer&&!token){$("pairbox").hidden=!canKeep();if(offer===2&&canKeep())$("pairgo").onclick()}})}
function devname(){var u=navigator.userAgent||"";
 if(/iPhone/.test(u))return"iPhone";if(/iPad/.test(u))return"iPad";
 if(/Android/.test(u))return/Mobile/.test(u)?"Android phone":"Android tablet";
 if(/Windows/.test(u))return"Windows PC";if(/CrOS/.test(u))return"Chromebook";
 if(/Macintosh|Mac OS X/.test(u))return"Mac";if(/Linux/.test(u))return"Linux PC";return"Browser"}
$("go").onclick=function(){pin=$("pin").value.replace(/\D/g,"");say("");enter(1)};
$("pin").onkeydown=function(e){if(e.key==="Enter"){e.preventDefault();$("go").onclick()}};
$("pairno").onclick=function(){$("pairbox").hidden=true};
$("pairgo").onclick=function(){var b=$("pairgo"),x=new XMLHttpRequest();b.disabled=true;say("Answer on the console...");
 x.open("POST","/files/pair");x.setRequestHeader("X-PIN",pin);
 x.onload=function(){b.disabled=false;var t="";if(x.status===200){try{t=JSON.parse(x.responseText).token}catch(_){}}
  if(t){token=t;$("pairbox").hidden=true;if(keep(t))say("This device is remembered.");else say("This browser cannot keep it. It works until you close the page.",true)}
  else if(x.status===403&&x.responseText.indexOf("not allowed")>=0)say("The console did not allow it.",true)
  else if(x.status===409)say("The console is asking about another device. Try again in a moment.",true)
  else say(why(x),true)};
 x.onerror=function(){b.disabled=false;say("Cannot reach the console.",true)};
 x.send(devname())};
$("sys").onchange=show;
/* uploads: one file at a time, in 8 MB slices to /files/put; after a cut connection the console is asked how much it has */
var CH=8*1024*1024,drops=[];
function enc(s){return encodeURIComponent(s)}
function putfile(it,rep,last,li,prog,done){
 var f=it.f,off=0,tries=0;
 function label(t){li.textContent=it.p+" - "+t}
 function setoff(n){prog(n-off);off=n}
 function fail(t){li.className="bad";label(t);done(false)}
 function finish(){setoff(f.size);li.className="ok";label("done");done(true)}
 function retry(){
  if(++tries>30){fail("connection lost");return}
  label("Reconnecting…");
  setTimeout(function(){var x=new XMLHttpRequest();
   x.open("GET","/files/put/status?system="+enc(it.s)+"&path="+enc(it.p));auth(x);x.timeout=15000;
   x.onload=function(){if(x.status!==200){fail(why(x));return}
    var r={};try{r=JSON.parse(x.responseText)}catch(_){}
    if(r.done&&!r.have&&r.size===f.size){finish();return}
    setoff(r.have||0);step()};
   x.onerror=x.ontimeout=retry;x.send()},2000)}
 function step(){
  var end=Math.min(off+CH,f.size),x=new XMLHttpRequest();
  x.open("POST","/files/put?system="+enc(it.s)+"&path="+enc(it.p)+"&offset="+off+"&total="+f.size+(rep?"&replace=1":"")+(last&&end>=f.size?"&final=1":""));
  auth(x);x.timeout=180000;
  x.upload.onprogress=function(e){if(e.lengthComputable)label(Math.min(100,Math.floor(100*(off+e.loaded)/Math.max(f.size,1)))+"%")};
  x.onload=function(){
   if(x.status===200){setoff(end);if(end>=f.size)finish();else step();return}
   var r={};try{r=JSON.parse(x.responseText)}catch(_){}
   if(x.status===409&&typeof r.have==="number"){if(++tries>30){fail("out of step with the console");return}setoff(r.have);step();return}
   if(x.status===409&&r.busy){retry();return}
   fail(why(x))};
  x.onerror=x.ontimeout=retry;
  x.send(f.slice(off,end))}
 label("0%");step()}
function gather(){var out=[];
 [$("files").files,$("dir").files].forEach(function(l){Array.prototype.forEach.call(l,function(f){out.push({f:f,p:f.webkitRelativePath||f.name})})});
 return out.concat(drops)}
$("send").onclick=function(){if(busy)return;var list=gather();
 if(!list.length){say("Choose at least one file or a folder.",true);return}
 say("");busy=true;$("send").disabled=true;$("log").textContent="";drops=[];$("drop").textContent="Or drop files or a folder here.";
 var all=0,did=0,sent=0,lastok=true,rep=$("rep").checked,log=$("log"),sysn=$("sys").value;
 list.forEach(function(it){all+=it.f.size;it.s=sysn});
 function show(i){$("ovr").textContent="Overall "+Math.floor(100*did/Math.max(all,1))+"% - file "+Math.min(i+1,list.length)+" of "+list.length}
 function next(i){
  if(i>=list.length){busy=false;$("send").disabled=false;$("ovr").textContent="Finished: "+sent+" of "+list.length+" files sent.";
   if(sent&&!lastok){var q=new XMLHttpRequest();q.open("POST","/files/put/rescan");auth(q);q.send()}
   load(function(ok){if(ok)fill()});return}
  var li=document.createElement("li");log.appendChild(li);show(i);
  putfile(list[i],rep,i===list.length-1,li,function(d){did+=d;show(i)},function(ok){if(ok)sent++;lastok=ok;next(i+1)})}
 next(0)};
/* dropping files (or folders, where the browser lists their entries) on the upload box */
function walk(list,done){var n=list.length;if(!n){done();return}
 function one(){if(--n===0)done()}
 list.forEach(function(en){
  if(en.isFile){en.file(function(f){drops.push({f:f,p:en.fullPath.replace(/^\//,"")});one()},one)}
  else if(en.isDirectory){var rd=en.createReader(),acc=[];
   (function more(){rd.readEntries(function(r){if(r.length){acc=acc.concat(Array.prototype.slice.call(r));more()}else walk(acc,one)},one)})()}
  else one()})}
var box=$("p-up");
box.ondragover=function(e){e.preventDefault();box.className="drag"};
box.ondragleave=function(){box.className=""};
box.ondrop=function(e){e.preventDefault();box.className="";if(busy)return;
 var dt=e.dataTransfer,ents=[],i,en;
 function note(){$("drop").textContent=drops.length+" dropped file(s) ready. Press Upload."}
 if(dt.items&&dt.items.length&&dt.items[0].webkitGetAsEntry){
  for(i=0;i<dt.items.length;i++){en=dt.items[i].webkitGetAsEntry();if(en)ents.push(en)}
  walk(ents,note)}
 else{Array.prototype.forEach.call(dt.files,function(f){drops.push({f:f,p:f.name})});note()}};

/* pictures need the PIN header, so they are fetched and shown from blobs, two at a time */
function pump(){while(active<2&&queue.length){var j=queue.shift();active++;(function(j){
 if(j.g!==gen){active--;return}
 call("GET",j.path,function(x){active--;
  if(x&&x.status===200&&j.g===gen){var u=URL.createObjectURL(x.response);urls.push(u);j.img.setAttribute("src",u)}
  pump()},true)})(j)}}
function pic(img,path){var j={img:img,path:path,g:gen};
 if(io){img._j=j;io.observe(img)}else{queue.push(j);pump()}}
function resetpics(){gen++;queue=[];urls.forEach(function(u){URL.revokeObjectURL(u)});urls=[];
 if(io)io.disconnect();
 io=window.IntersectionObserver?new IntersectionObserver(function(es){es.forEach(function(e){
  if(e.isIntersecting){io.unobserve(e.target);queue.push(e.target._j);pump()}})},{rootMargin:"200px"}):null}

function games(fresh,tries){
 if(fresh){goff=0;$("glist").textContent="";resetpics()}
 var my=++rq;
 var q="/files/games?system="+encodeURIComponent($("gsys").value)+"&q="+encodeURIComponent($("gq").value)+"&offset="+goff;
 $("gnote").textContent="Loading...";
 call("GET",q,function(x){
  if(my!==rq)return;
  if(jerr(x)){$("gnote").textContent="";return}
  var r={};try{r=JSON.parse(x.responseText)}catch(_){}
  if(r.loaded===false&&(tries||0)<25){setTimeout(function(){if(my===rq)games(false,(tries||0)+1)},1000);return}
  var l=$("glist");
  (r.games||[]).forEach(function(g){var li=document.createElement("li"),im=document.createElement("img"),sp=document.createElement("span"),sy=document.createElement("small");
   im.className="cv";im.alt="";sp.className="nm";sp.textContent=g.name;sy.className="sy";sy.textContent=g.system;sp.appendChild(sy);
   li.appendChild(im);li.appendChild(sp);l.appendChild(li);
   if(Z.onrow)Z.onrow(g,li);
   if(g.cover)pic(im,"/files/cover?id="+g.id);
   li.onclick=function(){if(window.confirm("Launch "+g.name+"?"))launch(g)}});
  goff=(r.offset||0)+(r.games||[]).length;
  $("gmore").hidden=!r.more;
  $("gnote").textContent=r.total?"":(r.loaded===false?"This system could not be read yet.":"No games found.")
 })}
function launch(g){say("");call("POST","/files/launch?id="+g.id,function(x){
 if(!x)return;if(x.status===200)say("Launching "+g.name+"...");else say(why(x),true)})}
$("gmore").onclick=function(){games(false)};
$("gsys").onchange=function(){games(true)};
$("gq").oninput=function(){clearTimeout(gtimer);gtimer=setTimeout(function(){games(true)},350)};
$("quit").onclick=function(){if(!window.confirm("Close the running game?"))return;
 call("POST","/files/quit",function(x){if(!x)return;if(x.status!==200){say(why(x),true);return}
  var k=false;try{k=JSON.parse(x.responseText).killed}catch(_){}
  say(k?"Closing the game...":"No game is running.",!k)})};

function shots(){var g=$("grid");g.textContent="";resetpics();$("snote").textContent="Loading...";
 var my=++rq;
 call("GET","/files/shots",function(x){if(my!==rq)return;
  if(jerr(x)){$("snote").textContent="";return}
  var r=[];try{r=JSON.parse(x.responseText)}catch(_){}
  $("snote").textContent=r.length?"":"No screenshots.";
  r.forEach(function(s){var im=document.createElement("img");im.className="sh";im.alt=s.id;g.appendChild(im);
   var p="/files/shot?id="+encodeURIComponent(s.id);pic(im,p);
   im.onclick=function(){var u=im.getAttribute("src");if(u){$("bigimg").setAttribute("src",u);$("big").hidden=false}}})})}
$("shre").onclick=shots;
$("big").onclick=function(){$("big").hidden=true};
var Z={$:$,say:say,why:why,call:call,jerr:jerr,load:load,fill:fill,systems:function(){return systems},pic:pic,resetpics:resetpics,
 tab:tab,ontab:ontab,auth:auth,nextReq:function(){return ++rq},isReq:function(n){return n===rq},games:function(){games(true)},onrow:null};
/*@MODULES@*/
/* start: the PIN of the QR code leaves the address at once; a phone already paired keeps its token (scanning again must not pair
   it twice), and only when the console no longer knows that token does the PIN of the QR code pair it again */
(function(){var h=/[#&]pin=(\d{4,6})(?:&|$)/.exec(location.hash),qp=h?h[1]:"";token=stored();
 if(h)try{history.replaceState(null,"",location.pathname+location.search)}catch(_){}
 if(token)load(function(ok){if(ok){$("pinbox").hidden=true;$("work").hidden=false;fill();return}
  if(!token&&qp){pin=qp;say("");enter(2)}});
 else if(qp){pin=qp;enter(2)}})();
})();
</script></main></body></html>
)HTML";
}

std::string esc_html(const std::string& t) {
  std::string o;
  for (char c : t) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

void put_before(std::string& page, const char* mark, const std::string& what) {
  size_t at = page.find(mark);
  if (at != std::string::npos) page.insert(at, what);
}

// The core page with each module's style, tab, section, overlay and script put in their places.
std::string build_page() {
  std::string page = core_page();
  std::string css, tabs_btn, sections, html, tab_ids, js;
  for (const WebModule& m : mods()) {
    css += m.css;
    html += m.html;
    if (!m.tab_id.empty()) {
      tabs_btn += "<button type=\"button\" id=\"t-" + m.tab_id + "\">" + esc_html(m.tab_label) + "</button>\n";
      sections += "<section id=\"p-" + m.tab_id + "\" hidden>\n" + m.section + "\n</section>\n";
      tab_ids += ",\"" + m.tab_id + "\"";
    }
    if (!m.js.empty()) js += "(function(Z){\n" + m.js + "\n})(Z);\n";
  }
  put_before(page, "</style>", css);
  put_before(page, "</div>\n<section id=\"p-up\">", tabs_btn);
  put_before(page, "</div>\n<p id=\"msg\"", sections);
  put_before(page, "<p id=\"msg\"", html);
  const std::string tmark = "/*@TABS@*/", jmark = "/*@MODULES@*/";
  page.replace(page.find(tmark), tmark.size(), tab_ids);
  page.replace(page.find(jmark), jmark.size(), js);
  return page;
}

}  // namespace

const char* page_html() {
  static const std::string page = build_page();  // modules register before main(): built once, on first use
  return page.c_str();
}

}  // namespace webtransfer
}  // namespace zf
