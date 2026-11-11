// Systems and games, read from the same files ES uses: `es_systems.cfg` (systems, extensions, launch command,
// emulators/cores) and each system's `gamelist.xml` (names, flags, metadata).
//
// Lazy by design: `load_index` reads only es_systems.cfg and counts files; the gamelist of a system is read by
// `System::open()` when the user enters it. A `Game` carries only light metadata; description and images are asked
// with `gamelist_details` when the game is selected.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
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
  /// Only in the games of a collection system (colsys.hpp): the name of the system the game really belongs to (interned, process
  /// lifetime; empty in every ordinary game) and its index in that system's `games` when the collection was built (a hint: the path
  /// decides). Launching, favourites, metadata and play counts always go to that system.
  std::string_view src_system;
  uint32_t src_idx = 0;
};

/// How the games are read (ES ShowHiddenFiles, IgnoreLeadingArticles). Process wide: `listopts::prepare` sets it from es_settings.cfg
/// before a system is opened; the default is ES's own.
struct LoadOptions {
  bool show_hidden = false;
  bool ignore_articles = true;
};
void set_load_options(const LoadOptions& o);
LoadOptions load_options();
/// The options as one number: 0 = the defaults. `System::load_opts` keeps the tag the games were read with.
uint8_t load_tag();

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
  /// `load_tag()` at the time the games were read: a different one means they must be read again (`listopts::prepare`).
  uint8_t load_opts = 0;
  /// The gamelist was read up to a point where it stops (truncated or malformed): the games before it are there, the rest is not.
  /// `damaged_told`: the toast that names the system was already shown.
  bool gamelist_damaged = false, damaged_told = false;
  /// An automatic or custom collection shown as a carousel card (colsys.hpp). Such systems live in `Ctx::cols`, never in `Ctx::systems`;
  /// their `games` are copies that point to the real system (`Game::src_system`).
  bool collection = false;

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
/// `damaged` (optional) tells whether the gamelist file is cut short or malformed (it does not open, or is blank: not damaged).
std::vector<Game> load_games(const std::string& dir, const std::vector<std::string>& exts, const std::string& gamelist, bool* damaged = nullptr);

/// Emulator and core as ES picks them: per-game key, then per-system key (each validated against the system's
/// emulators), then the `default="true"` ones (`FileData::getlaunchCommand`, `SystemData::getEmulator/getCore`).
std::pair<std::string, std::string> resolve_emu(const System& sys, const Game& game, const ConfMap& conf);

/// Sort key ES uses for a game name.
std::string sort_key(const std::string& sort);
/// Same, with the leading article kept when `strip_articles` is false.
std::string sort_key_opt(const std::string& sort, bool strip_articles);

}  // namespace zf
