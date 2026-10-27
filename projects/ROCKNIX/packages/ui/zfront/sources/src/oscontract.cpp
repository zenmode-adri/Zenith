#include "oscontract.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#ifdef ZF_WITH_SDL
#include <SDL.h>
#endif

#include "launch.hpp"
#include "screensaver.hpp"

namespace zf {
namespace oscontract {

namespace {

struct State {
  std::shared_ptr<Services> sv;
  bool dry = false;
  std::vector<std::string> dry_log;
  Mode mode = Mode::Quit;
  std::atomic<bool> quit{false};
  IdleTimer idle;
  screensaver::ResumeDetector resume;  // a suspend (power button) while the screensaver was on
  bool redraw = false;                 // the screensaver started / stopped: the loop must draw
  std::vector<std::string> dirs;
  bool dirs_set = false;
  std::map<std::string, std::pair<TimePoint, bool>> hook_cache;  // `*-selected` events: when checked, has hooks
  std::string pending_launch;
  bool ready_done = false;
  bool wmclass_set = false;
  std::vector<std::function<void()>> finish_hooks;
  // read by the HTTP thread while the main thread is inside a game
  std::mutex gm;
  bool game_on = false;
  std::string game_path, game_name;
  std::atomic<int> busy{0};
};

State& st() {
  static State s;
  return s;
}

Paths& default_paths() {
  static Paths p;
  return p;
}

volatile sig_atomic_t g_hup = 0;
volatile sig_atomic_t g_forward_term = 0;

void on_sighup(int) {
  g_hup = 1;
  if (g_forward_term) ::kill(::getpid(), SIGTERM);  // SDL turns SIGTERM into SDL_QUIT; before/after SDL it just ends us
}

CmdRunner& runner() {
  static CmdRunner fallback = [] {
    CmdRunner r;
    r.dry_run = true;  // nothing bound (harness): never run a system command by accident
    return r;
  }();
  return st().sv ? st().sv->cmd : fallback;
}

// True (and recorded) when the effect must be skipped.
bool dry_note(const std::string& what) {
  if (!dry_run()) return false;
  st().dry_log.push_back(what);
  return true;
}

bool exists_kind(const std::string& path, bool want_socket) {
  struct stat sb;
  if (::stat(path.c_str(), &sb) != 0) return false;
  return !want_socket || S_ISSOCK(sb.st_mode);
}

int ms_until(TimePoint end) {
  auto d = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
  return d < 0 ? 0 : int(d);
}

bool touch_file(const std::string& path) {
  if (dry_note("touch " + path)) return true;
  int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  ::close(fd);
  return true;
}

bool safe_process_name(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (char c : n) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '+';
    if (!ok) return false;
  }
  return true;
}

std::string exe_basename() {
  char buf[512];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return {};
  buf[n] = 0;
  return path_filename(buf);
}

}  // namespace

Paths& paths() { return default_paths(); }
bool dry_run() { return st().dry; }
std::vector<std::string>& dry_log() { return st().dry_log; }

// ---- lifecycle ------------------------------------------------------------------------------------------------------

void install_signals(bool forward_term) {
  g_forward_term = forward_term ? 1 : 0;
  struct sigaction sa;
  sa.sa_handler = on_sighup;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // no SA_RESTART: the loop's poll(2) must return
  ::sigaction(SIGHUP, &sa, nullptr);
}

void startup(std::shared_ptr<Services> sv, bool dry_run_flag) {
  State& s = st();
  s.sv = std::move(sv);
  s.dry = dry_run_flag;
  install_signals(true);
  backlight(0);
  set_kill("emulationstation");
  s.idle.input(Clock::now());
  s.resume.check(screensaver::clock_offset_ns());
}

void on_finish(std::function<void()> hook) { st().finish_hooks.push_back(std::move(hook)); }

void finish() {
  State& s = st();
  for (auto& h : s.finish_hooks) h();
  s.finish_hooks.clear();
  clear_ready();
  backlight(0);
  switch (s.mode) {
    case Mode::Quit:
      fire_event("quit");
      break;
    case Mode::Restart:
      touch_file(path_join(paths().tmp_dir, "restart.please"));
      break;
    case Mode::Reboot:
    case Mode::FastReboot:
      touch_file(path_join(paths().tmp_dir, "reboot.please"));
      runner().run("shutdown -r now", 30000);
      break;
    case Mode::Shutdown:
    case Mode::FastShutdown:
      touch_file(path_join(paths().tmp_dir, "shutdown.please"));
      runner().run("shutdown -P -h now", 30000);
      break;
  }
}

bool wait_for_path(const std::string& path, int timeout_ms, bool want_socket) {
  if (exists_kind(path, want_socket)) return true;
  if (timeout_ms <= 0) return false;
  TimePoint end = Clock::now() + std::chrono::milliseconds(timeout_ms);
  std::string dir = path_dir(path);
  int ifd = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  bool watching = ifd >= 0 && !dir.empty() && ::inotify_add_watch(ifd, dir.c_str(), IN_CREATE | IN_MOVED_TO | IN_ATTRIB) >= 0;
  bool ok = false;
  for (;;) {
    if (exists_kind(path, want_socket)) {  // also right after adding the watch: nothing can slip in between
      ok = true;
      break;
    }
    int left = ms_until(end);
    if (left <= 0) break;
    if (watching) {
      pollfd pf{ifd, POLLIN, 0};
      int r = ::poll(&pf, 1, std::min(left, 250));  // the cap is only a safety net: events wake us at once
      if (r > 0) {
        char buf[1024];
        while (::read(ifd, buf, sizeof buf) > 0) {
        }
      }
    } else {
      ::poll(nullptr, 0, std::min(left, 20));
    }
  }
  if (ifd >= 0) ::close(ifd);
  return ok;
}

namespace {
bool wait_env(const char* var, int timeout_ms, bool want_socket) {
  const char* p = std::getenv(var);
  if (!p || !*p) return true;
  TimePoint t0 = Clock::now();
  bool ok = wait_for_path(p, timeout_ms, want_socket);
  long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
  if (!ok) std::fprintf(stderr, "zfront: gave up waiting %ld ms for %s (%s)\n", ms, p, var);
  else if (ms > 0) std::fprintf(stderr, "zfront: waited %ld ms for %s\n", ms, p);
  return ok;
}
}  // namespace

bool wait_display() { return wait_env("ES_WAIT_WAYLAND_SOCKET", 10000, true); }
bool wait_roms() { return wait_env("ES_WAIT_ROMS_READY", 15000, false); }

void on_first_frame() {
  State& s = st();
  if (s.ready_done) return;
  s.ready_done = true;
  if (!dry_note("create " + paths().ready_file)) {
    int fd = ::open(paths().ready_file.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    if (fd >= 0) ::close(fd);
  }
  fire_event("start");
}

void clear_ready() {
  if (dry_note("remove " + paths().ready_file)) return;
  ::unlink(paths().ready_file.c_str());
}

void before_video() {
  State& s = st();
  if (exe_basename() == "emulationstation" || std::getenv("SDL_VIDEO_WAYLAND_WMCLASS")) return;
  ::setenv("SDL_VIDEO_WAYLAND_WMCLASS", "emulationstation", 0);
  s.wmclass_set = true;
}

// ---- system facts ---------------------------------------------------------------------------------------------------

int backlight(int power) {
  if (dry_note("bl_power " + std::to_string(power))) return 0;
  int n = 0;
  DIR* d = ::opendir(paths().backlight_dir.c_str());
  if (!d) return 0;
  std::string val = std::to_string(power) + "\n";
  while (const dirent* e = ::readdir(d)) {
    if (e->d_name[0] == '.') continue;
    std::string f = path_join(path_join(paths().backlight_dir, e->d_name), "bl_power");
    int fd = ::open(f.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) continue;
    if (::write(fd, val.data(), val.size()) == ssize_t(val.size())) ++n;
    ::close(fd);
  }
  ::closedir(d);
  return n;
}

bool set_kill(const std::string& name) {
  if (dry_note("set_kill " + name)) return true;
  return write_atomic(paths().kill_file, name + "\n");
}

std::string kill_target() {
  std::string t;
  if (!read_file(paths().kill_file, t)) return {};
  return std::string(trim(t));
}

// ---- quit / suspend -------------------------------------------------------------------------------------------------

void request_quit() {
  st().quit = true;
#ifdef ZF_WITH_SDL
  SDL_Event ev;
  SDL_zero(ev);
  ev.type = SDL_QUIT;
  SDL_PushEvent(&ev);  // wakes the loop; harmless when SDL is not running
#endif
}

bool quit_requested() { return st().quit.load() || g_hup; }
Mode quit_mode() { return st().mode; }

void quit(Mode m) {
  st().mode = m;
  switch (m) {
    case Mode::Reboot:
    case Mode::FastReboot:
      fire_event("quit", {"reboot"});
      fire_event("reboot");
      break;
    case Mode::Shutdown:
    case Mode::FastShutdown:
      fire_event("quit", {"shutdown"});
      fire_event("shutdown");
      break;
    default:
      break;
  }
  request_quit();
}

bool suspend() {
  // suspendmode=off means no hardware suspend (AllowSuspend=no): the power key then goes through rocknix-fake-suspend, so do the same
  runner().spawn(". /etc/profile; if [ \"$(get_setting system.suspendmode)\" = off ]; then rocknix-fake-suspend power; else systemctl suspend; fi");
  return true;
}

bool suspend_supported() { return file_exists(paths().suspend_helper); }

// ---- events ---------------------------------------------------------------------------------------------------------

void set_script_dirs(std::vector<std::string> dirs) {
  st().dirs = std::move(dirs);
  st().dirs_set = true;
  st().hook_cache.clear();
}

const std::vector<std::string>& script_dirs() {
  State& s = st();
  if (!s.dirs_set) {
    s.dirs = launch::default_script_dirs();
    s.dirs_set = true;
  }
  return s.dirs;
}

namespace {
// `*-selected` events fire on every cursor stop: whether a script listens is cached for 5 s so that nobody-listens costs a map lookup.
bool selected_hooks(const std::string& event) {
  State& s = st();
  TimePoint now = Clock::now();
  auto it = s.hook_cache.find(event);
  if (it == s.hook_cache.end() || now - it->second.first > std::chrono::seconds(5)) {
    s.hook_cache[event] = {now, launch::has_hooks(script_dirs(), event)};
    it = s.hook_cache.find(event);
  }
  return it->second.second;
}
}  // namespace

bool selected_listening(const char* event) {
  State& s = st();
  if (!s.sv) return false;  // nothing bound: harness / preview never runs scripts
  if (s.dry) return true;   // tests: the call lands in dry_log()
  return selected_hooks(event);
}

std::vector<std::string> fire_event(const std::string& event, const std::vector<std::string>& args) {
  State& s = st();
  if (dry_run()) {
    std::string line = "event " + event;
    for (const std::string& a : args) line += " " + a;
    s.dry_log.push_back(line);
    return {};
  }
  const std::vector<std::string>& dirs = script_dirs();
  if (ends_with(event, "-selected")) {  // fires on every cursor move: do not hit the disk each time
    if (!selected_hooks(event)) return {};
  } else if (!launch::has_hooks(dirs, event)) {
    return {};
  }
  return launch::fire_event(dirs, event, args, event == "quit");  // on Linux only `quit` is synchronous
}

void fire_game_selected(const std::string& system, const std::string& path, const std::string& name) {
  if (selected_listening("game-selected")) fire_event("game-selected", {system, path, name});
}
void fire_system_selected(const std::string& system) {
  if (selected_listening("system-selected")) fire_event("system-selected", {system});
}

// ---- inactivity / screensaver ---------------------------------------------------------------------------------------

IdleTimer& idle() { return st().idle; }

std::optional<TimePoint> idle_deadline(const EsSettings& es) {
  IdleTimer& t = st().idle;
  t.set_timeout(es.get_int("ScreenSaverTime", 300000));
  if (!t.stamped()) t.input(Clock::now());
  return t.deadline();
}

void screensaver_start(const EsSettings& es) {
  State& s = st();
  IdleTimer& t = s.idle;
  if (t.active()) return;
  const std::string* b = es.get("ScreenSaverBehavior");
  std::string behavior = b && !b->empty() ? *b : "black";
  if (behavior == "suspend") {
    if (suspend_supported()) {  // ES: suspends and leaves no screensaver behind (sleep.sh lights the panel again on resume)
      suspend();
      t.input(Clock::now());
      return;
    }
    behavior = "black";
  }
  // zfront ports black and dim; the video / slideshow types of ES fall back to black (ES does too when it has no media)
  screensaver::Mode m = screensaver::mode_for_behavior(behavior);
  t.set_active(true);
  screensaver::set_mode(m);
  s.redraw = true;
  // black: the panel goes off (the es-script does it too; writing it here keeps it working without scripts). dim keeps the
  // light: that is the point of dimming (the fork's 10-backlight-off.sh must skip "dim", see os-patches/0005).
  if (m == screensaver::Mode::Black) backlight(4);
  fire_event("screensaver-start", {m == screensaver::Mode::Dim ? "dim" : "black"});
  fire_event("sleep");
}

void screensaver_stop() {
  State& s = st();
  IdleTimer& t = s.idle;
  if (!t.active()) return;
  t.set_active(false);
  screensaver::set_mode(screensaver::Mode::Off);
  s.redraw = true;
  backlight(0);  // also after dim: an es-script may have turned the light off
  fire_event("screensaver-stop");
  fire_event("wake");
}

bool saver_blocked() {
  State& s = st();
  return game_running() || busy() || (s.sv && !s.sv->jobs.idle());
}

bool resume_pending() { return st().resume.peek(screensaver::clock_offset_ns()); }

bool take_redraw() {
  bool r = st().redraw;
  st().redraw = false;
  return r;
}

std::optional<TimePoint> saver_deadline(const EsSettings& es, TimePoint now) {
  if (!st().idle.active() || screensaver::mode() != screensaver::Mode::Dim) return std::nullopt;
  if (!es.get_bool("DrawClock", true)) return std::nullopt;
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  double into = double(ts.tv_sec % 60) + double(ts.tv_nsec) / 1e9;
  return now + std::chrono::milliseconds(screensaver::ms_to_next_minute(into));
}

void before_acts(const EsSettings& es, TimePoint now, std::vector<Action>& acts, bool raw_input) {
  State& s = st();
  IdleTimer& t = s.idle;
  t.set_timeout(es.get_int("ScreenSaverTime", 300000));
  if (!t.stamped()) t.input(now);
  bool resumed = s.resume.check(screensaver::clock_offset_ns());
  if (t.active()) {
    if (!acts.empty() || raw_input || resumed) {  // the first key only wakes the panel; a suspend wakes it too
      acts.clear();
      screensaver_stop();
      t.input(now);
    }
    return;
  }
  if (resumed || raw_input) t.input(now);  // coming back from a suspend / an unmapped button is activity
  if (acts.empty() && t.due(now)) {
    if (saver_blocked())
      t.defer(now, std::chrono::seconds(5));  // look again soon; never leave a past deadline (the loop would spin)
    else
      screensaver_start(es);
  }
}

void input(TimePoint now) { st().idle.input(now); }

// ---- running game / busy --------------------------------------------------------------------------------------------

void game_started(const std::string& rom, const std::string& name) {
  State& s = st();
  {
    std::lock_guard<std::mutex> g(s.gm);
    s.game_on = true;
    s.game_path = rom;
    s.game_name = name;
  }
  if (s.wmclass_set) {  // emulators must not inherit our app_id
    ::unsetenv("SDL_VIDEO_WAYLAND_WMCLASS");
    s.wmclass_set = false;
  }
}

void game_ended() {
  State& s = st();
  {
    std::lock_guard<std::mutex> g(s.gm);
    s.game_on = false;
    s.game_path.clear();
    s.game_name.clear();
  }
  s.idle.input(Clock::now());  // a game of hours must not end in an instant screensaver
}

bool game_running() {
  std::lock_guard<std::mutex> g(st().gm);
  return st().game_on;
}

std::string running_game_json() {
  std::lock_guard<std::mutex> g(st().gm);
  if (!st().game_on) return {};
  return "{\"name\":\"" + json_escape(st().game_name) + "\",\"path\":\"" + json_escape(st().game_path) + "\"}";
}

Busy::Busy() { ++st().busy; }
Busy::~Busy() { --st().busy; }
bool busy() { return st().busy.load() > 0; }

bool emukill() {
  if (!game_running()) return false;
  std::string t = kill_target();
  if (!safe_process_name(t) || t == "emulationstation" || t == exe_basename()) return false;
  runner().spawn("killall " + shell_quote(t));
  return true;
}

// ---- external requests ----------------------------------------------------------------------------------------------

void request_launch(const std::string& rom_path) { st().pending_launch = rom_path; }

bool resolve_launch(Ctx& cx, const std::string& rom_path, Outcome& out) {
  std::string p(trim(rom_path));
  if (p.empty() || p[0] != '/') return false;
  for (size_t i = 0; i < cx.systems.size(); ++i) {
    std::string base = cx.systems[i].rom_dir;
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    if (base.empty() || p.size() <= base.size() || p.compare(0, base.size(), base) != 0 || p[base.size()] != '/') continue;
    cx.open_system(i);  // only the system that holds the path
    const System& sys = cx.systems[i];
    for (size_t j = 0; j < sys.games.size(); ++j) {
      if (sys.games[j].path != p) continue;
      std::vector<JoyInfo> joys;
      if (cx.pad) joys = cx.pad->joysticks();
      std::string cmd = launch::build_command(sys, sys.games[j], cx.conf.map(), joys);
      if (cmd.empty()) return false;
      out = Outcome{Outcome::Launch, std::move(cmd), int(i), int(j)};
      return true;
    }
  }
  return false;
}

std::optional<Outcome> take_external(App& app) {
  State& s = st();
  if (s.pending_launch.empty()) return std::nullopt;
  std::string p = std::move(s.pending_launch);
  s.pending_launch.clear();
  Outcome o;
  if (!resolve_launch(app.cx, p, o)) {
    std::fprintf(stderr, "zfront: POST /launch: no game with path %s\n", p.c_str());
    return std::nullopt;
  }
  screensaver_stop();  // ES: cancelScreenSaver() before launching
  s.idle.input(Clock::now());
  return o;
}

// ---- helpers --------------------------------------------------------------------------------------------------------

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o.push_back(char(c));
        }
    }
  }
  return o;
}

std::string sanitize_notice(const std::string& s, size_t max_bytes) {
  std::string o;
  o.reserve(std::min(s.size(), max_bytes));
  for (unsigned char c : s) o.push_back(c < 0x20 || c == 0x7f ? ' ' : char(c));
  std::string_view t = trim(o);
  size_t cut = std::min(t.size(), max_bytes);
  while (cut > 0 && cut < t.size() && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;  // do not split a UTF-8 sequence
  return std::string(t.substr(0, cut));
}

void reset_for_test() {
  State& s = st();
  s.sv.reset();
  s.dry = false;
  s.dry_log.clear();
  s.mode = Mode::Quit;
  s.quit = false;
  s.idle = IdleTimer();
  s.resume.reset();
  s.redraw = false;
  screensaver::set_mode(screensaver::Mode::Off);
  s.dirs.clear();
  s.dirs_set = false;
  s.hook_cache.clear();
  s.pending_launch.clear();
  s.ready_done = false;
  s.wmclass_set = false;
  s.finish_hooks.clear();
  {
    std::lock_guard<std::mutex> g(s.gm);
    s.game_on = false;
    s.game_path.clear();
    s.game_name.clear();
  }
  s.busy = 0;
  g_hup = 0;
  g_forward_term = 0;
  default_paths() = Paths();
}

}  // namespace oscontract
}  // namespace zf
