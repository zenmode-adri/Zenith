// Battery estimate per game: how long the charge lasts with a given game, from what its past sessions used. The play log
// (playstats.hpp) carries, per session on the battery, the charge used (tenths of a percent of a full battery); this turns the last
// sessions of a game into a rate (% per hour), falling back to the system and then to every game when the game has none.
// The pure arithmetic is here; the two places that show it are GAME OPTIONS (`game_entry`) and PLAY STATISTICS (`top_drainers`).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "playstats.hpp"

namespace zf {
class MenuBuilder;
struct Ctx;
}  // namespace zf

namespace zf::batest {

/// A session counts when it is at least this long and used some charge (the same five minutes the recording asks for).
constexpr uint64_t kMinSecs = 300;
/// How many of the newest sessions are used: of the game, of its system, of all games.
constexpr size_t kGameN = 5, kSystemN = 10, kGlobalN = 20;
/// Each older session weighs this much of the one after it.
constexpr double kDecay = 0.6;

struct Rate {
  enum Source { None, Game, System, Global } source = None;
  double pct_per_hour = 0;  // of a full battery
  size_t sessions = 0;      // how many went in
  bool valid() const { return source != None; }
};

/// Does this log line count (long enough, charge measured and positive)?
bool usable(const playstats::LogEntry& e);
/// The rate of the newest `n` usable sessions among those `pick` accepts (nothing: invalid). Weighted by recency (newest first, `kDecay`
/// per step) and by length, so it is total used over total time with recent sessions counting more. Pure.
Rate rate_of(const std::vector<playstats::LogEntry>& log, size_t n, const std::string& system, const std::string& rom, bool by_system, bool by_game);
/// Game first, then its system, then everything.
Rate estimate(const std::vector<playstats::LogEntry>& log, const std::string& system, const std::string& rom);
/// Hours the charge `percent` lasts at `r`; negative when there is no rate.
double hours_left(const Rate& r, int percent);
/// "~3 h 20 min" (rounded to 5 minutes), "~<1 min", "> 48 h" beyond two days.
std::string format_left(double hours);

struct Drainer {
  std::string system, rom;
  double pct_per_hour = 0;
  size_t sessions = 0;
};
/// The games that used the most per hour (their own sessions only), at most `n`, fastest first. Pure.
std::vector<Drainer> top_drainers(const std::vector<playstats::LogEntry>& log, size_t n = 5);

/// GAME OPTIONS: the BATTERY row of one game (nothing without a battery; "NOT ENOUGH DATA YET" without sessions).
void game_entry(MenuBuilder& m, Ctx& cx, const std::string& system, const std::string& rom_path);

}  // namespace zf::batest
