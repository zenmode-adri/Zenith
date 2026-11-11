// Command line conveniences that let `start_es.sh` start zfront like it starts ES (docs/os-contract.md, "Linea de ordenes"):
//   * `--theme` may be left out: the theme folder comes from ES's own `ThemeSet` setting (see `resolve_theme`),
//   * `--log-path DIR` (ES's option) is accepted: stderr goes to `DIR/zfront.log`,
//   * with `--fullscreen` and no `--size` the layout ratio follows the real output (sdlr.cpp reloads the theme if it differs).
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace zf {
class Theme;
}

namespace zf::boot {

/// Folder of the merged Zenith theme for ES's `ThemeSet` value (empty when the setting is not there). `root` prefixes every
/// absolute path (tests). zfront has a single theme, Zenith, so a ThemeSet that is not Zenith is shown as Zenith (light unless the name says dark).
///   Zenith set named in ThemeSet -> that folder in the user themes dir, then /usr/share/themes (the OS package builds both
///   `es-theme-zenith` and `es-theme-zenith-dark`), otherwise /usr/share/zfront/theme-{dark,light}, otherwise the OS package.
/// "dark" in the name -> dark; anything else, no setting included, -> light (the image ships `ThemeSet=es-theme-zenith`, the light one).
/// Returns "" when no folder with a layout.xml exists.
std::string resolve_theme(std::string_view theme_set, const std::string& root = {});

/// Puts the user's choices (`zfront.sel_style`, ...) over the loaded theme; `get(key)` returns the stored value or "". Unknown values are ignored.
void apply_user_theme(Theme& th, const std::function<std::string(const std::string&)>& get);

/// The user's own appearance choices (system.cfg `zfront.*`), applied over the loaded theme. `var` is the theme variable,
/// `values` the accepted ones (anything else is ignored: the theme's own value stays).
struct UserThemeKey {
  const char* key;
  const char* var;
  std::vector<std::string> values;
};
const std::vector<UserThemeKey>& user_theme_keys();

/// One installable Zenith-structured theme: the `ThemeSet` value to store and its name in the menu.
struct ThemeChoice {
  std::string value;
  std::string label;
  bool es = false;  // an EmulationStation-structured theme (zfront translates it), not a Zenith-structured one
};
/// Zenith themes found on the device, plain Zenith first, the rest by name: folders with a layout.xml named `*zenith*` in
/// /usr/share/themes and the user themes dir, and the variants zfront ships in /usr/share/zfront/theme-<name> (`theme-light` is
/// `es-theme-zenith`; the others are stored as `es-theme-zenith-<name>`, which `resolve_theme` maps back).
std::vector<ThemeChoice> list_theme_sets(const std::string& root = {});

/// Folder of the EmulationStation-style theme (a `theme.xml` and no Zenith `layout.xml`) that ThemeSet names, searched in the user themes
/// dir, /usr/share/themes and /etc/emulationstation/themes; "" when ThemeSet is not one (Zenith themes included).
std::string resolve_es_theme(std::string_view theme_set, const std::string& root = {});
/// ES themes found on the device (the same folders), as `ThemeChoice`s with `es` set and " (ES)" in the label.
std::vector<ThemeChoice> list_es_themes(const std::string& root = {});

/// Translate the ES theme `es_dir` (see es_theme.hpp) for a w x h screen into a generated Zenith-structured folder, built on the Zenith
/// light/dark folders `resolve_theme` finds. `setting` looks up es_settings.cfg (subset selections); `out_root` empty = default place.
/// False with `err` when it cannot be done (the caller then uses Zenith); `report` is the coverage report on success.
bool translate_es_theme(const std::string& es_dir, unsigned w, unsigned h, const std::function<const std::string*(const std::string&)>& setting,
                        const std::string& root, const std::string& out_root, std::string& dir, std::string& report, std::string& err);

/// Theme folder to draw. `problem(dir, why)` says whether a folder cannot be used (true, with the reason). `primary` is used when it is
/// fine; otherwise `fallback` (when given, different, and fine) with `fell_back` set and `why` the reason `primary` failed. False, with
/// `why`, when neither works. A theme that cannot be drawn must never end the program: systemd restarts the frontend, and the console
/// would loop with no way to fix the setting.
bool pick_theme(const std::string& primary, const std::string& fallback, const std::function<bool(const std::string&, std::string&)>& problem,
                std::string& used, bool& fell_back, std::string& why);

/// ES `--log-path DIR`: sends stderr to `DIR/zfront.log` (appended; restarted when it passed 1 MB). False (stderr untouched) when
/// the folder does not exist or the file cannot be opened.
bool redirect_log(const std::string& dir);

/// ES `global.bootgame.path` / `.cmd` in system.cfg: the game that starts with the console, once per boot (ES `launchStartupGame`; set
/// from the game's options). The path is the ROM's full path, as ES stores it. The command is written for ES, which starts that game if the
/// user switches back to it; zfront starts the game from its path, the same launch a press on the game makes.
inline constexpr const char* kBootGamePath = "global.bootgame.path";
inline constexpr const char* kBootGameCmd = "global.bootgame.cmd";
/// The marker of "this boot already started its game": under /run, which is cleared at every boot.
std::string startup_marker();
/// Whether this process may start the boot game at all: never in a preview, a dry run, a dump or a harness run (`--script`, demo systems).
bool startup_game_allowed(bool preview, bool dry_run, bool dump, bool harness);
/// The ROM to start now, or "" (`rom` is the value of kBootGamePath). Nothing starts when `rom` is empty, when `exists` rejects it (`why`
/// says so), or when this boot already started its game. The first call of a boot creates `marker` before returning the ROM: systemd restarts
/// zfront after a quit or a crash, and those restarts must not start the game again. When the marker cannot be written nothing starts either
/// (`why` says why), because the game would then start on every restart.
std::string startup_game(const std::string& rom, const std::string& marker, const std::function<bool(const std::string&)>& exists, std::string& why);

}  // namespace zf::boot
