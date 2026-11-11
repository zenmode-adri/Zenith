// MODULE `colsys`: the enabled collections as carousel systems, as ES shows them.
//
//   * Automatic (ES's own): ALL GAMES, LAST PLAYED (the 50 most recent, newest first), FAVORITES, NEVER PLAYED; zfront's smart ones
//     (smartcol.hpp): RECENTLY ADDED, UNFINISHED, MOST PLAYED THIS WEEK. The other ES automatic ones (2 players, 4 players, genres,
//     arcade, lightgun... and RetroAchievements) are NOT built: they need metadata the light games do not keep.
//   * Custom: every enabled `custom-<name>.cfg` (editable) and `<name>.xcc` (dynamic filter: systems, text, favourites).
//   Order in the carousel (ES's CollectionSystemManager::updateSystemsList): the real systems, then the custom collections by name, then
//   the automatic ones by key (all, favorites, neverplayed, recent), then the smart ones in menu order. (ES sorts the custom ones into
//   the systems when SortSystems is alphabetical; here they always follow them.) An empty collection is not shown.
//
// Representation: a collection is a `System` with `collection = true` kept in `Ctx::cols`, NEVER in `Ctx::systems`, so every loop over the
// real systems (play statistics, library doctor, web remote, scraper, netplay...) is unaffected. Its card index is `systems.size() + j`
// (`Ctx::sys_at`). Its games are copies of the source games (name, path, metadata) plus `Game::src_system` / `src_idx`, so a list can be drawn
// without opening anything; every ACTION (launch, favourite, edit, play count) is resolved with `resolve` to the real (system, game) first.
//
// Building reads every gamelist: it runs on the job thread (`App::refresh_collections`) from a copy of the systems, and the result is
// applied only while the carousel is the only screen (the cursor is kept by name). While a collection list is open, `resync` keeps its rows
// in step with the source games (new metadata, a game that stopped qualifying or was deleted).
#pragma once
#include <ctime>
#include <string>
#include <vector>

#include "app.hpp"
#include "playstats.hpp"
#include "smartcol.hpp"

namespace zf::colsys {

constexpr size_t kLastPlayedLimit = 50;  // ES LAST PLAYED

/// True when the settings enable at least one collection this module can build (automatic, smart or custom).
bool any_enabled(const EsSettings& es);
/// True when MOST PLAYED THIS WEEK is on (the only one that needs the play log).
bool needs_log(const EsSettings& es);

/// Reads the games of every system of `snap` that was not loaded (`!loaded && games.empty()`), as `System::open` does. Job thread.
void fill(std::vector<System>& snap);

/// The collection systems for `snap` (every system's games loaded: `fill`), ordered as above, empty ones left out. `cols` are `collection`
/// systems with `loaded = true`, `count` set and `theme` = the theme folder of the collection. Pure apart from reading the custom
/// collection files and, for RECENTLY ADDED, the ROM files' times. Kid mode keeps only kid games; hidden games stay out.
/// Leaves only the systems that hold games (no screenshots viewer, tools or music player): the collections are built from these.
void keep_game_systems(std::vector<System>& snap);
std::vector<System> build(const std::vector<System>& snap, const EsSettings& es, const std::vector<playstats::LogEntry>& log, const smartcol::Options& o);

/// Lists keep the order the collection was built in (LAST PLAYED, smart ones) instead of favourites first.
bool keeps_order(const std::string& name);

/// A game of a list resolved to the real system: `sys` indexes `Ctx::systems`, `game` its `games`.
struct Real {
  size_t sys = 0, game = 0;
};
/// `sys` / `game` as shown in a list. An ordinary system gives itself; a collection gives the source (opening that system's gamelist when
/// it is not read yet). False when the game is gone (the source system left the list, the ROM was removed).
/// `open = false`: false as well when the source system is not read yet (idle-time work must not read a gamelist on the UI thread).
bool resolve(Ctx& cx, size_t sys, size_t game, Real& out, bool open = true);
/// The real system a collection game belongs to, found by name WITHOUT opening it (only its `gamelist` / `rom_dir` / `theme` are wanted);
/// null for an ordinary game or an unknown system.
const System* source_system(const Ctx& cx, const Game& g);

/// Collection `sys` (an index like `ListScreen::si_`): copies the current metadata of every game from its source (only sources already
/// read can have changed), drops the games that no longer qualify (FAVORITES without the flag, NEVER PLAYED once played, hidden, deleted)
/// and refreshes the card count. No-op for an ordinary system.
void resync(Ctx& cx, size_t sys);

/// Something that changes the members happened (a favourite, a play, an edit, a deleted game): `App` rebuilds when the carousel is back.
void request();
bool requested();
void clear_request();

}  // namespace zf::colsys
