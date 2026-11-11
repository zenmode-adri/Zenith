// menu-game: EDIT THIS GAME'S METADATA and DELETE GAME (ES: GuiMetaDataEd, GuiGameOptions::deleteGame). The editor is the screen
// `menu.game.edit` (arg = `game_arg(system, rom)`): a draft of the fields ES shows (name, description, genre, players, developer, publisher,
// release date, rating, favorite, hidden), SAVE, SCRAPE (this game only) and DELETE. B with unsaved changes asks SAVE CHANGES? like ES.
// The gamelist work is in gamemeta.* (`game_edit`, `game_delete`).
#pragma once
#include <string>

#include "menu.hpp"

namespace zf::mg {

/// The rows the options of a game add: the editor and DELETE GAME.
void edit_game_entries(MenuBuilder& m, Ctx& cx, const std::string& system, const std::string& rom_path);
/// Deletes the game after the question was answered with YES: clears the "launch at startup" key when it is this game, removes it
/// (`game_delete`) and closes the menus on success; a refusal shows a toast and stays.
Transition delete_game_now(Ctx& cx, const std::string& system, const std::string& rom_path);

}  // namespace zf::mg
