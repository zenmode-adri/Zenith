// MODULE `gamelist`. `gamelist.xml` metadata: streaming reading and in-place updates. Same format as ES: `<game>`
// elements under `<gameList>`, paths `./x` relative to the system ROM folder.
//
// Design (from the Rust prototype `zf/gamelist`, with a streaming reader instead of a DOM):
//  * The reader never builds a tree and never keeps the file: it reads 128 KB blocks (grown only for one huge
//    element), hands every `<game>` to a callback as string_views valid only during the call, and forgets it.
//  * `GameMeta` is COMPACT (64 B, no heap): flags, rating, year, playcount, lastplayed, interned genre/players, and
//    `span` = byte range of the `<game>` element. `desc`, `image` and `thumbnail` are NOT kept: `gamelist_details`
//    seeks to the span when a game is selected and verifies that it is still that game.
//  * Updates (`update_after_play`, `set_favorite`) edit by byte ranges and rewrite the file by block copy into a
//    temporary file (fsync + rename in the same directory); a broken XML is never touched.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zf {

struct GameMeta {
  /// Interned (process lifetime) so equal values share storage: 16 B instead of a std::string each.
  std::string_view genre, players;
  uint64_t lastplayed = 0;  // YYYYMMDDHHMMSS, 0 = never
  uint32_t playcount = 0;
  int32_t release_year = -1;  // -1: unknown
  uint32_t span_begin = 0, span_end = 0;  // byte range of the `<game>` element in the file (0,0 = none)
  uint8_t rating_pct = 0;     // 0..100, 0 = not rated
  bool favorite = false;
  bool hidden = false;
  uint32_t gametime = 0;  // seconds played (`<gametime>`)

  float rating() const { return float(rating_pct) / 100.0f; }
  /// `lastplayed` back in ES format `YYYYMMDDTHHMMSS` (empty if never).
  std::string lastplayed_iso() const;
};

static_assert(sizeof(GameMeta) <= 64, "GameMeta is kept at one cache line per game");

/// Heavy fields, read on demand for the selected game only.
struct GameDetails {
  std::string desc;
  std::string image;      // absolute path or empty
  std::string thumbnail;  // absolute path or empty
};

/// Equal strings share one copy for the whole process (genre/players have a handful of distinct values).
std::string_view gamelist_intern(std::string_view s);

/// ES `resolveRelativePath`: `./x` -> `<rom_dir>/x`, `~/x` -> `$HOME/x`, absolute stays, anything else is relative.
std::string resolve_rel_path(std::string_view p, const std::string& rom_dir);

/// One `<game>` as the streaming reader sees it. Views are valid only inside the callback. `path` is the RAW text
/// of `<path>` (entities decoded, not resolved): use `resolve_rel_path` or compare against `./...` yourself.
struct GameEntry {
  std::string_view path, name, sortname;
  GameMeta meta;
  uint64_t begin = 0, end = 0;  // byte range of the element (meta.span_* is the same, 0 if it does not fit 32 bits)
};

/// Callback reference (no allocation, no <functional>).
struct EntryFn {
  void* ctx;
  void (*fn)(void*, const GameEntry&);
  template <class F>
  static EntryFn of(F& f) {
    return {&f, [](void* c, const GameEntry& e) { (*static_cast<F*>(c))(e); }};
  }
};

struct GamelistInfo {
  bool opened = false;       // file could be opened (always true for text)
  bool ok = false;           // reached the end of `<gameList>`: the structure is complete
  bool saw_markup = false;   // at least one '<' (false: empty or blank file)
  bool root_selfclosing = false;  // `<gameList/>`
  uint64_t root_begin = 0, root_end = 0;  // byte range of the `<gameList ...>` / `<gameList/>` tag
  uint64_t root_close_begin = 0;  // offset of `</gameList>`
  size_t games = 0;          // `<game>` elements delivered (with a `<path>`)
  size_t skipped = 0;        // `<game>` elements too big to read (> 16 MiB) that were jumped over: NOT delivered, but still in the
                             // file. Readers may carry on (`ok` stays true); writers must not edit such a file (see gamelist.cpp)
};

/// Streams every `<game>` of the file / text through `cb` in document order (`<folder>` and unknown elements are
/// skipped). Never fails: a truncated or malformed file delivers the games before the error and `ok` is false.
GamelistInfo gamelist_scan_file(const std::string& path, EntryFn cb);
GamelistInfo gamelist_scan_text(std::string_view xml, EntryFn cb);

/// `desc`, `image` and `thumbnail` of one game: reads only its byte range (`meta.span_*`) and checks that it is
/// still `rom`. If the file changed (stale span) it falls back to one streaming search. Paths are resolved
/// against `rom_dir`.
GameDetails gamelist_details(const std::string& xml_path, const std::string& rom_dir, const std::string& rom, const GameMeta& meta);

/// Byte range of the file that an update replaced (`old_len` bytes at `begin` became `new_len`): lets the caller
/// shift the spans of the games already loaded (`System::apply_edit`).
struct GamelistEdit {
  uint64_t begin = 0, old_len = 0, new_len = 0;
};

/// After a successful run: playcount + 1, lastplayed = `now_iso` (`YYYYMMDDTHHMMSS`, local time) and, when `add_secs`
/// is not 0, gametime + `add_secs` (as ES: the game's `<gametime>` in seconds). Creates the game / file if missing; a
/// malformed file is left untouched and the call fails.
bool gamelist_update_after_play(const std::string& xml_path, const std::string& rom_dir, const std::string& rom, const std::string& now_iso, std::string& err,
                                GamelistEdit* edit = nullptr, uint64_t add_secs = 0);
bool gamelist_set_favorite(const std::string& xml_path, const std::string& rom_dir, const std::string& rom, bool fav, std::string& err, GamelistEdit* edit = nullptr);

/// One game of a batch edit (`gamelist_edit_fields`): the plain-text children to set and, optionally, the `<scrap>` stamp.
struct GameFieldEdit {
  std::string rom;                                         // absolute ROM path (what `resolve_rel_path` gives for its `<path>`)
  std::vector<std::pair<std::string, std::string>> fields;  // (tag, text): replaces the child (last occurrence) or appends it
  std::string scraper, scrape_date;                        // non-empty: `<scrap name=scraper date=scrape_date/>` set; other scrapers' stamps stay
};
struct GamelistBatchResult {
  size_t edited = 0, created = 0, failed = 0;
};
/// Applies all `edits` in ONE rewrite of the file (block copy to a sibling temporary + fsync + rename). Everything that is not
/// replaced stays byte for byte (unknown children and attributes included); a game that is not in the file is appended; a
/// missing/blank file becomes a new document; a malformed file is left untouched and the call fails. Two edits of the same
/// ROM: the last one wins. A game whose element cannot be edited safely counts in `failed` and is left as it was.
bool gamelist_edit_fields(const std::string& xml_path, const std::string& rom_dir, const std::vector<GameFieldEdit>& edits, std::string& err,
                          GamelistBatchResult* res = nullptr);

}  // namespace zf
