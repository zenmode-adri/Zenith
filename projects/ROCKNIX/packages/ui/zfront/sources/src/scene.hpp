// Zenith views as lists of draw operations: the system carousel (`system_view`) and the game list (`list_view`),
// plus the pieces every screen shares (status bar, help bar). Geometry comes from the theme variables (fractions of
// the screen) and ES `CarouselComponent::renderCarousel` (checked against a real console capture).
//
// To add a view: write a function here (or in a new scene_<x>.cpp declared in this header) that returns ops through
// `std::vector<Op>& out`, using `status_bar` and `help_bar` for the common parts.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "detailed.hpp"
#include "gfx.hpp"
#include "recent.hpp"
#include "status.hpp"
#include "theme.hpp"

namespace zf {

/// `SystemItem::sys` value meaning "the system at the same position in `Ctx::systems`" (the harness and the tests build items 1:1).
constexpr size_t kItemIsSystem = size_t(-1);

struct SystemItem {
  /// Value of `{system:theme}`: picks `assets/logos/<theme>.webp`.
  std::string theme_name;
  std::string full_name;
  uint32_t games = 0;
  /// Index in `Ctx::systems` of the system this card shows. The carousel leaves out HiddenSystems and may be sorted, while
  /// `Ctx::systems` keeps every system in es_systems.cfg order (the menus list them all): use `Ctx::system_of`.
  size_t sys = kItemIsSystem;
};

/// Theme images the scenes keep across frames (pinned in the cache).
struct Common {
  ImgId tile = 0, frame = 0, avatar = 0;
};

bool load_common(const Theme& th, Assets& a, Common& out, std::string& err);

/// (icon name in `assets/help-<icon>.png`, text)
using HelpPrompts = std::vector<std::pair<const char*, const char*>>;
/// The help bars of the carousel and of the game list: texts in capitals in the system language (`system.language`).
const HelpPrompts& system_help();
const HelpPrompts& list_help();

void status_bar(const Theme& th, Assets& a, float w, float h, const Status& st, std::vector<Op>& ops);
void help_bar(const Theme& th, Assets& a, float w, float h, const HelpPrompts& prompts, std::vector<Op>& ops);

/// The "continue playing" row under the carousel. `items` is what to show; `system_view` sets `box_w x box_h` to the size the
/// screenshots must be decoded at (the caller asks `covers::request`), or 0 when the row did not fit on this screen.
struct RecentRow {
  std::vector<RecentItem> items;
  uint32_t box_w = 0, box_h = 0;
  bool decode_inline = false;  // harness and tests: decode in `system_view` (no job thread)
  /// Tile that carries the focus ring (index into `items`, -1 = none) and how strongly the ring shows (0..1: it fades in and out).
  int focus = -1;
  float focus_a = 1.0f;
  /// The row has the keys (CarouselScreen): the help bar names the keys of the row instead of the carousel's.
  bool focus_help = false;
};

/// Indices into `r.items` of the tiles the row shows when the carousel rests on `cur` on a screen of `w` x `h`: none when the row does
/// not fit or the count of the system is still pending, the first `fit` ones that fit side by side without the screenshots that
/// failed to decode. The same geometry as `system_view` draws with, for the screen that needs it between two frames (keys).
std::vector<size_t> recent_visible(const Theme& th, Assets& a, float w, float h, const SystemItem& cur, const RecentRow& r);
/// Help bar while the row has the focus.
const HelpPrompts& recent_help();

/// What the text under the carousel (name, count, hairline, "continue playing") shows: it fades out, changes system and fades in
/// while the carousel moves. `item` = index of the system it shows (-1: the one under the cursor), `alpha` = 0..1.
struct InfoFade {
  int item = -1;
  float alpha = 1.0f;
};

/// Positions (unwrapped carousel indices) `system_view` builds for a camera at `cam` with `n` systems: `first..last`. Every tile
/// that can be on screen is in it, also the half-visible one that is about to slide in from the edge: it must already exist, not
/// appear when the camera crosses an integer.
void carousel_span(float cam, int n, int& first, int& last);

/// `cam` is the (possibly fractional) carousel position; `cursor_abs` the unwrapped selected item.
void system_view(const Theme& th, Assets& a, const Common& c, float w, float h, const std::vector<SystemItem>& items, int cursor_abs, float cam,
                 const Status& status, RecentRow* recent, const InfoFade& info, std::vector<Op>& ops);

/// Game list (theme `basic, detailed` textlist): header with avatar and system name, up to `listLines` rows with the
/// pill selector, the selected game's column (name only, or `detail` through `detailed_column`), status and help bars.
void list_view(const Theme& th, Assets& a, const Common& c, float w, float h, const std::string& system_name, const std::vector<std::string>& names,
               size_t cursor, const Status& status, const DetailedData* detail, const std::vector<std::string>* marks, std::vector<Op>& ops);

}  // namespace zf
