#include "launch.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include "app.hpp"
#include "gamelist.hpp"
#include "gamemeta.hpp"
#include "savestates.hpp"
#include "settings.hpp"
#include "util.hpp"

extern char** environ;

namespace zf {
namespace launch {

std::string controllers_config(const std::vector<JoyInfo>& joys) {
  std::string s;
  for (size_t i = 0; i < joys.size() && i < kMaxPlayers; ++i) {
    std::string n = std::to_string(i + 1);
    s += " -p" + n + "index " + std::to_string(joys[i].index) + " -p" + n + "guid " + joys[i].guid + " ";
  }
  return s;
}

// `getGenericPath`: `\` to `/`, collapse `//`, drop trailing `/`.
static std::string generic_path(const std::string& p) {
  std::string s;
  s.reserve(p.size());
  for (char c : p) {
    if (c == '\\') c = '/';
    if (c == '/' && !s.empty() && s.back() == '/') continue;
    s.push_back(c);
  }
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

std::string escape_path(const std::string& p) {
  std::string b = generic_path(p);
  // ES escapes `\ '"!$^&*(){}[]?;<>`. Backtick, `|`, tab and newline are added here: without them a ROM called
  // "a`id`.gb" or "a|b.gb" runs commands (the line falls back to `sh -c`). `\<newline>` is a line continuation, so a ROM
  // with a newline in its name cannot start, but nothing else runs either.
  for (char c : std::string("\\ '\"!$^&*(){}[]?;<>`|\t\n")) {
    size_t start = 0;
    while (start < b.size()) {
      size_t off = b.find(c, start);
      if (off == std::string::npos) break;
      start = off + 1;
      if (off == 0 || b[off - 1] != '\\') {
        b.insert(off, 1, '\\');
        start += 1;
      }
    }
  }
  return b;
}

// ES `formatCommandLineArgument` quotes the value only when it has a space and escapes just `"`. The value (game or system
// name) comes from gamelist.xml or the scraper and ends up in a shell line, so it is also quoted when it holds anything the
// shell interprets, and `\`, `$` and backtick are escaped inside the quotes. Plain names come out exactly as in ES.
static std::string cmdline_arg(const std::string& s) {
  bool quote = false;
  std::string e;
  for (char c : s) {
    if (c == '"' || c == '\\' || c == '$' || c == '`') e += '\\';
    e += c;
    if (std::string_view(" \t\r\n;&|<>(){}[]*?!#~'\"$`\\^").find(c) != std::string_view::npos) quote = true;
  }
  return quote ? "\"" + e + "\"" : e;
}

static void replace_all(std::string& s, const std::string& from, const std::string& to) {
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
}

std::string build_command(const System& sys, const Game& game, const ConfMap& conf, const std::vector<JoyInfo>& joys) {
  if (sys.command.empty()) return std::string();
  auto [emulator, core] = resolve_emu(sys, game, conf);
  const std::string& rom_raw = game.path;
  const char* home = std::getenv("HOME");
  std::string c = sys.command;  // replacements in ES order, each on the result of the previous one
  replace_all(c, "%SYSTEM%", sys.name);
  replace_all(c, "%ROM%", escape_path(rom_raw));
  replace_all(c, "%BASENAME%", path_stem(rom_raw));
  replace_all(c, "%ROM_RAW%", rom_raw);
  replace_all(c, "%EMULATOR%", emulator);
  replace_all(c, "%CORE%", core);
  replace_all(c, "%HOME%", home ? home : "");
  replace_all(c, "%GAMENAME%", cmdline_arg(game.name));
  replace_all(c, "%SYSTEMNAME%", cmdline_arg(sys.fullname));
  replace_all(c, "%GAMEINFOXML%", "");
  replace_all(c, "%CONTROLLERSCONFIG%", controllers_config(joys));
  replace_all(c, "%NETPLAY%", "");
  return c;
}

AfterLaunch after_launch(int exit_code, unsigned long session_secs) {
  AfterLaunch o;
  bool ok = exit_code == 0;
  o.count_play = ok;
  o.add_gametime = (ok && session_secs >= kMinPlaytimeSecs) ? session_secs : 0;
  o.reload_lists = ok;
  if (exit_code == 250 || exit_code == 299) o.error = {ExitError::LogFile, exit_code};
  else if (exit_code >= 200 && exit_code <= 300) o.error = {ExitError::Code, exit_code};
  return o;
}

std::string error_message(const ExitError& e, const std::string* log_text) {
  switch (e.code) {
    case 200: return N_("THE EMULATOR EXITED UNEXPECTEDLY");
    case 201: return N_("BAD COMMAND LINE ARGUMENTS");
    case 202: return N_("INVALID CONFIGURATION");
    case 203: return N_("UNKNOWN EMULATOR");
    case 204: return N_("EMULATOR IS MISSING");
    case 205: return N_("CORE IS MISSING");
    default: break;
  }
  if (e.kind == ExitError::LogFile && log_text && !log_text->empty()) return *log_text;
  return "UKNOWN ERROR : " + std::to_string(e.code);
}

// ---- split_simple / spawn / run ------------------------------------------------------------------------------

bool split_simple(std::string_view cmd, std::vector<std::string>& out) {
  out.clear();
  std::string cur;
  bool have = false, q = false;
  for (size_t i = 0; i < cmd.size(); ++i) {
    char c = cmd[i];
    switch (c) {
      case '\\': {
        if (++i >= cmd.size()) return false;  // trailing backslash
        char n = cmd[i];
        if (n == '\n') return false;          // line continuation: sh drops both
        if (q && n != '"' && n != '\\') return false;  // sh keeps the backslash inside quotes for other chars
        cur.push_back(n);
        have = true;
        break;
      }
      case '"':
        q = !q;
        have = true;
        break;
      case ' ':
      case '\t':
        if (q) {
          cur.push_back(c);
        } else if (have) {
          out.push_back(std::move(cur));
          cur.clear();
          have = false;
        }
        break;
      case '$':
      case '`':
      case '\n':
        return false;  // expansion (inside quotes) or command syntax (outside)
      case '\'': case ';': case '&': case '|': case '<': case '>': case '(': case ')': case '*': case '?':
      case '~': case '#': case '{': case '}': case '[': case ']': case '!':
        // conservative: some only matter in certain positions, but ES paths are escaped
        if (!q) return false;
        cur.push_back(c);
        have = true;
        break;
      default:
        cur.push_back(c);
        have = true;
        break;
    }
  }
  if (q) return false;
  if (have) out.push_back(std::move(cur));
  // NAME=value first would be an assignment for sh
  return !out.empty() && out[0].find('=') == std::string::npos;
}

namespace {

struct SpawnOpts {
  bool idle = false;       // SCHED_IDLE
  bool own_group = false;  // new process group (killpg)
  bool quiet = false;      // stdin /dev/null, stdout+stderr to `out_path` or /dev/null
  const char* out_path = nullptr;
};

// pid > 0, or -errno (glibc's posix_spawn reports exec errors itself, so a missing program costs no fork surprise).
int spawn_raw(const char* path, const char* const* argv, const SpawnOpts& o, bool search_path) {
  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  short flags = 0;
  if (o.own_group) {
    flags |= POSIX_SPAWN_SETPGROUP;
    posix_spawnattr_setpgroup(&at, 0);
  }
  short sched_flag = 0;
  if (o.idle) {
    sched_flag = POSIX_SPAWN_SETSCHEDULER;
    sched_param sp{};
    posix_spawnattr_setschedpolicy(&at, SCHED_IDLE);
    posix_spawnattr_setschedparam(&at, &sp);
  }
  // the child must not inherit a blocked-signal mask or ignored signals from SDL / the harness
  sigset_t none, all;
  sigemptyset(&none);
  sigfillset(&all);
  posix_spawnattr_setsigmask(&at, &none);
  posix_spawnattr_setsigdefault(&at, &all);
  flags |= POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (o.quiet) {
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (o.out_path) posix_spawn_file_actions_addopen(&fa, 1, o.out_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
  }
  char* const* av = const_cast<char* const*>(argv);
  pid_t pid = 0;
  posix_spawnattr_setflags(&at, short(flags | sched_flag));
  int rc = search_path ? posix_spawnp(&pid, path, &fa, &at, av, environ) : posix_spawn(&pid, path, &fa, &at, av, environ);
  if (rc != 0 && sched_flag && rc != ENOENT && rc != EACCES && rc != ENOEXEC) {  // scheduler refused: normal priority
    posix_spawnattr_setflags(&at, flags);
    rc = search_path ? posix_spawnp(&pid, path, &fa, &at, av, environ) : posix_spawn(&pid, path, &fa, &at, av, environ);
  }
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  return rc == 0 ? int(pid) : -rc;
}

int spawn_sh(const std::string& line, const SpawnOpts& o) {
  const char* av[] = {"sh", "-c", line.c_str(), nullptr};
  return spawn_raw("/bin/sh", av, o, false);
}

// A hook script gets its arguments as separate argv entries, never through a shell: game names and paths come from
// gamelist.xml and could otherwise run commands. A script without a shebang (ENOEXEC) is handed to `sh`, as `sh -c` did.
int spawn_hook(const std::string& script, const std::string* event, const std::vector<std::string>& args, const SpawnOpts& o) {
  std::vector<std::string> a{script};
  if (event) a.push_back(*event);
  for (size_t i = 0; i < args.size() && i < 3; ++i) {
    if (args[i].empty()) break;
    a.push_back(args[i]);
  }
  std::vector<const char*> av;
  for (const std::string& s : a) av.push_back(s.c_str());
  av.push_back(nullptr);
  int pid = spawn_raw(av[0], av.data(), o, false);
  if (pid == -ENOEXEC) {
    av.insert(av.begin(), "sh");
    pid = spawn_raw("/bin/sh", av.data(), o, false);
  }
  return pid;
}

// Direct spawn of a simple command; -errno if it is not simple or the exec failed.
int spawn_simple(const std::string& cmd, const SpawnOpts& o) {
  std::vector<std::string> a;
  if (!split_simple(cmd, a)) return -EINVAL;
  std::vector<const char*> av;
  av.reserve(a.size() + 1);
  for (const std::string& s : a) av.push_back(s.c_str());
  av.push_back(nullptr);
  return spawn_raw(av[0], av.data(), o, true);
}

int wait_status(int pid) {
  int st = 0;
  while (::waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return st;
}

int g_hook_timeout_ms = 30000;

// A synchronous hook (`quit`) must not hold the shutdown for ever: after `ms` the whole process group (`own_group`) is killed.
int wait_status_limited(int pid, int ms) {
  int st = 0;
  for (int waited = 0;; waited += 2) {
    pid_t r = ::waitpid(pid, &st, WNOHANG);
    if (r == pid) return st;
    if (r < 0 && errno != EINTR) return -1;
    if (waited >= ms) break;
    timespec ts{0, 2 * 1000 * 1000};
    ::nanosleep(&ts, nullptr);
  }
  ::kill(-pid, SIGKILL);
  return wait_status(pid);
}

}  // namespace

namespace {
// SIGTERM during a game: SDL is shut down then, so nobody handles it and the default action would kill zfront and leave the
// game orphaned. Instead the game gets the signal (a second SIGTERM kills it) and zfront ends after it.
volatile sig_atomic_t g_game_pid = 0;
volatile sig_atomic_t g_term_count = 0;

void on_term(int) {
  ++g_term_count;
  pid_t c = pid_t(g_game_pid);
  if (c > 0) ::kill(c, g_term_count > 1 ? SIGKILL : SIGTERM);
}
}  // namespace

bool term_requested() { return g_term_count > 0; }

RunResult run(const std::string& cmd) {
  auto t0 = Clock::now();
  RunResult r;
  struct sigaction sa, old;
  sa.sa_handler = on_term;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // no SA_RESTART: a blocked waitpid wakes up (and simply waits again)
  ::sigaction(SIGTERM, &sa, &old);
  int pid = spawn_simple(cmd, {});
  if (pid <= 0) pid = spawn_sh(cmd, {});  // not simple, or exec failed (sh reports 127 / runs a shebang-less script)
  if (pid > 0) {
    g_game_pid = pid;
    if (g_term_count > 0) ::kill(pid, SIGTERM);  // it arrived before the pid was known
    int st = wait_status(pid);
    g_game_pid = 0;
    r.exit_code = (st >= 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : 1;
  }
  ::sigaction(SIGTERM, &old, nullptr);
  r.secs = static_cast<unsigned long>(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t0).count());
  return r;
}

// ---- hooks ---------------------------------------------------------------------------------------------------

std::vector<std::string> default_script_dirs() {
  std::vector<std::string> v = {"/storage/.config/emulationstation/scripts"};
  char buf[512];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) {
    buf[n] = 0;
    v.push_back(path_join(path_dir(buf), "scripts"));
  }
  v.push_back("/var/run/emulationstation/scripts");
  return v;
}

namespace {

std::vector<int>& hook_pids() {
  static std::vector<int> v;
  return v;
}

bool is_dir_entry(const std::string& dir, const dirent* e) {
  if (e->d_type == DT_DIR) return true;
  if (e->d_type != DT_UNKNOWN && e->d_type != DT_LNK) return false;
  struct stat st;
  return ::stat(path_join(dir, e->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Executable non-directory entries of `dir`, sorted. With `out == nullptr` it only answers "is there one" and stops at
// the first. With `event` set, a sub-directory called `event` is also searched one level (not recursively): that is
// what `has_hooks` needs; `fire_event` lists each directory separately.
bool list_scripts(const std::string& dir, std::vector<std::string>* out) {
  DIR* d = ::opendir(dir.c_str());
  if (!d) return false;
  bool any = false;
  while (const dirent* e = ::readdir(d)) {
    if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
    if (e->d_type == DT_DIR) continue;
    std::string p = path_join(dir, e->d_name);
    if (e->d_type != DT_REG && is_dir_entry(dir, e)) continue;
    if (::access(p.c_str(), X_OK) != 0) continue;
    any = true;
    if (!out) break;
    out->push_back(std::move(p));
  }
  ::closedir(d);
  if (out) std::sort(out->begin(), out->end());
  return any;
}

std::string script_line(const std::string& script, const std::string* event, const std::vector<std::string>& args) {
  std::string c = script.find(' ') != std::string::npos ? "\"" + script + "\"" : script;
  if (event) c += " " + *event;
  for (size_t i = 0; i < args.size() && i < 3; ++i) {
    if (args[i].empty()) break;
    std::string d;
    for (char ch : args[i])
      if (ch != '"') d.push_back(ch);
    c += d.find(' ') != std::string::npos ? " \"" + d + "\"" : " " + d;
  }
  return c;
}

std::vector<std::string> unique_dirs(const std::vector<std::string>& dirs) {
  std::vector<std::string> u;
  for (const std::string& d : dirs)
    if (std::find(u.begin(), u.end(), d) == u.end()) u.push_back(d);
  return u;
}

}  // namespace

void set_hook_timeout_ms(int ms) { g_hook_timeout_ms = ms; }

void reap_hooks() {
  std::vector<int>& v = hook_pids();
  for (size_t i = 0; i < v.size();) {
    int st = 0;
    pid_t r = ::waitpid(v[i], &st, WNOHANG);
    if (r == 0) {
      ++i;
    } else {
      v.erase(v.begin() + long(i));
    }
  }
}

bool has_hooks(const std::vector<std::string>& dirs, std::string_view event) {
  for (const std::string& d : dirs) {
    if (list_scripts(path_join(d, event), nullptr)) return true;
    if (list_scripts(d, nullptr)) return true;
  }
  return false;
}

std::vector<std::string> fire_event(const std::vector<std::string>& dirs, const std::string& event, const std::vector<std::string>& args, bool wait) {
  std::vector<std::string> ran;
  reap_hooks();
  auto exec = [&](const std::string& script, const std::string* ev) {
    SpawnOpts o;
    o.quiet = true;
    o.own_group = true;
    int pid = spawn_hook(script, ev, args, o);
    if (pid > 0) {
      if (wait) wait_status_limited(pid, g_hook_timeout_ms);
      else hook_pids().push_back(pid);
    }
    ran.push_back(script_line(script, ev, args));  // what a shell line would have looked like: for logs and tests
  };
  std::vector<std::string> u = unique_dirs(dirs), files;
  for (const std::string& d : u) {
    files.clear();
    list_scripts(path_join(d, event), &files);
    for (const std::string& s : files) exec(s, nullptr);
  }
  for (const std::string& d : u) {
    files.clear();
    list_scripts(d, &files);
    for (const std::string& s : files) exec(s, &event);
  }
  return ran;
}

std::vector<std::string> game_start(const std::vector<std::string>& dirs, const std::string& rom_raw, const std::string& name) {
  if (!has_hooks(dirs, "game-start")) return {};
  return fire_event(dirs, "game-start", {generic_path(rom_raw), path_stem(rom_raw), name}, false);
}

std::vector<std::string> game_end(const std::vector<std::string>& dirs) {
  if (!has_hooks(dirs, "game-end")) return {};
  return fire_event(dirs, "game-end", {}, false);
}

// ---- LaunchPrep ----------------------------------------------------------------------------------------------

std::string prepare_line(std::string_view cmd, std::string_view program) {
  size_t sp = cmd.find(' ');
  std::string_view first = cmd.substr(0, sp);
  size_t sl = first.rfind('/');
  if (sl != std::string_view::npos) first.remove_prefix(sl + 1);
  if (first.empty() || first != program) return {};
  std::string s(cmd.substr(0, sp));
  s += " --prepare";
  if (sp != std::string_view::npos) s.append(cmd.substr(sp));
  return s;
}

Spawner idle_spawner(std::string log_path) {
  return [log = std::move(log_path)](const std::string& line) {
    SpawnOpts o;
    o.idle = true;
    o.own_group = true;
    o.quiet = true;
    o.out_path = log.empty() ? nullptr : log.c_str();
    return spawn_simple(line, o);  // no sh fallback: a missing runemu.sh simply means "not prepared"
  };
}

int prepare(const std::string& cmd, const Spawner& spawn, std::string_view program) {
  std::string line = prepare_line(cmd, program);
  if (line.empty()) return 0;
  int pid = spawn(line);
  return pid > 0 ? pid : -1;
}

LaunchPrep::LaunchPrep() : LaunchPrep(Config{}) {}
LaunchPrep::LaunchPrep(Config c) : cfg_(std::move(c)) {
  spawn_ = cfg_.spawn ? cfg_.spawn : idle_spawner(cfg_.log_path);
  if (!cfg_.generation) cfg_.generation = settings_generation;
  gen_ = cfg_.generation();
}

void LaunchPrep::check_generation(TimePoint now) {
  unsigned g = cfg_.generation();
  if (g == gen_) return;
  gen_ = g;
  last_.clear();  // ratio, shader, game options...: system.cfg / es_settings.cfg changed, so what runemu prepared is stale
  waiting_ = !target_.empty() && !prepare_line(target_, cfg_.program).empty();
  due_ = now + std::chrono::milliseconds(cfg_.dwell_ms);
}
LaunchPrep::~LaunchPrep() { reap(); }

void LaunchPrep::reap() {
  if (pid_ <= 0) return;
  int st = 0;
  pid_t r = ::waitpid(pid_, &st, WNOHANG);
  if (r != 0) pid_ = 0;  // finished (or not ours any more)
}

void LaunchPrep::set_target(std::string_view cmd, TimePoint now) {
  check_generation(now);
  if (cmd == target_) return;
  target_.assign(cmd);
  waiting_ = !target_.empty() && target_ != last_ && !prepare_line(target_, cfg_.program).empty();
  due_ = now + std::chrono::milliseconds(cfg_.dwell_ms);
}

std::optional<TimePoint> LaunchPrep::next_deadline() const {
  if (waiting_) return due_;
  return std::nullopt;
}

void LaunchPrep::poll(TimePoint now) {
  reap();
  check_generation(now);
  if (!waiting_ || now < due_) return;
  if (pid_ > 0) {  // one at a time
    due_ = now + std::chrono::milliseconds(cfg_.retry_ms);
    return;
  }
  waiting_ = false;
  int pid = prepare(target_, spawn_, cfg_.program);
  last_ = target_;  // do not repeat for the same command (also when it failed)
  if (pid > 0) {
    pid_ = pid;
    ++spawned_;
  }
}

void LaunchPrep::launched() {
  reap();
  if (pid_ > 0) {  // the user is waiting for runemu, which waits for this: stop being idle-priority (best effort)
    sched_param sp{};
    ::sched_setscheduler(pid_, SCHED_OTHER, &sp);
  }
  target_.clear();
  last_.clear();
  waiting_ = false;
}

// ---- execute -------------------------------------------------------------------------------------------------

Outcome execute(const Request& r, const Env& env) {
  Outcome o;
  if (env.prep) env.prep->launched();
  const std::string& cmd = r.cmd;
  std::vector<std::string> dirs = env.script_dirs.empty() ? default_script_dirs() : env.script_dirs;
  std::fprintf(stderr, "zfront: launching at uptime %s: %s\n", uptime_text().c_str(), cmd.c_str());
  game_start(dirs, r.rom_raw, r.name);
  if (env.release) env.release();
  o.run = run(cmd);
  game_end(dirs);
  if (env.restore && !term_requested()) env.restore();  // being stopped: do not rebuild the window
  std::fprintf(stderr, "zfront: back at uptime %s after %lu s (exit %d)\n", uptime_text().c_str(), o.run.secs, o.run.exit_code);
  o.after = after_launch(o.run.exit_code, o.run.secs);
  if (o.after.error.kind != ExitError::None) {
    std::string log;
    const std::string* lp = nullptr;
    if (o.after.error.kind == ExitError::LogFile && read_file("/tmp/launch_error.log", log)) {
      lp = &log;
      ::unlink("/tmp/launch_error.log");
    }
    o.message = error_message(o.after.error, lp);
  }
  return o;
}

void apply_after(Ctx& cx, size_t sys, size_t game, const Outcome& o) {
  savestates::after_game();  // HOOK menu-collections: undo of the savestate preparation (no-op without a plan)
  if (!o.after.count_play) return;
  game_played(cx, sys, game, o.after.add_gametime);  // memory now, gamelist.xml on the job thread
}

std::string error_toast(const Outcome& o) {
  if (o.message.empty()) return {};
  std::string_view line = trim(o.message);
  line = trim(line.substr(0, line.find_first_of("\r\n")));  // the log of a script can be long: first line only
  std::string text = tr("AN ERROR OCCURRED");
  if (line.empty()) return text;
  constexpr size_t kMaxChars = 44;  // the toast is one line and fits a 480 px screen (toast.cpp does not cut by itself)
  std::string msg(line);
  if (utf8_len(msg) > kMaxChars) {
    size_t i = 0;
    for (size_t n = 0; n < kMaxChars && i < msg.size(); ++n) utf8_next(msg, i);
    msg.resize(i);
    msg += "\xE2\x80\xA6";
  } else {
    msg = tr(msg.c_str());  // the fixed texts of codes 200..205 are in the catalogue
  }
  return text + ": " + msg;
}

}  // namespace launch
}  // namespace zf
