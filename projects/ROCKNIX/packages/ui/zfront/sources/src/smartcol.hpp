// Smart automatic collections: RECENTLY ADDED, UNFINISHED, MOST PLAYED THIS WEEK, plus the membership rule of ES's own NEVER PLAYED.
// Pure data layer (no UI, no globals besides the file system): the games come from the loaded systems (unloaded ones are read from
// their gamelist for the call, like playstats::collect), the clock and the file times are parameters, so tests are deterministic.
//
// Settings: the three new ones are kept under their own key `ZfCollectionSystemsAuto` (comma list, trimmed) so ES, which rewrites
// `CollectionSystemsAuto` from its own option list, never sees or drops them. NEVER PLAYED keeps its ES key `neverplayed` in
// `CollectionSystemsAuto` (it is one of ES's built-ins, see collections::auto_decls).
//
// `build` returns what each enabled, non-empty collection shows; colsys.hpp turns it (with ES's own automatic and the custom ones) into
// the carousel systems.
#pragma once
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "playstats.hpp"
#include "settings.hpp"
#include "systems.hpp"
#include "uimode.hpp"

namespace zf::smartcol {

struct Decl {
  std::string name;          // key in ZfCollectionSystemsAuto
  std::string long_name;     // English text, shown through tr() (capitalised like collections::label)
  std::string theme_folder;  // theme folder of the carousel entry ("auto-recentlyadded")
};
/// The three new collections, in menu order.
const std::vector<Decl>& decls();
std::string label(const Decl& d);

/// Settings key that holds them.
constexpr const char* kSettingsKey = "ZfCollectionSystemsAuto";
std::vector<std::string> enabled(const EsSettings& es);  // trimmed, empty tokens dropped

constexpr int kAddedDays = 30;  // RECENTLY ADDED window
constexpr int kUnfinishedMaxPlays = 3;
constexpr uint32_t kUnfinishedMaxSecs = 2 * 3600;  // less than 2 h in total
constexpr int kUnfinishedIdleDays = 7;             // last played more than this long ago
constexpr int kWeekDays = 7;                       // same window as playstats week_secs[0]

constexpr size_t kNoGame = size_t(-1);
struct Member {
  size_t sys = 0;         // index in the systems vector
  size_t game = kNoGame;  // index in systems[sys].games; kNoGame when that system is not loaded (use `path`)
  std::string path, name;
  time_t key = 0;  // what the order was made on (file time / last played / seconds in the week), for the UI and tests
};

struct Options {
  time_t now = 0;
  uimode::Mode mode = uimode::Mode::Full;  // Kid: only kid games
  bool show_hidden = false;                // only honoured in Full mode (ES: hidden stay hidden in Kiosk and Kid)
  /// Time a ROM file appeared: max(st_mtime, st_ctime); 0 when it cannot be read. Default: stat(2).
  std::function<time_t(const std::string&)> file_time;
};

/// `YYYYMMDDHHMMSS` (GameMeta::lastplayed, local time) to time_t; 0 for 0 / invalid.
time_t lastplayed_time(uint64_t lp);

/// Members of one collection, ordered: "recentlyadded" (newest file first), "unfinished" (last played newest first),
/// "weekly" (most time this week first), "neverplayed" (name order). Names tie-break everywhere. Unknown name: empty.
/// `log` = the parsed play log (only "weekly" uses it).
std::vector<Member> members(const std::string& name, const std::vector<System>& systems, const std::vector<playstats::LogEntry>& log, const Options& o);

struct Collection {
  std::string name;  // "neverplayed" or a Decl::name
  std::string theme_folder;
  std::string label;
  std::vector<Member> games;
};
/// Enabled collections that have games (all enabled ones with `keep_empty`, ES LoadEmptySystems), in the order: neverplayed first,
/// then the Decl order.
std::vector<Collection> build(const std::vector<System>& systems, const EsSettings& es, const std::vector<playstats::LogEntry>& log, const Options& o,
                              bool keep_empty = false);

}  // namespace zf::smartcol
