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
#include "status.hpp"
#include "theme.hpp"

namespace zf {

struct SystemItem {
  /// Value of `{system:theme}`: picks `assets/logos/<theme>.webp`.
  std::string theme_name;
  std::string full_name;
  uint32_t games = 0;
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

/// `cam` is the (possibly fractional) carousel position; `cursor_abs` the unwrapped selected item.
void system_view(const Theme& th, Assets& a, const Common& c, float w, float h, const std::vector<SystemItem>& items, int cursor_abs, float cam,
                 const Status& status, std::vector<Op>& ops);

/// Game list (theme `basic, detailed` textlist): header with avatar and system name, up to `listLines` rows with the
/// pill selector, the selected game's column (name only, or `detail` through `detailed_column`), status and help bars.
void list_view(const Theme& th, Assets& a, const Common& c, float w, float h, const std::string& system_name, const std::vector<std::string>& names,
               size_t cursor, const Status& status, const DetailedData* detail, std::vector<Op>& ops);

}  // namespace zf
