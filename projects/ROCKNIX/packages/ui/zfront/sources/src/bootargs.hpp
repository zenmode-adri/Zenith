// Command line conveniences that let `start_es.sh` start zfront like it starts ES (docs/os-contract.md, "Linea de ordenes"):
//   * `--theme` may be left out: the theme folder comes from ES's own `ThemeSet` setting (see `resolve_theme`),
//   * `--log-path DIR` (ES's option) is accepted: stderr goes to `DIR/zfront.log`,
//   * with `--fullscreen` and no `--size` the layout ratio follows the real output (sdlr.cpp reloads the theme if it differs).
#pragma once
#include <string>
#include <string_view>

namespace zf::boot {

/// Folder of the merged Zenith theme for ES's `ThemeSet` value (empty when the setting is not there). `root` prefixes every
/// absolute path (tests). zfront has a single theme, Zenith, so a ThemeSet that is not Zenith is shown as Zenith (light unless the name says dark).
///   Zenith set named in ThemeSet -> that folder in the user themes dir, then /usr/share/themes (the OS package builds both
///   `es-theme-zenith` and `es-theme-zenith-dark`), otherwise /usr/share/zfront/theme-{dark,light}, otherwise the OS package.
/// "dark" in the name -> dark; anything else, no setting included, -> light (the image ships `ThemeSet=es-theme-zenith`, the light one).
/// Returns "" when no folder with a layout.xml exists.
std::string resolve_theme(std::string_view theme_set, const std::string& root = {});

/// ES `--log-path DIR`: sends stderr to `DIR/zfront.log` (appended; restarted when it passed 1 MB). False (stderr untouched) when
/// the folder does not exist or the file cannot be opened.
bool redirect_log(const std::string& dir);

}  // namespace zf::boot
