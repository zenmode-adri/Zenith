// Play statistics. ES keeps per game only playcount / gametime / lastplayed (gamelist.xml); this adds a small log of the sessions
// (`playlog.tsv`, one line per finished game) so "this week" and the weeks before can be told, and the pure arithmetic that turns the
// gamelists + the log into the numbers of GAMES > PLAY STATISTICS (play_stats_screen.cpp). Nothing here draws.
#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "systems.hpp"

namespace zf::playstats {

/// Sessions shorter than this are not logged (launch failures, a game that closed at once).
constexpr uint64_t kMinSecs = 10;
/// When the log grows past this many bytes the older half goes.
constexpr size_t kMaxLogBytes = 512 * 1024;
constexpr int kWeeks = 5;  // this week + the 4 before

/// `/storage/.config/zfront/playlog.tsv`; `ZF_PLAYLOG` replaces it (tests).
std::string log_path();

/// `YYYY-MM-DDTHH:MM:SS` (local time) TAB seconds TAB system TAB rom path [TAB battery drop], with a closing line break. Tabs and line
/// breaks inside the names become spaces. The optional last column is the charge the session used in tenths of a percent of a full
/// battery (`drop_pct10 >= 0`; absent for older lines, sessions on the charger and short ones): readers that stop at the rom path ignore it.
std::string format_line(time_t start, uint64_t secs, const std::string& system, const std::string& rom, int drop_pct10 = -1);

/// Appends one session to `path` (O_APPEND, one write). False and nothing written when `secs < kMinSecs` or the file cannot be written.
/// Trims the file first when it is over `kMaxLogBytes` (keeps the newest half, through a temp file).
bool record(const std::string& path, time_t start, uint64_t secs, const std::string& system, const std::string& rom, int drop_pct10 = -1);
/// `record` into `log_path()` for a game that ended at `end` (0: now); the start is `end - secs`.
bool record_ended(const std::string& system, const std::string& rom, uint64_t secs, time_t end = 0, int drop_pct10 = -1);

struct LogEntry {
  time_t start = 0;
  uint64_t secs = 0;
  std::string system, rom;
  int drop_pct10 = -1;  // battery used, tenths of a percent of a full charge; -1: not measured
};
/// Every well formed line of the log (bad lines are skipped).
std::vector<LogEntry> parse_log(const std::string& text);

/// What a gamelist says about one game.
struct GameTime {
  std::string name, system;  // `system` is the display name
  uint64_t secs = 0;
  uint32_t plays = 0;
};
/// The games with some gametime or plays of every system. A system whose games are not loaded is read from its gamelist for this call only.
std::vector<GameTime> collect(const std::vector<System>& systems);

struct Stats {
  uint64_t total_secs = 0;  // all games, from the gamelists
  uint64_t total_plays = 0;
  uint64_t week_secs[kWeeks] = {};  // [0] = the last 7 days, [1] = the 7 days before that...
  std::vector<GameTime> top;        // at most 10, most time first
  std::vector<std::pair<std::string, uint64_t>> systems;  // display name, secs; most first
  bool empty() const;
};
/// Pure: the numbers from the gamelists (`games`), the log and the current time.
Stats compute(const std::vector<GameTime>& games, const std::vector<LogEntry>& log, time_t now);

/// "12 h 05 min" / "45 min" / "<1 min" (0 s: "0 min"). Rounded to the nearest minute.
std::string format_duration(uint64_t secs);

}  // namespace zf::playstats
