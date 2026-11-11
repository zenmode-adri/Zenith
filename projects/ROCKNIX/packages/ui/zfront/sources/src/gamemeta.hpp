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
#include <vector>

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

/// Counter that changes whenever the games of any system were edited, scraped or removed. A list on screen (`ListScreen`) compares it
/// with the value it saw and rebuilds its names, so a rename, a hidden game or a deleted one shows at once. Main thread only.
unsigned games_generation();
void games_changed();

/// What ES's metadata editor (GuiMetaDataEd) shows of a game. `rating` is in tenths (0..10: five stars in half steps), `releasedate` is
/// `YYYY-MM-DD` or empty.
struct GameForm {
  std::string name, desc, genre, players, developer, publisher, releasedate;
  std::string sortname;  // ES "Sort name" (`<sortname>`)
  int rating = 0;
  bool favorite = false, hidden = false;
  bool kidgame = false;  // ES "Kidgame" (`<kidgame>`)
  bool operator==(const GameForm& o) const {
    return name == o.name && desc == o.desc && genre == o.genre && players == o.players && developer == o.developer && publisher == o.publisher &&
           releasedate == o.releasedate && rating == o.rating && favorite == o.favorite && hidden == o.hidden && sortname == o.sortname && kidgame == o.kidgame;
  }
  bool operator!=(const GameForm& o) const { return !(*this == o); }
};
/// `YYYY-MM-DD` with a real month and day (or empty: no date).
bool valid_release_date(const std::string& d);
/// The form of the game `path` of system `sys`, read from its gamelist element (what is in memory for the light fields). False when
/// the game is not in the system.
bool game_form_load(const Ctx& cx, size_t sys, const std::string& path, GameForm& out);
/// Saves what differs between `before` and `after` for the game `path`: memory at once (the list shows it), the gamelist on the job
/// thread. Only the changed fields are written. A hidden game leaves the list (`games_changed`).
void game_edit(Ctx& cx, size_t sys, const std::string& path, const GameForm& before, const GameForm& after);

enum class DeleteResult { Ok, Refused };
/// ES's DELETE GAME: removes the ROM file and the media files the gamelist names (only files inside the ROM folder), the game from the
/// custom collections and its `<game>` element. The game leaves `System::games` at once, the files and the gamelist go on the job
/// thread. Nothing at all happens (`Refused`, `why` says what) when the ROM path does not look right: empty or root ROM folder, ROM not
/// inside the ROM folder (also through `..` or a symlinked folder), or a directory (ES deletes those recursively; zfront does not).
/// A ROM file that is already gone is not an error: the entry is cleaned up.
DeleteResult game_delete(Ctx& cx, size_t sys, const std::string& path, std::string& why);

}  // namespace zf
