// MODULE `bezelproject` (ES: GuiBezelInstaller + ApiSystem::getBatoceraBezelsList / installBatoceraBezel / uninstallBatoceraBezel +
// the bezel rules of ContentInstaller/GuiBezelInstaller::OnContentInstalled). The work is done by `rocknix-es-thebezelproject` of the
// image (`list`, `install <system>`, `remove <system>`), which fills /storage/roms/bezels/thebezelproject; `menu_game_src.cpp` lists
// the sets of that folder and /tmp/overlays/bezels, so a new set shows up in DECORATION SET by itself.
//
// Output of `list` (one line per system with ROMs): `[A] snes - https://github.com/thebezelproject/bezelproject-SNES` (available) or
// `[I] snes - <url>` (installed); `install` prints `[snes] Installing bezels >>> 42%` every second (when not on a terminal) and ends with
// the exit status; errors are one line `Error : ...`.
#pragma once
#include <string>
#include <vector>

#include "app.hpp"

namespace zf::bezel {

struct Set {
  bool installed = false;
  std::string name, url, folder;
};

/// The script: `rocknix-es-thebezelproject`, or `$ZF_BEZEL_CMD` (tests).
std::string script();
/// ES `isScriptingSupported(DECORATIONS) && isScriptingSupported(THEBEZELPROJECT)`: the script is in /usr/bin and a decorations folder
/// has something in it.
bool available();
std::string list_command();
std::string install_command(const std::string& name);
std::string remove_command(const std::string& name);
/// A system name the script may be given (letters, digits, `.`, `_`, `-`).
bool valid_name(const std::string& name);

/// ES `getBatoceraBezelsList`: lines are split at blanks; only `[I]` and `[A]` lines with a name, never the name `?`.
std::vector<Set> parse_list(const std::string& output);
/// Last non-empty line of a script output (the progress, or the error).
std::string last_line(const std::string& text);

/// ES `GuiBezelInstaller::OnContentInstalled`, install: a system with no bezel set of its own (or AUTO) takes `thebezelproject` when the
/// global decoration is set and not AUTO.
void on_installed(Ctx& cx, const std::string& system);
/// Uninstall: a system that had `thebezelproject` goes back to AUTO.
void on_removed(Ctx& cx, const std::string& system);

}  // namespace zf::bezel
