// IN-GAME HOME MENU (experimental, off by default: `zfront.homemenu=1` in system.cfg, GAMES menu switch).
//
// Like the Home button of a console: while a game runs, a button chord held for one second pauses the emulator (SIGSTOP) and
// shows a zfront panel on top (battery, brightness, volume, SAVE STATE / LOAD STATE, RESUME, QUIT GAME). Closing it resumes the
// game where it was (SIGCONT).
//
// How it fits (there is NO window and NO event loop of ours while a game runs: `launch::run` waits for the game and SDL was shut
// down by `Env::release` so the emulator can own the display):
//   * `launch::run` calls `Env::supervise` every ~100 ms instead of blocking in waitpid (only when the setting is on: with it off
//     the old blocking path runs unchanged);
//   * the supervisor (sdlr.cpp) owns a `PadWatcher` on the evdev nodes SDL knew (read-only, non-exclusive: the emulator and
//     input_sense keep reading the same nodes) and, when the chord is held long enough, runs `Session::trigger`;
//   * the panel is NOT drawn in this process: it is `zfront --home-panel ...`, a small child with its own window (own Wayland
//     app_id `zfront-home`, so the `for_window [app_id="emulationstation"] reload` rule of the sway config does not fire) whose
//     exit code is the answer. A crash or a hang of the panel can therefore never leave the game stopped: the parent resumes it.
//
// Chord: the default is START + R1 held 1 s. Select-based chords are taken: RetroArch's hotkey button is Select (`input_enable_hotkey_btn`
// = 8 = Select in retrogame_joypad.cfg/r36s_Gamepad.cfg) with L1 = load state, R1 = save state, L2 = rewind, R2 = fast forward, X = menu,
// Y = fps, Start = exit; and input_sense uses L1 as its modifier A (L1+Select+Start kills the emulator, L1+face buttons = screenshot...).
// RetroArch ignores every other chord while `input_enable_hotkey` is set, and input_sense only reacts to chords with L1 or Select, so
// START + R1/R2/L2 are free of both. (Standalone emulators other than RetroArch were NOT checked.)
//
// Fail safe, in this order: every path out of `Session::trigger` ends with SIGCONT to everything it stopped; the stopped pids are
// written to a file before the first SIGSTOP and resumed at the next start of zfront if we died; SIGTERM / SIGSEGV / SIGABRT
// while stopped resume them from the handler.
#pragma once
#include <poll.h>

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "settings.hpp"
#include "util.hpp"

namespace zf::homemenu {

constexpr const char* kKeyEnable = "zfront.homemenu";   // "1" = on (default off)
constexpr const char* kKeyCombo = "zfront.homemenu.combo";  // "r1" (default), "r2", "l2": the shoulder pressed together with START
constexpr int kDefaultHoldMs = 1000;

// evdev key codes (linux/input-event-codes.h), the ones input_sense matches by name (BTN_TL, BTN_SELECT, BTN_START...)
constexpr int kBtnTR = 0x137, kBtnTL2 = 0x138, kBtnTR2 = 0x139, kBtnStart = 0x13b;

enum class Combo { StartR1, StartR2, StartL2 };
/// "r1" / "r2" / "l2" (anything else: the default, START + R1).
Combo combo_from_setting(std::string_view v);
std::vector<int> combo_codes(Combo c);

// ---- chord detection (pure) --------------------------------------------------------------------------------------------------------
/// Time-based: evdev sends nothing while a button stays down, so the hold completes at a deadline, not at an event.
class ComboDetector {
 public:
  explicit ComboDetector(std::vector<int> codes, int hold_ms = kDefaultHoldMs);
  /// One EV_KEY event: `value` 0 up, 1 down, 2 autorepeat (ignored). Codes outside the chord are ignored.
  void key(int code, int value, TimePoint now);
  /// SYN_DROPPED: the kernel lost events, so what we think is held may be wrong. Forget everything (a button still held is not
  /// seen again until it is released and pressed).
  void dropped();
  /// True ONCE per hold, when every button of the chord has been down for `hold_ms`. It re-arms when a chord button is released.
  bool poll(TimePoint now);
  /// When `poll` will turn true if nothing changes; nullopt when the chord is not complete or already fired.
  std::optional<TimePoint> deadline() const;
  /// A chord button is down.
  bool any_held() const { return !held_.empty(); }

 private:
  std::vector<int> codes_;
  int hold_ms_;
  std::set<int> held_;
  bool complete_ = false, fired_ = false;
  TimePoint since_{};
};

/// Feeds `bytes` of raw `struct input_event` records to `d` (the number of whole records read from an evdev node).
void feed_events(const void* buf, size_t bytes, ComboDetector& d, TimePoint now);

/// Non-exclusive reader of the pad evdev nodes (O_RDONLY|O_NONBLOCK: input_sense and the emulator keep reading them).
class PadWatcher {
 public:
  explicit PadWatcher(ComboDetector d) : det_(std::move(d)) {}
  ~PadWatcher();
  PadWatcher(const PadWatcher&) = delete;
  PadWatcher& operator=(const PadWatcher&) = delete;
  /// Opens the nodes; unreadable ones are skipped. Returns how many opened.
  size_t open(const std::vector<std::string>& paths);
  /// Takes ownership of a descriptor (tests).
  void add_fd(int fd) { fds_.push_back(fd); }
  size_t count() const { return fds_.size(); }
  /// Sleeps up to `max_ms` (shorter when the hold completes first, `poll(2)` on the nodes in between). True when the chord fired.
  bool wait(int max_ms);
  const ComboDetector& detector() const { return det_; }
  /// Waits until no chord button is down, at most `max_ms`; true if they are all up.
  bool wait_released(int max_ms);

 private:
  ComboDetector det_;
  std::vector<int> fds_;
};

// ---- the pause / panel / resume sequence (injectable) ----------------------------------------------------------------------------
enum class PanelResult { Resume, Quit, SaveState, LoadState, Failed };
/// Exit codes of `zfront --home-panel` (anything else, or a signal, is `Failed`).
constexpr int kExitResume = 0, kExitQuit = 20, kExitSave = 21, kExitLoad = 22;
PanelResult panel_result_from_exit(int wait_status);

enum class Outcome {
  Nothing,       // no process to pause: nothing was touched
  Resumed,       // paused, panel closed (or failed), resumed
  Quit,          // resumed, then the game was asked to end
  StateSaved,    // resumed, then SAVE_STATE sent (the command itself is fire-and-forget)
  StateLoaded,   // resumed, then LOAD_STATE sent
  PauseFailed    // a SIGSTOP failed: everything already stopped was resumed, the panel was not shown
};

struct Io {
  std::function<int(int pid, int sig)> kill;                 // ::kill
  std::function<std::vector<int>()> find_pids;               // the emulator process(es)
  std::function<void(const std::vector<int>&)> record;       // persist the stopped pids ({} = none)
  std::function<PanelResult(const std::vector<int>&)> panel; // shows the panel; returns when it is closed
  std::function<bool()> term_requested;                      // a SIGTERM arrived meanwhile
  std::function<void()> quit_game;                           // the existing emukill path
  std::function<void(const char* cmd)> send_state;           // "SAVE_STATE" / "LOAD_STATE" to RetroArch (after the resume)
  std::function<void(const std::vector<int>&)> refocus;      // give the focus back to the game window (best effort)
  std::function<void()> wait_buttons_up;                     // bounded wait for the chord to be released before resuming
};
/// The whole sequence. Whatever happens it ends with SIGCONT to every pid it stopped.
Outcome trigger(const Io& io);

/// The setting is on (default off) / the chosen chord.
bool enabled(const SystemConf& conf);
Combo combo(const SystemConf& conf);
/// The Io of the device: kill(2), the process names of the kill target, the `--home-panel` child, `emukill`, RetroArch's UDP commands
/// (only when its network_cmd_enable is on), `swaymsg [pid=N] focus`, and a bounded wait for the chord to be released on `w`.
Io real_io(PadWatcher& w);
/// RetroArch's runtime config (the file that wins over the source .cfg of the image).
constexpr const char* kRetroarchCfg = "/storage/.config/retroarch/retroarch.cfg";

// ---- helpers --------------------------------------------------------------------------------------------------------------------
/// Pids whose /proc/<pid>/comm is one of `names` (comm is cut at 15 characters), excluding `self`. `proc_root` is "/proc" (tests).
std::vector<int> find_pids_by_name(const std::vector<std::string>& names, int self, const std::string& proc_root = "/proc");
/// `network_cmd_enable` and `network_cmd_port` of a retroarch.cfg text.
struct NetCmd {
  bool enabled = false;
  int port = 55355;
};
NetCmd parse_retroarch_net(std::string_view cfg_text);
/// UDP datagram to 127.0.0.1:port. False when it could not be sent.
bool send_udp_local(int port, std::string_view text);
/// A name of the kill target (oscontract::kill_names) is a RetroArch binary.
bool is_retroarch(const std::vector<std::string>& names);

// pids stopped by us, for the signal handlers and the recovery after a crash
/// Marks `pids` as stopped (signal handlers resume them) / clears.
void set_stopped(const std::vector<int>& pids);
/// SIGTERM / SIGSEGV / SIGABRT handlers that resume the stopped pids; installed by `install_safety`, removed by `remove_safety`.
void install_safety();
void remove_safety();
/// Writes / reads the pid file (`/run/zfront/home-stopped`). `recover_stopped` sends SIGCONT to what a dead zfront left in it and
/// removes the file; call it at start. Returns how many were resumed.
void write_stopped_file(const std::string& path, const std::vector<int>& pids);
size_t recover_stopped(const std::string& path, const std::function<int(int, int)>& kill_fn);
const char* stopped_file_path();

// ---- the child panel -------------------------------------------------------------------------------------------------------------
/// Command line of the panel child: `exe` + fixed arguments + `--home-panel [--home-states]`.
void set_panel_args(std::string exe, std::vector<std::string> args);
bool panel_args_set();
/// Runs `zfront --home-panel` and waits for it (polling `term_requested` meanwhile; kills it then). Failed when it cannot start.
PanelResult run_panel_child(bool with_states, const std::function<bool()>& term_requested);
/// In the child: what `menu.home` answers (read by main.cpp for the exit code).
void set_panel_result(PanelResult r);
/// True in the `--home-panel` process (the loop must not announce itself as the frontend: no ready file, no `start` event).
void set_panel_mode(bool on);
bool panel_mode();
PanelResult panel_result();
int exit_code_of(PanelResult r);

}  // namespace zf::homemenu
