// MODULE `detailed` (owner: detailed agent). Right column of the game list (theme views `basic, detailed`): cover,
// name, released / genre / players / rating rows and the description. Geometry comes from the theme variables
// (`colX colW coverW coverH listY nameY nameH valX valW meta1..4 metaH starY starH descY descH descVisible fName fMeta
// fDesc lblW`), all fractions of the screen, so it follows `screen.ratio`.
//
// Contract: `scene::list_view(..., const DetailedData* detail)` calls `detailed_column` when `detail` is not null and
// draws the plain game name when it is null. `ListScreen` (screens.cpp) asks `detailed_for` for the selected game;
// the 120 ms settle delay before decoding the cover/description is applied there: while the cursor is moving
// `settled == false` and `detailed_for` returns data WITHOUT image and description.
//
// Layout decisions (where ES would print "Unknown"):
//  - released / genre / players without data are omitted and the rows below move up one slot; the description moves up
//    by the freed height and grows to keep its bottom edge;
//  - the rating row is always there: no rating = five empty stars (user decision, like ES);
//  - the name and the cover keep their theme positions (a missing cover leaves its area empty, like ES).
//
// Description (ES `TextComponent` with `autoScroll=vertical`, ES constants): the text is wrapped in full and shown
// through a window of the theme's `descH`, so the last visible line is cut in half (no ellipsis). If it does not fit it
// waits 6 s, scrolls 1 px every 150 ms, waits 6 s at the end and jumps back to the top. The scroll is a pure function
// of the time since the description was shown (`desc_scroll_offset`): nothing is polled, the screen asks
// `desc_scroll_next` how long it may sleep.
//
// Genre (ES `TextComponent` with `autoScroll=horizontal`, the theme's 2 s): a genre wider than its box is cut at the edge,
// waits, moves left and comes round again after a gap, the same pure function of the time (`genre_scroll_offset`). Each
// pixel is a frame (about a quarter of a core against 0 % at rest), so unlike ES it stops at its start after
// `kGenreLoops` rounds until the card is shown again; one that fits sets no timer.
//
// Performance: everything is cached in `Assets` by key (text by string+size+colour, the wrapped description by its
// full parameters, the cover by path+box); the gamelist details of the last 8 games are cached here.
#pragma once
#include <string>
#include <vector>

#include "gfx.hpp"
#include "systems.hpp"
#include "theme.hpp"

namespace zf {

struct DetailedData {
  std::string name;
  /// Upper-case `name`, filled by `detailed_for` (`detailed_column` computes it when empty).
  std::string name_up;
  std::string image;  // absolute path or empty
  std::string video;  // absolute path or empty (list preview; only once the cursor settled)
  std::string desc;
  int32_t year = -1;
  std::string genre, players;
  /// 0..1 like gamelist.xml; 0 means "not rated" (five empty stars).
  float rating = 0;
  bool show_rating = true;  // false: a screenshot, which nobody rates

  /// Seconds since the description was shown (drives the scroll); < 0 = not shown yet.
  double desc_time = -1;
  /// Output of `detailed_column`: pixels the description can scroll (0 = it fits: no animation).
  mutable int desc_max_scroll = 0;
  /// Output of `detailed_column`: width of a genre that does not fit and its speed in px/s (0 = it fits: no animation).
  mutable float genre_w = 0, genre_speed = 0;

  /// true: a cover that is not cached yet is decoded inside `detailed_column` (harness, tests, `--bench`). false (the
  /// device loop): it is left out and `cover_missing` is set, and the caller asks `covers::request` for
  /// `image` at `cover_w x cover_h` (decoded on the job thread, covers.hpp).
  bool decode_inline = true;
  mutable bool cover_missing = false;
  mutable uint32_t cover_w = 0, cover_h = 0;
};

// ES constants (TextComponent.cpp): seconds before scrolling, seconds per pixel, seconds at the end before going back.
constexpr double kDescDelay = 6.0, kDescStep = 0.150, kDescHold = 6.0;
// Genre: seconds before it first moves, seconds still at each start of the loop, seconds of gap before it comes round.
constexpr double kGenreDelay = 2.0, kGenreLoopDelay = 1.0, kGenreGap = 1.5;
constexpr int kGenreLoops = 3;

/// Pixel offset of the description `t` seconds after it was shown (`max_off` = how far it can scroll).
int desc_scroll_offset(double t, int max_off);
/// Seconds until `desc_scroll_offset` changes (> 0); infinite when `max_off <= 0`.
double desc_scroll_next(double t, int max_off);
/// Pixels the genre has moved `t` seconds after it was shown (`w` = its width, `speed` in px/s).
int genre_scroll_offset(double t, float w, float speed);
/// Seconds until `genre_scroll_offset` changes (> 0); infinite when `w <= 0` or once it has stopped.
double genre_scroll_next(double t, float w, float speed);

/// True: ListScreen waits 120 ms before decoding the cover/description and draws the column.
/// The picture of a game of the list (from the gamelist, cached); "" when it has none.
std::string detailed_image(const System& sys, size_t idx);
/// Same for a game that is not in `sys.games` (a collection's copy): `sys` is the system the game belongs to (its gamelist and ROM folder).
std::string detailed_image(const System& sys, const Game& g);
bool detailed_enabled();
/// Forgets the cached gamelist details (description, picture paths) of the games seen; they are read again when a game is
/// shown. Called when a game starts, to give the memory back.
void detailed_cache_clear();
size_t detailed_cache_size();
/// `--demo list`: answer with sample data (the Zelda of the reference capture) instead of the gamelist.
void detailed_set_demo(bool on);
/// Data for game `idx` of `sys` (the system is open). false if nothing to show.
bool detailed_for(const System& sys, size_t idx, bool settled, DetailedData& out);
/// Same for a game that is not in `sys.games` (see `detailed_image`).
bool detailed_for(const System& sys, const Game& g, bool settled, DetailedData& out);
/// Game list of Zenith Minimal on the grid of cells (remake::Grid): how many of the 12 columns the list takes (the theme's `listW`, to whole
/// columns); the column of the selected game takes the rest.
int detail_list_cols(const Theme& th, float w, float h);

/// Appends the column's operations (cover, name, meta rows, stars, description).
void detailed_column(const Theme& th, Assets& a, float w, float h, const DetailedData& d, std::vector<Op>& ops);

}  // namespace zf
