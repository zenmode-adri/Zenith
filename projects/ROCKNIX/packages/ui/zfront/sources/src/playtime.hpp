// PLAY TIME LIMIT (for children): a limit of minutes of play per DAY, `zfront.playlimit` in system.cfg (Off / 15 / 30 / 45 / 60 / 90 / 120,
// default Off), set in GAME SETTINGS (a screen Kid and Kiosk mode cannot open, like DATE & TIME, so a child can neither raise the limit nor
// move the date forward to start a new day).
//
// What it does, while the limit is on:
//   * zfront counts the time of every game it launches (the time between starting it and its end, on the steady clock: a clock that
//     jumps or a suspended console adds and takes nothing);
//   * 5 minutes before the end it shows a message INSIDE the game when it can (RetroArch only, see `device_hooks`), and at the end it
//     closes the game by the same path as QUIT GAME / the phone (`oscontract::emukill`);
//   * with the time spent, a launch shows "Time is up for today" and does not start;
//   * INFORMATION shows TIME LEFT TODAY.
// The total of the day is kept in /storage/.config/zfront/playtime (`ZF_PLAYTIME_FILE` for tests), written atomically (temporary file +
// rename) at most once a minute and when a game ends; a restart in the middle of a game loses less than a minute.
//
// The date is only the KEY of the day. A new day starts when the local date is valid (year 2024..2099) and LATER than the stored one.
// A date that goes back, or is invalid (a console without battery clock boots in 1970 or 2000), changes nothing: the time spent is kept
// and no time is given; only a stored day more than 1 day AHEAD of a valid today is replaced by today (the total is kept), so a date
// moved far forward by mistake does not block the days in between. Not covered: a date moved forward on purpose in a mode where the date can be changed.
//
// Everything in this header except `local_today`, `store` and `device_hooks` is pure (injected clock, no screen).
#pragma once
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "settings.hpp"

namespace zf {
struct System;
}

namespace zf::playtime {

constexpr const char* kKey = "zfront.playlimit";
/// The warning comes when this much is left.
constexpr long kWarnMs = 5L * 60 * 1000;
constexpr int kFirstValidYear = 2024, kLastValidYear = 2099;
/// Time a game is given to start before the watcher acts on it.
constexpr long kStartGraceMs = 20 * 1000;

/// Minutes of the setting; 0 = Off. Only a whole number from 1 to 1440 counts, anything else is Off (never "unlimited by mistake" the
/// other way: a bad value does not lock the console).
int limit_minutes(std::string_view setting);

struct Date {
  int year = 0, month = 0, day = 0;
  /// A real date: 2024..2099, month 1..12, day 1..31.
  bool valid() const;
  /// "YYYY-MM-DD" (sorts like the date).
  std::string key() const;
};
/// The local date now (year 0 when the clock cannot be read).
Date local_today();

struct Events {
  bool warn = false;     // just got to 5 minutes left (once)
  bool expired = false;  // just got to no time left (once)
};

/// The day's total.
class Tracker {
 public:
  /// "" / garbage / out of range = a clean tracker (nothing played, no day).
  static Tracker parse(std::string_view text);
  std::string text() const;

  /// See the header: only a valid, later date starts a new day (the total goes to 0); the first valid date adopts the day it finds.
  void set_date(const Date& today);
  /// Adds `ms` of play (negative = 0) and tells what `limit_min` makes of it. Off (0) gives no events. It counts with the limit Off too:
  /// the callers only call it when a limit is on, so what is counted starts when the limit is turned on.
  Events add(long ms, int limit_min);

  long used_ms() const { return used_ms_; }
  /// Milliseconds left under `limit_min` (never below 0); -1 when Off.
  long left_ms(int limit_min) const;
  bool exhausted(int limit_min) const { return limit_min > 0 && left_ms(limit_min) <= 0; }
  const std::string& day() const { return day_; }
  /// Changed since the last `Saver` write.
  bool dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }

 private:
  std::string day_;  // "YYYY-MM-DD" or empty
  long used_ms_ = 0;
  bool warned_ = false, expired_ = false, dirty_ = false;
};

/// A tracker from the file ("" when missing, empty or corrupt: a clean one).
Tracker load_file(const std::string& path);

/// Writes the tracker atomically at most once per `interval_ms` (the first change at once).
class Saver {
 public:
  explicit Saver(std::string path, long interval_ms = 60 * 1000) : path_(std::move(path)), interval_(interval_ms) {}
  /// Writes when the tracker is dirty and the interval has passed (or `force`). True when it wrote. A failed write leaves it dirty.
  bool tick(Tracker& t, long now_ms, bool force = false);
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  long interval_;
  long last_ = 0;
  bool saved_ = false;
};

/// What a game in progress needs from the clock: the pure part of the watcher.
class Session {
 public:
  Session(Tracker& t, Saver& s, int limit_min, long start_ms) : t_(t), saver_(s), limit_(limit_min), last_(start_ms) {}
  struct Step {
    Events ev;
    long wake_ms = 0;  // when to call `advance` again, from now
  };
  /// The steady clock `now_ms` and the local date: adds what was played since the last call (a step that goes back adds nothing), saves
  /// when due, and says when to look again (the warning point, the end, or a minute; every 10 s once there is no time left, to close the
  /// game again if it is still there). With the limit Off nothing is counted or written.
  Step advance(long now_ms, const Date& today);
  /// The game ended: the last step and a forced save.
  void finish(long now_ms, const Date& today);

 private:
  Tracker& t_;
  Saver& saver_;
  int limit_;
  long last_;
};

struct Hooks {
  std::function<void(long left_ms)> warn;  // 5 minutes left (or less, when the game started late): how much is left
  std::function<void(int attempt)> expire;  // no time left: close the game (0 the first time, then every 10 s while it runs)
};

/// Runs a `Session` on its own thread for the length of a game: it sleeps on a condition variable until the next point (no polling),
/// so the main thread can stay blocked waiting for the game. `stop` (or the destructor) wakes it, does the last step and saves.
class Watcher {
 public:
  /// `grace_ms`: nothing is looked at (no hook runs) until the game has had this long to start. `runemu.sh` needs a few seconds before the
  /// emulator exists, and a message or a kill sent before that goes nowhere. The time still counts from the start.
  Watcher(Tracker& t, Saver& s, int limit_min, Hooks h, std::function<Date()> today = local_today, long grace_ms = kStartGraceMs);
  ~Watcher();
  Watcher(const Watcher&) = delete;
  Watcher& operator=(const Watcher&) = delete;
  void start();
  void stop();

 private:
  void loop();
  Tracker& t_;
  Saver& saver_;
  int limit_;
  Hooks hooks_;
  std::function<Date()> today_;
  long grace_ms_;
  std::mutex m_;
  std::condition_variable cv_;
  std::thread th_;
  bool stop_ = false, running_ = false;
};

// ---- the device ---------------------------------------------------------------------------------------------------------------------

/// The path of the file (`ZF_PLAYTIME_FILE` or /storage/.config/zfront/playtime).
std::string file_path();
/// The one tracker of the process, loaded from the file on first use. Only the main thread touches it, and only while no `Watcher` runs.
struct Store {
  Tracker tracker;
  Saver saver;
};
Store& store();
/// Drops the loaded store (tests).
void reset_store();
/// `zfront.playlimit` of the settings in minutes (0 = Off).
int limit_of(const SystemConf& conf);
/// A launch must not start: the limit is on and the day's time is spent (looks at today's date first).
bool launch_blocked(const SystemConf& conf);
/// Does this launch count against the limit (and get blocked by it)? Games do, PortMaster ports included (system `ports`, platform `pc`).
/// Tools (system `tools`, platform `tools`: the scripts of /storage/.config/modules, the R36 Tuner, the Tools menu), the music player and
/// the image viewer do not: the same rule as `doctor::is_game_system`. `sys` is null when the launch is not of a listed system.
bool counts_as_game(const System* sys);
/// Minutes left today, rounded up; -1 when Off.
int minutes_left(const SystemConf& conf);
/// "TIME LEFT TODAY" value: "25 min"; empty when Off.
std::string left_text(const SystemConf& conf);
/// The hooks of the device: the warning is RetroArch's own on-screen message (UDP command SHOW_MSG, only when its network commands are on);
/// the end closes the game with `oscontract::emukill` (SIGTERM: RetroArch then quits through its normal shutdown, saving as on any quit).
Hooks device_hooks();
/// A watcher for the game that is about to start; null when the limit is Off. The caller keeps it until the game ends (`stop()` or reset).
std::unique_ptr<Watcher> begin_game(const SystemConf& conf);

}  // namespace zf::playtime
