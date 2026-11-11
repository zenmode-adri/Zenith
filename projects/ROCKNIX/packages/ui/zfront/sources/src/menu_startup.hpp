// menu-game: LAUNCH THIS GAME AT STARTUP (ES: the switch of GuiMenu::popSpecificConfigurationGui and the STOP row of the developer
// options). The keys are ES's (`global.bootgame.path` and `.cmd` in system.cfg); the start itself is `boot::startup_game` (bootargs.hpp).
#pragma once
#include <string>

#include "menu.hpp"

namespace zf::mg {

/// The switch of a game's options: on makes this game the one that starts with the console (it replaces another), off clears it.
void startup_game_entry(MenuBuilder& m, Ctx& cx, const std::string& system, const std::string& rom_path);
/// The row of the games menu that clears the game set to start with the console; shown only while one is set.
void startup_stop_entry(MenuBuilder& m, Ctx& cx);
/// Writes the game as the one that starts with the console (its path and its launch command). False when `system` has no such game.
bool set_startup_game(Ctx& cx, const std::string& system, const std::string& rom_path);
bool clear_startup_game(Ctx& cx);
bool is_startup_game(const Ctx& cx, const std::string& rom_path);

}  // namespace zf::mg
