// One game of a gamelist.xml: write chosen fields, remove its element, read plain-text fields (developer, publisher...) that
// `GameMeta` does not keep. Same rules as the other writers in gamelist.hpp (block copy + fsync + rename, a malformed file is
// never touched, the `GamelistEdit` lets `System::apply_edit` keep the spans of the other games right).
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gamelist.hpp"

namespace zf {

/// Sets (replaces or appends) the plain-text children `fields` (tag, text) of the game `rom` (absolute path). Creates the game /
/// the file when missing, like `gamelist_set_favorite`.
bool gamelist_set_fields(const std::string& xml_path, const std::string& rom_dir, const std::string& rom,
                         const std::vector<std::pair<std::string, std::string>>& fields, std::string& err, GamelistEdit* edit = nullptr);

/// Removes the `<game>` element of `rom` (and its indentation and line break). A game that is not in the file, or no file, is
/// not an error (`edit` stays empty). A malformed file is left untouched and the call fails.
bool gamelist_remove_game(const std::string& xml_path, const std::string& rom_dir, const std::string& rom, std::string& err, GamelistEdit* edit = nullptr);

/// The text (entities decoded) of the children `tags` of the game `rom`; absent tags are left out of the result. Reads the byte
/// range of `meta` (falls back to one streaming search when it is stale). Empty map when the game is not in the file.
std::map<std::string, std::string> gamelist_read_fields(const std::string& xml_path, const std::string& rom_dir, const std::string& rom, const GameMeta& meta,
                                                        const std::vector<std::string>& tags);

}  // namespace zf
