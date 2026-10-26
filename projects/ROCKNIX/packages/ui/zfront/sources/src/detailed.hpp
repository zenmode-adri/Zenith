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
  std::string desc;
  int32_t year = -1;
  std::string genre, players;
  /// 0..1 like gamelist.xml; 0 means "not rated" (five empty stars).
  float rating = 0;

  /// Seconds since the description was shown (drives the scroll); < 0 = not shown yet.
  double desc_time = -1;
  /// Output of `detailed_column`: pixels the description can scroll (0 = it fits: no animation).
  mutable int desc_max_scroll = 0;

  /// true: a cover that is not cached yet is decoded inside `detailed_column` (harness, tests, `--bench`). false (the
  /// device loop): it is left out and `cover_missing` is set, and the caller asks `covers::request` for
  /// `image` at `cover_w x cover_h` (decoded on the job thread, covers.hpp).
  bool decode_inline = true;
  mutable bool cover_missing = false;
  mutable uint32_t cover_w = 0, cover_h = 0;
};

// ES constants (TextComponent.cpp): seconds before scrolling, seconds per pixel, seconds at the end before going back.
constexpr double kDescDelay = 6.0, kDescStep = 0.150, kDescHold = 6.0;

/// Pixel offset of the description `t` seconds after it was shown (`max_off` = how far it can scroll).
int desc_scroll_offset(double t, int max_off);
/// Seconds until `desc_scroll_offset` changes (> 0); infinite when `max_off <= 0`.
double desc_scroll_next(double t, int max_off);

/// True: ListScreen waits 120 ms before decoding the cover/description and draws the column.
bool detailed_enabled();
/// `--demo list`: answer with sample data (the Zelda of the reference capture) instead of the gamelist.
void detailed_set_demo(bool on);
/// Data for game `idx` of `sys` (the system is open). false if nothing to show.
bool detailed_for(const System& sys, size_t idx, bool settled, DetailedData& out);
/// Appends the column's operations (cover, name, meta rows, stars, description).
void detailed_column(const Theme& th, Assets& a, float w, float h, const DetailedData& d, std::vector<Op>& ops);

}  // namespace zf
