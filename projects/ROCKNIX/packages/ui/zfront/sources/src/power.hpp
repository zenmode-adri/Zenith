// Battery: what the console can say right now, a small log of how the charge goes down, and the estimate of what used it.
//
// The RK817 gauge gives capacity, status, voltage_avg, current_avg (smoothed, negative = discharging), charge_now and charge_full; it has no sensor per part of the
// console. So, like Android, "what used the most" is an estimate: the charge lost during each game is measured (the frontend logs it when the game starts and ends);
// the screen comes from its brightness and the time it was on; what is left while the frontend is up is "menus and system"; a jump between the two clocks is the
// time asleep. The log is one line per event in `$ZF_BATTERY_LOG` or /storage/.config/zfront/battery.log, a few lines an hour, and it trims itself.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace zf {
struct Ctx;
}

namespace zf::power {

/// What the backlight uses at 50 % brightness: measured on the R36 Ultra with the screen on and off in the menu (doc 26); other screens will differ.
inline constexpr float kDefaultScreenMw = 417.0f;

struct Now {
  bool have = false;  // the console has a battery
  int percent = 0;
  bool charging = false;
  float volts = 0, amps = 0, watts = 0;  // amps > 0 while discharging; 0 when the driver does not say
  long charge_uah = 0, full_uah = 0;
  int brightness_pct = -1;
};
/// Reads the driver under `$ZF_BATTERY_ROOT` (tests, harness) or /sys.
Now read_now();

struct Row {
  enum Kind { Game, Screen, Menus, Asleep };
  Kind kind = Game;
  std::string name;  // of a game
  float pct = 0;     // share of the charge used
  long secs = 0;
};
struct Summary {
  bool valid = false;     // something has been measured since the last full charge
  std::vector<Row> rows;  // biggest first
  std::vector<std::pair<float, int>> curve;  // (hours before the last entry, percent), oldest first
  float span_hours = 0;
  long awake_secs = 0;
  int used_pct = 0;
};
/// From the text of the log. Pure, for the tests. The screen share (`screen_mw_50` at 50 % brightness) is taken out of what the games and menus used.
Summary summarize(const std::string& log, float screen_mw_50 = kDefaultScreenMw);
inline constexpr float kNoHours = -1.0f;
/// Hours left at the present rate, `kNoHours` while charging or when the driver does not say.
float hours_left(const Now& n);

std::string log_path();
void log_event(char kind, const std::string& name = {});
Summary summary_now();

/// The battery a game session used. Valid only on the battery at both ends, for a session of at least `kMinSessionSecs`, with a positive drop.
inline constexpr long kMinSessionSecs = 300;
struct Session {
  bool valid = false;
  int drop_pct10 = 0;  // tenths of a percent of a full battery (from the charge counter when the driver has it, else from the percentage)
};
/// Pure: the session between the readings `a` (start) and `b` (end), `secs` long.
Session make_session(const Now& a, const Now& b, uint64_t secs);
/// The session of the game that just ended (readings taken in game_started / game_ended); the next call returns an invalid one.
Session take_session(uint64_t secs);

/// A game starts / ended (the frontend keeps waiting meanwhile, so these two lines are what measures it).
void game_started(const std::string& name);
void game_ended();
/// Called from the loop whenever the status is polled: a line every few minutes, and the automatic power saver.
void tick(Ctx& cx);

/// Power saver: on-demand clocks without the performance mode of the games, lower brightness, screensaver sooner. Going back restores what the user had.
bool saver_on(Ctx& cx);
void set_saver(Ctx& cx, bool on, bool automatic = false);
/// Percentage at which it comes on by itself (0 = never).
int saver_auto(Ctx& cx);
void set_saver_auto(Ctx& cx, int pct);

}  // namespace zf::power
