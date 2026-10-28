// MODULE `launch` (owner: launch agent). Game launching as ES does it (`FileData::getlaunchCommand` / `launchGame`):
// command placeholders, controller arguments, post-run bookkeeping rules, the runner, the `game-start`/`game-end`
// user script hooks and the idle-time preparation (`LaunchPrep`). Everything except `run`, `fire_event`, `execute`
// and `LaunchPrep` is pure.
//
// The escaping is the one of ES (`Utils::FileSystem::getEscapedPath`, ported literally from the Rust branch
// zf/launch); it differs from the skeleton Rust `systems.rs` on purpose (that one did not match ES).
//
// Assumptions about the OS side (for `runemu.sh`):
//  * `run` returns as soon as the PROCESS WE SPAWNED exits; we read no pipe, so a cleanup that `runemu.sh` detaches
//    (`( ... ) &`) never delays us, while a cleanup done in the foreground does.
//  * Exit codes: 0 = ok (playcount), anything else = no bookkeeping, 200..300 = error box text.
//  * stdout/stderr of the game are inherited (ES redirects them to es_launch_*.log; zfront does not).
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pad.hpp"  // JoyInfo
#include "systems.hpp"
#include "util.hpp"

namespace zf {
struct Ctx;
namespace launch {

constexpr size_t kMaxPlayers = 8;
/// Sessions shorter than this do not add play time (ES: probably a loading problem).
constexpr unsigned long kMinPlaytimeSecs = 10;

/// `" -pNindex I -pNguid G "` per player (player 1 first), at most 8. None gives an empty string.
std::string controllers_config(const std::vector<JoyInfo>& joys);
/// `Utils::FileSystem::getEscapedPath` (Linux): generic path, then a backslash before each of `\ '"!$^&*(){}[]?;<>`.
/// Deviation from ES: backtick, `|`, tab and newline are escaped too (ES leaves them, so a ROM called "a`id`.gb" runs
/// commands through `sh -c`). `#` and `~` stay as they are: they only matter at the start of a word and a path starts with `/`.
std::string escape_path(const std::string& p);
/// Full command line ES would run for `game` (netplay and savestate arguments excluded). An empty `sys.command`
/// gives an empty string (ES aborts the launch).
std::string build_command(const System& sys, const Game& game, const ConfMap& conf, const std::vector<JoyInfo>& joys);

struct ExitError {
  enum Kind { None, Code, LogFile } kind = None;
  int code = 0;
};

struct AfterLaunch {
  /// Add 1 to PlayCount and set LastPlayed to now (only exit 0).
  bool count_play = false;
  /// Seconds to add to GameTime (0 if the session was shorter than `kMinPlaytimeSecs` or exit != 0).
  unsigned long add_gametime = 0;
  /// Reload all game lists keeping the position (ES does it when exit == 0).
  bool reload_lists = false;
  /// ES shows an error box for 200..=300.
  ExitError error;
};
AfterLaunch after_launch(int exit_code, unsigned long session_secs);
/// ES `getMessageFromExitCode` (English ids; translate at the call site). `log_text` = `/tmp/launch_error.log`.
std::string error_message(const ExitError& e, const std::string* log_text);

struct RunResult {
  int exit_code = 1;
  unsigned long secs = 0;
};

/// Splits `cmd` into argv as `sh` would, but only for the shape ES commands have: words separated by blanks, `\x`
/// escapes and "double quoted" parts without `$`/backtick. Anything else a shell would interpret returns false (the
/// caller falls back to `sh -c`). `escape_path` makes ROM paths safe for this. One pass over the string.
bool split_simple(std::string_view cmd, std::vector<std::string>& argv);

/// Runs `cmd` and waits, inheriting stdio. A command of the simple shape is `posix_spawnp`ed directly (no `sh`);
/// otherwise, or if the direct spawn fails (script without shebang...), `sh -c` is used. A process killed by a signal
/// reports 1 (no playcount).
RunResult run(const std::string& cmd);
/// A SIGTERM (stop, or SIGHUP forwarded by oscontract) arrived while a game ran. `run` handles it itself: the signal is
/// passed to the game (a second one kills it), `run` waits for it to end and returns normally, and the loop then quits
/// instead of coming back to the menus. Stays true for the rest of the process.
bool term_requested();

/// `/storage/.config/emulationstation/scripts`, `<exe dir>/scripts`, `/var/run/emulationstation/scripts`.
std::vector<std::string> default_script_dirs();
/// True when `event` has something to run in `dirs` (one `opendir` per dir, no allocation when there is nothing).
bool has_hooks(const std::vector<std::string>& dirs, std::string_view event);
/// `Scripting::fireEvent`: every executable file of `<dir>/<event>/` and then every executable plain file of `<dir>/`
/// (event name as first argument), each dir once, sorted by name; spawned directly with the arguments as separate argv
/// entries (ES builds a quoted `sh -c` line; a name with `$(...)` or `;` must not run anything), stdio to /dev/null, at
/// most 3 arguments, up to the first empty one. `wait=false` does not wait (children are collected by later `reap_hooks`
/// calls); `wait=true` kills the script's process group after `set_hook_timeout_ms`. Returns the shell-style lines (logs, tests).
std::vector<std::string> fire_event(const std::vector<std::string>& dirs, const std::string& event, const std::vector<std::string>& args, bool wait);
/// Collects finished hook children (non-blocking); costs nothing when there are none.
void reap_hooks();
/// Time a synchronous hook may run before it is killed (default 30 s). For tests.
void set_hook_timeout_ms(int ms);
/// `game-start` with (ROM path, stem, game name) / `game-end` without arguments. Free when no script exists. The path is
/// the real one (ES passed it shell-escaped, which a script had to undo).
std::vector<std::string> game_start(const std::vector<std::string>& dirs, const std::string& rom_raw, const std::string& name);
std::vector<std::string> game_end(const std::vector<std::string>& dirs);

// ---------------------------------------------------------------------------------------------------------------
// Idle-time preparation ("games open instantly"). Interface of runemu.sh (fork branch launch-fast):
// `runemu.sh --prepare <SAME arguments as the real launch>` prepares the RetroArch config, exits 0, leaves
// /run/zf-prep/{marker,extraopts} and ignores what it cannot prepare. The real launch is the UNCHANGED command: runemu
// reuses the work only if the arguments are identical and the config files did not change, otherwise it does
// everything as usual. So there is no key and no `--prepared`: all we do is run `--prepare` at the right time.
// Rules: ~1 s of cursor rest on a game, one at a time, SCHED_IDLE (own process group), never killed (if the user
// launches meanwhile runemu waits <= 1 s for the lock; we only raise the child's priority). No thread: a spawn costs
// ~100 us and the child is reaped lazily; the loop only needs `next_deadline`/`poll`.

/// The `--prepare` line for a launch command: the first word (basename `program`), ` --prepare`, then the rest of the
/// arguments byte for byte. Empty when `cmd` is not a command of `program`.
std::string prepare_line(std::string_view cmd, std::string_view program = "runemu.sh");
/// Spawns a command line without waiting; returns the pid (> 0) or <= 0 on failure. Injectable for tests.
using Spawner = std::function<int(const std::string& line)>;
/// The real spawner: direct posix_spawnp at SCHED_IDLE in its own process group, stdio to /dev/null (or `log_path`).
Spawner idle_spawner(std::string log_path = {});
/// `prepare(req)`: builds the `--prepare` line of `cmd` and starts it with `spawn`. Returns the pid, 0 if `cmd` is not
/// preparable, < 0 if the spawn failed.
int prepare(const std::string& cmd, const Spawner& spawn, std::string_view program = "runemu.sh");

class LaunchPrep {
 public:
  struct Config {
    std::string program = "runemu.sh";
    unsigned dwell_ms = 1000;  // cursor rest time before spawning
    unsigned retry_ms = 250;   // re-check while the previous preparation still runs
    std::string log_path;      // stdout/stderr of the prepare (empty: /dev/null)
    Spawner spawn;             // empty: idle_spawner(log_path)
    /// Counter that changes whenever a setting a launch depends on is written (default: `settings_generation`). A change
    /// forgets what was prepared and prepares the current target again, once the running preparation (if any) ended.
    std::function<unsigned()> generation;
  };
  LaunchPrep();
  explicit LaunchPrep(Config c);
  ~LaunchPrep();
  LaunchPrep(const LaunchPrep&) = delete;
  LaunchPrep& operator=(const LaunchPrep&) = delete;

  /// The command a launch would run now for the item under the cursor ("" = none). Same as before: no-op. Different:
  /// arms the dwell timer (a preparation already running is NOT killed; the next one waits for it).
  void set_target(std::string_view cmd, TimePoint now);
  /// When `poll` must be called next; nullopt when nothing is pending.
  std::optional<TimePoint> next_deadline() const;
  /// Spawns the preparation once the dwell time is over (and the previous one ended); reaps finished children.
  void poll(TimePoint now);
  /// The game is being launched: forget the target (it is prepared again after the game if the cursor rests), raise
  /// the priority of a still running preparation.
  void launched();
  bool running() const { return pid_ > 0; }
  /// Number of preparations spawned so far (tests, stats).
  unsigned spawned() const { return spawned_; }

 private:
  void reap();
  /// Settings written since the last look: the work done so far no longer matches what a launch would do.
  void check_generation(TimePoint now);
  Config cfg_;
  Spawner spawn_;
  std::string target_, last_;
  TimePoint due_{};
  bool waiting_ = false;
  int pid_ = 0;
  unsigned spawned_ = 0;
  unsigned gen_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------
/// What `execute` needs from the caller.
struct Request {
  std::string cmd;
  std::string rom_raw, name;  // for game-start
};
struct Env {
  /// Free audio, joysticks and the window (SDL) just before the game runs / bring them back after.
  std::function<void()> release, restore;
  LaunchPrep* prep = nullptr;
  /// Script dirs; empty = `default_script_dirs()`.
  std::vector<std::string> script_dirs;
};
struct Outcome {
  RunResult run;
  AfterLaunch after;
  /// Text for the error box ("" if none).
  std::string message;
};
/// One whole launch: prep.launched -> game-start (async) -> release -> run -> game-end (async) -> restore. Bookkeeping that
/// touches disk is NOT done here (see `apply_after`) so the caller can redraw first.
Outcome execute(const Request& r, const Env& env);
/// playcount, lastplayed and gametime of game `game` of system `sys` (only when exit was 0): the in-memory game at once,
/// gamelist.xml on the job thread (`game_played`, gamemeta.hpp). Nothing is reloaded from disk.
void apply_after(Ctx& cx, size_t sys, size_t game, const Outcome& o);
/// Toast text for a failed launch ("AN ERROR OCCURRED: <first line of the message>", translated and cut to one line),
/// or "" when there was no error to show. Exit codes 200..300 only; `runemu.sh` itself exits 0 or 1, so this rarely fires.
std::string error_toast(const Outcome& o);

}  // namespace launch
}  // namespace zf
