// THEME CONFIGURATION (ES: GuiMenu::openThemeConfiguration, shown by USER INTERFACE SETTINGS when the theme `hasSubsets()`), the screen
// `menu.ui.theme`, and ES's per-theme memory of the choices (GuiMenu::saveSubsetSettings / loadSubsetSettings).
//
// A theme's `<subset>`s (aspect ratio, colour scheme, icon set...) are read from the ES theme the ThemeSet names (`estheme::list_subsets`, on
// the job thread, once per theme and screen size). The screen lists them in ES's order and groups with ES's keys:
//   colorset / iconset / menu / systemview / gamelistview / region   ->  ThemeColorSet / ThemeIconSet / ThemeMenu / ThemeSystemView /
//                                                                         ThemeGamelistView / ThemeRegionName  (es_settings.cfg)
//   any other subset                                                  ->  subset.<name>
// The default option (the first of the subset) is stored as an empty value. Like ES, the choices apply when the screen is left (one reload for
// all of them): zfront reads its theme once, so the frontend restarts, as it does for THEME SET. Only the subsets are here: zfront draws its own
// views, so a subset changes what it takes from the ES theme (colours, font, logos), not the layout.
//
// Zenith (zfront's own theme) declares no subsets: the row is hidden, as ES hides it for a theme without them.
//
// `themesettings/<ThemeSet>.cfg` (under ES's user folder): the choices of a theme set, written when another theme set is picked and read back
// when this one is picked again. One `name=value` per line, lines ended by `\r`; `colorset`, `iconset`, `menu`, `systemview`, `gamelistview` and
// `region` are the bare names of the Theme* keys, the others are the `subset.*` keys themselves, plus GamelistViewStyle, DefaultGridSize and the
// `<system>.defaultView` / `<system>.gridSize` of each system.
#pragma once
#include <string>
#include <vector>

#include "es_theme.hpp"
#include "menu.hpp"

namespace zf::themecfg {

/// ES setting of a subset: the Theme* name of the six special ones, `subset.<name>` for the rest.
std::string setting_key(const std::string& subset);

/// One row of THEME CONFIGURATION.
struct Row {
  std::string subset, key, label;
  std::vector<Option> options;  // value = option name, label = its displayName
  std::string def;              // the first option: stored as ""
  bool applies = false;         // ES lists these under GAMELIST THEME OPTIONS (`appliesTo` with a title), the others under THEME OPTIONS
};
/// The rows ES shows for these subsets (a subset without options, or titled "-" with a single option, has none), in its order: subsets
/// without `appliesTo` first, then the others.
std::vector<Row> rows_for(const std::vector<estheme::SubsetInfo>& infos);
/// Whether zfront honors `subset` for a theme it redraws itself (`estheme::remade_style`: "iconic", "xmb", "artbook", "techdweeb"). For those
/// themes the choices are only worth offering when drawing them changes something: Iconic honors COLOR SCHEME, PLAY VIDEO PREVIEWS and
/// SHOW DETAILED GAME INFO; the others none yet (they keep their own colours and faces). An empty style (any other theme, Zenith) honors all.
bool honored(const std::string& style, const std::string& subset);
/// `rows` without the ones `honored` refuses for `style`.
std::vector<Row> honored_rows(std::vector<Row> rows, const std::string& style);
/// The option the row shows: the stored one when it is an option of the subset, else the default.
std::string current_value(const EsSettings& es, const Row& r);
/// What is stored for the choice `chosen`: "" for the default.
std::string stored_value(const Row& r, const std::string& chosen);

/// Where the per-theme files are (`<user dir>/themesettings`, re-rootable with `mg::env()`).
std::string settings_dir();
/// ES saveSubsetSettings: writes (or removes, when there is nothing to remember) the file of `theme_set` from the current settings.
void save_for(const Ctx& cx, const std::string& theme_set);
/// ES loadSubsetSettings: puts the choices of `theme_set`'s file into the settings (nothing when it has none). Does not save.
void load_for(Ctx& cx, const std::string& theme_set);
/// ES: the theme keys of the settings go back to "" (the lines that exist).
void clear_all(Ctx& cx);
/// ES THEME SET change: remembers the choices of the current set, stores `new_set`, clears the choices, loads those of `new_set`, tells the
/// scripts (`theme-changed <new> <old>`). The caller saves the settings and restarts.
void switch_theme(Ctx& cx, const std::string& new_set);

/// Whether the active ES theme has subsets: true once they are known (read on the job thread the first time; `refresh` is called when they
/// arrive, so a menu built before that shows the row when it rebuilds). False for a theme zfront has not translated (Zenith) or without subsets.
bool available(Ctx& cx, const Refresh& refresh);

}  // namespace zf::themecfg
