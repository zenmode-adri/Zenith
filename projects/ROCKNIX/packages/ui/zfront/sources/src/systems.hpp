// Systems and games, read from the same files ES uses: `es_systems.cfg` (systems, extensions, launch command,
// emulators/cores) and each system's `gamelist.xml` (names, flags, metadata).
//
// Lazy by design: `load_index` reads only es_systems.cfg and counts files; the gamelist of a system is read by
// `System::open()` when the user enters it. A `Game` carries only light metadata; description and images are asked
// with `gamelist_details` when the game is selected.
#pragma once
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gamelist.hpp"

namespace zf {

struct Game {
  std::string name;
  std::string path;
  /// Light metadata (flags, rating, year, playcount...). desc/images: `gamelist_details` on demand.
  GameMeta meta;
};

struct System {
  std::string name, fullname, theme, command;
  /// Space separated platform of es_systems.cfg (what the scraper maps to ScreenScraper system ids).
  std::string platform;
  /// (emulator, core) chosen when launching: the `default="true"` core of the first emulator that has one.
  std::string emulator, core;
  /// Every (emulator, cores) pair of the system, to validate the overrides of system.cfg.
  std::vector<std::pair<std::string, std::vector<std::string>>> emus;
  std::vector<Game> games;
  /// ROM folder (base of the `./` paths of the gamelist) and the gamelist file that goes with it.
  std::string rom_dir, gamelist;
  /// Extensions (`.gb`...) of the system.
  std::vector<std::string> exts;
  /// Number of games for the carousel without loading the gamelist (`load_index`: counts files, hidden games
  /// included). Equals `games.size()` once `open()` ran or after `load`.
  size_t count = 0;
  /// True once `open()` (or an eager load) ran, even if every game turned out hidden.
  bool loaded = false;

  /// After `gamelist_update_after_play`/`set_favorite` rewrote the gamelist: shifts the byte spans of the loaded
  /// games so `gamelist_details` keeps seeking to the right place (the meta of the edited game itself is stale).
  void apply_edit(const GamelistEdit& e);

  /// Loads the games with their gamelist metadata (lazy systems from `load_index`). No-op if already loaded.
  void open();
};

using ConfMap = std::unordered_map<std::string, std::string>;

/// Loads every system of `cfg` that has at least one game, games included. `roms_root` replaces the `/storage/roms`
/// prefix (tests on the PC).
/// `keep_empty` (ES LoadEmptySystems): the systems without games are kept too, with `count` 0.
bool load_systems(const std::string& cfg, const std::string& roms_root, std::vector<System>& out, std::string& err, bool keep_empty = false);
/// Like `load_systems` but without reading any gamelist: `games` is empty and `count` comes from the directory scan.
bool load_systems_index(const std::string& cfg, const std::string& roms_root, std::vector<System>& out, std::string& err, bool keep_empty = false);

/// Fastest start: no gamelist, no counting. A system appears when it has at least one game (the walk stops at the
/// first one); `count` is `kCountPending` until `App::start_counts` fills it in the background.
bool load_systems_lazy(const std::string& cfg, const std::string& roms_root, std::vector<System>& out, std::string& err, bool keep_empty = false);
bool load_systems_text_lazy(const std::string& xml, const std::string& roms_root, std::vector<System>& out, std::string& err, bool keep_empty = false);

/// Same as `load_systems` from XML text and a ROM root (tests).
bool load_systems_text(const std::string& xml, const std::string& roms_root, bool eager, std::vector<System>& out, std::string& err, bool keep_empty = false);

/// Games of a ROM folder in ES order (sortname/name, case-insensitive, ignoring a leading "A", "AN" or "THE"); hidden
/// ones removed.
std::vector<Game> load_games(const std::string& dir, const std::vector<std::string>& exts, const std::string& gamelist);

/// Emulator and core as ES picks them: per-game key, then per-system key (each validated against the system's
/// emulators), then the `default="true"` ones (`FileData::getlaunchCommand`, `SystemData::getEmulator/getCore`).
std::pair<std::string, std::string> resolve_emu(const System& sys, const Game& game, const ConfMap& conf);

/// Sort key ES uses for a game name.
std::string sort_key(const std::string& sort);

}  // namespace zf
