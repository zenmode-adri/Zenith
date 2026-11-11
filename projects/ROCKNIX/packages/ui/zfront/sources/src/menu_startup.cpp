// menu-game: LAUNCH THIS GAME AT STARTUP (ES: the switch of GuiMenu::popSpecificConfigurationGui and the STOP row of the developer
// options). The keys are ES's: `global.bootgame.path` (the ROM's full path) and `global.bootgame.cmd` (its launch command), both in
// system.cfg through conf_put.
#include "menu_startup.hpp"

#include <vector>

#include "bootargs.hpp"
#include "i18n.hpp"
#include "launch.hpp"
#include "menu_game.hpp"
#include "pad.hpp"
#include "util.hpp"

namespace zf::mg {

bool is_startup_game(const Ctx& cx, const std::string& rom_path) { return !rom_path.empty() && conf_get(cx, boot::kBootGamePath) == rom_path; }

bool set_startup_game(Ctx& cx, const std::string& system, const std::string& rom_path) {
  const System* sys = find_system(cx, system);
  if (!sys || rom_path.find_first_of("\r\n") != std::string::npos) return false;
  const Game* game = nullptr;
  for (const Game& g : sys->games)
    if (g.path == rom_path) game = &g;
  if (!game) return false;
  std::vector<JoyInfo> joys;
  if (cx.pad) joys = cx.pad->joysticks();
  const std::string cmd = launch::build_command(*sys, *game, cx.conf.map(), joys);
  if (cmd.empty()) return false;  // a game without a command cannot start (ES: the same)
  // the command first, the path last: the path is what arms the start, so a failure in between never leaves a game armed
  // without its command (conf_put toasts when a write fails)
  if (!conf_put(cx, boot::kBootGameCmd, cmd)) return false;
  if (!conf_put(cx, boot::kBootGamePath, rom_path)) {
    conf_put(cx, boot::kBootGameCmd, "");
    return false;
  }
  return true;
}

bool clear_startup_game(Ctx& cx) {
  // disarm first (the path), then forget the command
  const bool path_ok = conf_put(cx, boot::kBootGamePath, "");
  if (!path_ok) return false;
  return conf_put(cx, boot::kBootGameCmd, "");
}

void startup_game_entry(MenuBuilder& m, Ctx& cx, const std::string& system, const std::string& rom_path) {
  Ctx* c = &cx;
  m.toggle(
       tr("LAUNCH THIS GAME AT STARTUP"), [c, rom_path] { return is_startup_game(*c, rom_path); },
       [c, system, rom_path](bool on) {
         if (on) set_startup_game(*c, system, rom_path);
         else clear_startup_game(*c);
       })
      .hint(tr("Starts this game when the console boots, before the menus, once per boot."));
}

void startup_stop_entry(MenuBuilder& m, Ctx& cx) {
  const std::string rom = conf_get(cx, boot::kBootGamePath);
  if (rom.empty()) return;
  const std::string folder = rom.substr(0, rom.find_last_of('/'));
  Ctx* c = &cx;
  Refresh rf = m.refresh;
  // ES shows the game as "<file name> [<folder>]" under the label
  m.action(tr("STOP LAUNCHING THIS GAME AT STARTUP"), [c, rf] {
     clear_startup_game(*c);
     rf();
   }).hint(path_stem(rom) + " [" + path_stem(folder) + "]");
}

}  // namespace zf::mg
