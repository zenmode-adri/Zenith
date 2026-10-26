// MODULE `menu-collections` (data layer). Everything ES keeps about collections, in the same files, so zfront and ES can be
// swapped without losing anything:
//   * `es_settings.cfg`: CollectionSystemsAuto / CollectionSystemsCustom (comma lists), HiddenSystems (`;` list), SortSystems,
//     StartupSystem, StartupOnGameList, UseCustomCollectionsSystemEx, CollectionShowSystemInfo, HiddenSystemsShowGames,
//     LoadEmptySystems (CollectionSystemManager.cpp, GuiCollectionSystemsOptions.cpp, Settings.cpp).
//   * `<ES user dir>/collections/custom-<name>.cfg`: editable collections, one game per line, `./roms/gbc/X.gbc` (relative to
//     /storage), `#` lines and lines starting with `0` ignored. `<name>.xcc`: dynamic collection (XML filter).
//   * `favorite` of gamelist.xml: written by gamelist.* (`game_set_favorite`: block copy + fsync + rename on a job).
//
// Pure functions and small file operations, no UI. Files are written with write_atomic (temp + rename), so a power cut never
// leaves half a collection; ES writes them in place.
//
// The carousel of zfront does not build collection systems yet (that is the system loader's job): `enabled_auto`,
// `enabled_custom` and `members` are what it needs, with ES's exact reading rules (see `enabled_auto`).
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "app.hpp"

namespace zf::collections {

// ---- where the files live ---------------------------------------------------------------------------------------------
struct Paths {
  /// `/storage/.config/emulationstation/collections` (ES: Paths::getUserEmulationStationPath() + "/collections").
  std::string dir;
  /// Base of the `./` paths inside custom-*.cfg (ES: Paths::getRootPath() = /storage on ROCKNIX).
  std::string root;
};
/// Defaults above; `ZF_COLLECTIONS_DIR` / `ZF_COLLECTIONS_ROOT` override them once (harness and tests point them at temp dirs).
Paths& paths();

// ---- lists in es_settings.cfg -----------------------------------------------------------------------------------------
/// ES `Utils::String::split(s, sep, true)`: empty tokens dropped, NOT trimmed.
std::vector<std::string> split_list(std::string_view s, char sep);
/// ES `vectorToCommaString`: sorted, joined with ",".
std::string join_sorted(std::vector<std::string> v);

/// Automatic collection as ES declares it (`CollectionSystemManager::getSystemDecls`).
struct AutoDecl {
  std::string name;          // key stored in CollectionSystemsAuto ("favorites", "_action"...)
  std::string long_name;     // English long name (shown through tr())
  std::string theme_folder;  // theme folder of the carousel entry ("auto-favorites")
  bool genre = false;        // "PER GENRE" group
};
/// The fixed ones (all, recent, favorites, 2players, 4players, neverplayed, retroachievements, arcade, vertical, lightgun, wheel,
/// trackball, spinner) followed by the per-genre ones (the top level genres of ES's genres.xml minus lightgun/wheel/trackball/
/// spinner). Arcade-by-manufacturer collections ("z<name>") are not listed: they exist only for arcade systems with games.
const std::vector<AutoDecl>& auto_decls();
/// Text shown for it: the ES catalog's translation of the long name with a capital, or the genre name in Spanish/English.
std::string label(const AutoDecl& d);

/// ES reading rules: the string is split on ',' WITHOUT trimming, so ES's own default "favorites, recent" enables only
/// "favorites" (" recent" never matches). zfront mirrors that exactly; once the user touches the menu the saved list is clean.
std::vector<std::string> enabled_auto(const EsSettings& es);
std::vector<std::string> enabled_custom(const EsSettings& es);
/// HiddenSystems (`;` separated system names).
std::vector<std::string> hidden_systems(const EsSettings& es);

/// `vec` changed to include/exclude `name`; keeps entries the menu does not know (arcade manufacturers...).
void set_member(std::vector<std::string>& vec, const std::string& name, bool on);

// ---- custom collections -------------------------------------------------------------------------------------------------
struct Custom {
  std::string name;
  bool dynamic = false;  // `<name>.xcc` (filter) instead of `custom-<name>.cfg`
};
/// Reads the folder (one `opendir`), sorted by name like ES's std::map. Missing folder: empty.
std::vector<Custom> list_custom();
std::string cfg_path(const std::string& name);  // <dir>/custom-<name>.cfg
std::string xcc_path(const std::string& name);  // <dir>/<name>.xcc

/// ES `getValidNewCollectionName`: keeps only [A-Za-z0-9-[]() ], "New Collection" when nothing is left, and appends " (n)"
/// while the result is in `taken` (system names, theme folders of automatic/custom collections, reserved ids).
std::string valid_new_name(std::string_view wanted, const std::vector<std::string>& taken);
/// Names an ES install already uses: auto collection ids + theme folders, "collections", the existing custom ones.
std::vector<std::string> reserved_names();

/// Creates an empty editable collection file. False (and `err`) when it cannot be written.
bool create_editable(const std::string& name, std::string& err);

/// What a dynamic collection filters on (the part of ES's GuiGamelistFilter that zfront ports): system names (empty: all),
/// text contained in the game name (upper case is how ES stores it) and the favourites flag.
struct Filter {
  std::vector<std::string> systems;
  std::string text;
  bool only_favorites = false;
};
/// `<filter name=".."><system>..</system><text>..</text><favorites>TRUE</favorites></filter>` (FileFilterIndex.cpp).
std::string filter_xml(const std::string& name, const Filter& f);
bool create_dynamic(const std::string& name, const Filter& f, std::string& err);
/// Reads back the parts of `<name>.xcc` zfront edits. False when the file is missing or not XML.
bool read_filter(const std::string& name, Filter& f);
/// Changes only those three parts of an existing `<name>.xcc` (genre, players, year... filters made in ES are kept); creates the
/// file when it does not exist.
bool write_filter(const std::string& name, const Filter& f, std::string& err);

/// Deletes `custom-<name>.cfg` / `<name>.xcc` and takes the name out of CollectionSystemsCustom (ES `deleteCustomCollection`).
/// The settings file is NOT saved here.
bool remove_custom(const std::string& name, EsSettings& es, std::string& err);

/// "./roms/gbc/X.gbc" for "/storage/roms/gbc/X.gbc" (ES `createRelativePath`, the part that matters on the console).
std::string to_key(const std::string& abs_path);
/// Inverse: "./x" -> root/x, "~/x" -> $HOME/x, other kept.
std::string from_key(const std::string& key);

/// Absolute paths of the games of an editable collection, in file order (blank, `#` and `0...` lines skipped).
std::vector<std::string> members(const std::string& name);
bool contains(const std::string& name, const std::string& abs_path);
/// Adds / removes one game (read, change, write atomically). `changed` tells whether the file was modified.
bool add_game(const std::string& name, const std::string& abs_path, std::string& err, bool* changed = nullptr);
bool remove_game(const std::string& name, const std::string& abs_path, std::string& err, bool* changed = nullptr);

// ---- favourites ----------------------------------------------------------------------------------------------------------
/// The list key: flips `favorite` of game `game` of system `sys` (memory at once, gamelist.xml on a job, undone with a toast if
/// the write fails) and shows ES's notification ("Added 'X' to 'FAVORITES'" / "Removed ..."). Returns false for an index out
/// of range.
bool toggle_favorite(Ctx& cx, size_t sys, size_t game);

/// `%s` placeholders replaced in order (ES messages are gettext format strings).
std::string fmt(const char* format, const std::string& a, const std::string& b = {});

// ---- state shared with the system loader --------------------------------------------------------------------------------
/// A menu changed something that the carousel must rebuild (systems shown, collections, sort...). The loop owner reads and
/// clears it when the menus close (ES's `reloadSystems` / `reloadAll` flags).
bool reload_needed();
void set_reload_needed(bool v);

}  // namespace zf::collections
