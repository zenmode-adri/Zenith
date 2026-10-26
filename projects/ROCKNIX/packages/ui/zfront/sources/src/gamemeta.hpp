// Edits of one game's metadata (played, favourite) that must never block the UI thread.
//
// `gamelist.xml` is rewritten by block copy + fsync + rename (up to ~25 ms on the A35 for a 40 MB file, much more on a
// busy SD card), so the file work runs on `cx.sv->jobs` and only the in-memory game is touched on the main thread:
//   1. main thread, at once: the `Game::meta` of the edited game changes (playcount, lastplayed, gametime or favourite),
//      so whatever is drawn next already shows the new value;
//   2. job thread: `gamelist_update_after_play` / `gamelist_set_favorite` on values captured BY VALUE (never a reference
//      into `Ctx`);
//   3. main thread, in the job's `done`: `System::apply_edit` shifts the byte spans of the other games so
//      `gamelist_details` keeps seeking to the right place. Jobs are FIFO, so consecutive edits compose.
// The edited game is looked up again BY PATH in `done` (the vector may have been reloaded meanwhile). A failed write
// shows a toast; a failed favourite also puts the flag back.
//
// ES reloads every gamelist from disk after a successful run (`reloadAllGames`). zfront does not: it already holds the
// values it wrote, and a reload costs a full re-parse and loses the cursor of the list. Anything else that edits the
// gamelist behind our back (a scraper run from a menu, a tool over SSH) is not seen until the system is opened again
// after a restart; a feature that needs it must reload the system AND rebuild `ListScreen`'s copy of the names.
#pragma once
#include <cstdint>
#include <ctime>
#include <string>

#include "gamelist.hpp"

namespace zf {

struct Ctx;

/// `YYYYMMDDTHHMMSS` in local time (ES `lastplayed` format).
std::string iso_local(time_t t);
/// `YYYYMMDDHHMMSS` as the number `GameMeta::lastplayed` holds.
uint64_t pack_lastplayed(const std::string& iso);

/// A game ran and exited with 0: playcount + 1, lastplayed = `now` (0: the current time), gametime + `secs`.
/// `sys`/`game` index `cx.systems` / its `games`. No-op for indices out of range.
void game_played(Ctx& cx, size_t sys, size_t game, uint64_t secs, time_t now = 0);
/// Sets or clears the favourite flag of a game.
void game_set_favorite(Ctx& cx, size_t sys, size_t game, bool fav);

}  // namespace zf
