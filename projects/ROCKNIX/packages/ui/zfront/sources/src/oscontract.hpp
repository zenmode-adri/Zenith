// MODULE `oscontract` (os-contract): what the rest of the OS expects from "emulationstation", so zfront can replace it
// without breaking sway rules, sleep.sh, autostart, the tuner... See cpp/docs/os-contract.md (contract + OS patches) and
// /home/epsil/zf-research/02-contrato-so-es.md (the specification).
//
//   startup / wait_*    boot: stale-state cleanup, bl_power=0, set_kill, SIGHUP, bounded waits for sway and the ROMs
//   on_first_frame      /tmp/emulationstation.ready + the `start` event
//   fire_event / quit   ES scripts (`<dir>/<event>/*.sh`), quit menu actions (.please files, shutdown -r / -P -h now)
//   idle / screensaver  inactivity timer (ScreenSaverTime), screensaver-start/stop, bl_power 4/0, swallows the waking key
//   game / busy state   thread-safe, answered by the HTTP thread while the main thread is blocked running a game
//   external requests   HTTP `POST /launch` reaches the device loop through `take_external`
//
// Dry-run has two layers: `startup(sv, dry_run=true)` makes every file write (bl_power, ready file, kill data, .please files)
// and every script event only a line in `dry_log()`; system commands (shutdown, systemctl suspend, killall) always go through
// `sv->cmd`, which has its own dry-run flag and log. With no `startup` the module never runs a system command (its fallback
// runner is dry). Paths are injectable (`paths()`) for the tests.
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app.hpp"
#include "services.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace zf {
namespace oscontract {

struct Paths {
  std::string backlight_dir = "/sys/class/backlight";      // <dir>/*/bl_power
  std::string ready_file = "/tmp/emulationstation.ready";
  std::string kill_file = "/tmp/.process-kill-data";       // `set_kill` of /etc/profile.d/001-functions
  std::string tmp_dir = "/tmp";                            // restart.please / reboot.please / shutdown.please
  std::string suspend_helper = "/usr/sbin/pm-suspend";     // ES only offers suspend when this exists (see suspend_supported)
};
Paths& paths();

// ---- lifecycle ------------------------------------------------------------------------------------------------------
/// Main thread, once, before the heavy work (device loop only). Binds the services (command runner), installs the SIGHUP
/// handler, writes bl_power=0 (a panel left off by a dead screensaver), `set_kill "emulationstation"`, starts the idle timer.
void startup(std::shared_ptr<Services> sv, bool dry_run);
/// After the loop returned and the window is gone: runs the `on_finish` hooks (the HTTP server stops there), lights the panel,
/// removes the ready file, then performs the pending quit mode (`quit` event, .please files, shutdown). Exit code stays 0.
void finish();
void on_finish(std::function<void()> hook);
/// Waits (inotify on the parent directory, no polling) until `path` exists (and is a socket if `want_socket`), at most
/// `timeout_ms`. True when it exists. A parent directory that does not exist yet falls back to 20 ms steps.
bool wait_for_path(const std::string& path, int timeout_ms, bool want_socket = false);
/// `ES_WAIT_WAYLAND_SOCKET` (max 10 s) / `ES_WAIT_ROMS_READY` (max 15 s), exported by start_es.sh. True when not requested or
/// satisfied; on a timeout it logs and goes on (like ES).
bool wait_display();
bool wait_roms();
/// First useful frame: creates the ready file and fires `start`. Idempotent.
void on_first_frame();
void clear_ready();
/// If the executable is not called "emulationstation", sets SDL_VIDEO_WAYLAND_WMCLASS=emulationstation for the next video
/// init (SDL's Wayland app_id is the exe name); `game_started` removes it again so emulators do not inherit it.
void before_video();

// ---- small system facts ---------------------------------------------------------------------------------------------
/// Writes `power` (0 on, 4 off) to every `<backlight_dir>/*/bl_power`; number of files written.
int backlight(int power);
/// `/tmp/.process-kill-data`, same content as `set_kill set <name>` (name + newline).
bool set_kill(const std::string& name);
std::string kill_target();

// ---- quit / suspend ---------------------------------------------------------------------------------------------------
/// ES `QuitMode`. `Quit` is "leave the frontend": exit 0 and systemd (Restart=always) starts it again; it is also what the
/// menu entry "RESTART EMULATIONSTATION" does. `Restart` additionally touches restart.please.
enum class Mode { Quit, Restart, Reboot, Shutdown, FastReboot, FastShutdown };
/// Main thread. Like `Utils::Platform::quitES`: Reboot/Shutdown fire `quit <mode>` (synchronous) and `<mode>` now, remember the
/// mode, and ask the loop to leave (SDL_QUIT). The system command runs in `finish()`, after the window is gone.
void quit(Mode m);
void request_quit();
bool quit_requested();
Mode quit_mode();
/// `systemctl suspend`, the path of the power button (logind runs the sleep hooks). Fire and forget.
bool suspend();
/// ES shows the suspend entry only when pm-suspend exists. Used for ScreenSaverBehavior=suspend.
bool suspend_supported();

// ---- events ---------------------------------------------------------------------------------------------------------
/// Script directories searched by every event: `launch::default_script_dirs()` unless overridden.
void set_script_dirs(std::vector<std::string> dirs);
const std::vector<std::string>& script_dirs();
/// `Scripting::fireEvent`: `<dir>/<event>/*` with the arguments and `<dir>/*` with the event name first. Only `quit` is
/// synchronous (Linux ES). Costs one stat per directory when nobody listens (cached for 5 s for `*-selected`). Returns the
/// command lines started (empty in dry-run: the call is recorded in `dry_log()`).
std::vector<std::string> fire_event(const std::string& event, const std::vector<std::string>& args = {});
/// `game-selected <system> <path> <name>` / `system-selected <system>`: nothing at all (no allocation) unless a script listens; with
/// no `startup` (harness, preview) never. Called by the screens when the cursor settles.
void fire_game_selected(const std::string& system, const std::string& path, const std::string& name);
void fire_system_selected(const std::string& system);
/// True when `game-selected` / `system-selected` has a listener (cached 5 s); true in dry-run, false when unbound. Screens ask before
/// arming any timer, so with no script installed the cursor costs nothing extra.
bool selected_listening(const char* event);

// ---- inactivity / screensaver -----------------------------------------------------------------------------------------
class IdleTimer {
 public:
  using ms = std::chrono::milliseconds;
  /// <= 0 disables.
  void set_timeout(long timeout_ms) { timeout_ = ms(timeout_ms > 0 ? timeout_ms : 0); }
  void input(TimePoint now) {
    last_ = now;
    stamped_ = true;
  }
  bool stamped() const { return stamped_; }
  bool active() const { return active_; }
  void set_active(bool a) { active_ = a; }
  /// When the screensaver must start; nullopt while disabled, already active or never stamped (the loop then sleeps).
  std::optional<TimePoint> deadline() const {
    if (!stamped_ || active_ || timeout_.count() <= 0) return std::nullopt;
    return last_ + timeout_;
  }
  bool due(TimePoint now) const {
    auto d = deadline();
    return d && *d <= now;
  }
  /// Due but not allowed to start (a job or a game is running): look again `delay` from `now` instead of leaving a
  /// deadline in the past (the loop would spin on it).
  void defer(TimePoint now, ms delay) { last_ = now + delay - timeout_; }

 private:
  TimePoint last_{};
  ms timeout_{0};
  bool stamped_ = false, active_ = false;
};
IdleTimer& idle();

/// The loop's next wake-up for the screensaver (reads `ScreenSaverTime`, ms, default 300000; 0 disables).
std::optional<TimePoint> idle_deadline(const EsSettings& es);
/// Device loop, once per iteration BEFORE the actions are handled: starts the screensaver when due; if it is active and the
/// user pressed something, ends it and DROPS the actions (the first key only wakes the panel, as in ES).
/// `raw_input`: a key / pad button went down that maps to no action; it still counts as activity (restarts the count,
/// wakes the screensaver).
/// Also: a suspend since the last call (sleep.sh brought the panel back) ends the screensaver and restarts the count; and the
/// screensaver does not start while a game runs, an `oscontract::Busy` is held or a background job runs (scraper, downloads):
/// it looks again 5 s later.
void before_acts(const EsSettings& es, TimePoint now, std::vector<Action>& acts, bool raw_input = false);
/// True once after the screensaver started or stopped: the loop must redraw (overlay on / off).
bool take_redraw();
/// While the screensaver is on, the only wake-up the loop needs: nullopt for black; for dim with `DrawClock`, the next minute
/// boundary (the clock under the overlay). Replaces EVERY other deadline (status bar, cursor blink, key repeat, toasts).
std::optional<TimePoint> saver_deadline(const EsSettings& es, TimePoint now);
/// A suspend happened since the last `before_acts` (cheap: two clock reads). The loop's sleep returns early when true.
bool resume_pending();
/// Why the screensaver must not start now.
bool saver_blocked();
/// AFTER the actions were handled (a game may have run for hours inside): restart the inactivity count.
void input(TimePoint now);
void screensaver_start(const EsSettings& es);
void screensaver_stop();

// ---- running game / busy (thread-safe) --------------------------------------------------------------------------------
void game_started(const std::string& rom, const std::string& name);
void game_ended();
bool game_running();
/// `{"name":..,"path":..}` of the running game, empty when none.
std::string running_game_json();
/// Long jobs (scraper, installers...) hold one of these so `/isIdle` says false.
class Busy {
 public:
  Busy();
  ~Busy();
  Busy(const Busy&) = delete;
  Busy& operator=(const Busy&) = delete;
};
bool busy();
/// `killall <name in /tmp/.process-kill-data>` when a game runs and the name is a safe process name that is not the
/// frontend itself. False (nothing done) otherwise.
bool emukill();

// ---- external requests (HTTP -> device loop) --------------------------------------------------------------------------
/// Main thread: remember a ROM path to launch (from the `done` callback of a job).
void request_launch(const std::string& rom_path);
/// Device loop, once per iteration: the `Outcome::Launch` for the pending request (cancels the screensaver), if the ROM is
/// known. Only the system whose `rom_dir` holds the path is opened.
std::optional<Outcome> take_external(App& app);
/// The lookup behind `take_external` (tests): command and indexes of the game with exactly this path.
bool resolve_launch(Ctx& cx, const std::string& rom_path, Outcome& out);

// ---- text helpers (tests) ---------------------------------------------------------------------------------------------
std::string json_escape(const std::string& s);
/// Printable single-line text for a toast: control characters become spaces, cut at `max_bytes` on a UTF-8 boundary.
std::string sanitize_notice(const std::string& s, size_t max_bytes = 200);

// ---- test hooks ---------------------------------------------------------------------------------------------------------
std::vector<std::string>& dry_log();
bool dry_run();
/// Installs the SIGHUP handler; `forward_term` makes it also send SIGTERM to ourselves (SDL turns that into SDL_QUIT).
void install_signals(bool forward_term);
void reset_for_test();

}  // namespace oscontract
}  // namespace zf
