// MODULE `netplay` (ES: GuiNetPlaySettings, GuiNetPlay, the netplay part of GuiGameOptions and FileData::getlaunchCommand).
//
// What is here and what is not:
//  * the lobby of libretro (`global.netplay.lobby`, default http://lobby.libretro.com/list/): the JSON is parsed field by field as
//    ES's `populateFromJson` does, rooms are matched with the games of the netplay systems like `getFileData` (by name: name, file stem
//    and the no-space form; the ROM's CRC is then compared with the room's, as ES's `checkCrc32` does), and the rows follow `populateList`
//    (AVAILABLE / ONLINE / UNAVAILABLE GAMES);
//  * the launch: `launch_game` builds the command with `launch::netplay_args` (the exact ROCKNIX strings of ES) and goes through the
//    same checks as a launch from the list (BIOS check, savestate arguments);
//  * NOT done: the LAN discovery of ES (UDP broadcast, "LAN GAMES"), matching by the CRC stored in gamelist.xml (zfront does not read
//    `<crc32>`: a room is found by its game name) and INDEX GAMES (ES's hasher), so those rows are absent.
//
// Threading: `fetch_lobby` and `build_entries` are blocking (they run curl and hash files): the lobby screen calls them on the job worker.
// The worker never reads `Ctx`: it gets a `Library` made on the main thread by `snapshot_library`.
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "app.hpp"
#include "launch.hpp"
#include "menu.hpp"

namespace zf::netplay {

constexpr const char* kLobbyUrl = "http://lobby.libretro.com/list/";  // GuiNetPlay::startRequest
constexpr int kDefaultPort = 55435;                                     // GuiNetPlaySettings

/// ES `isNetplaySupported` of a system (some emulator core has features="netplay"; without a features file: the command has `%NETPLAY%`).
bool system_supported(const System& sys);
/// ES `SystemData::isNetplayActivated`: some system supports it.
bool activated(const Ctx& cx);
/// ES `FileData::isNetplaySupported`: `global.netplay` is on and the core of this game has features="netplay".
bool game_supported(const Ctx& cx, const System& sys, const Game& game);
/// system.cfg `global.netplay` is "1" (ES getBool, default off).
bool enabled(const Ctx& cx);

/// ES writes "<name> Player" into `global.netplay.nickname` the first time a launch finds it empty.
void ensure_nickname(Ctx& cx);

// ---- community relay servers (ES `loadCommunityRelayServers`) --------------------------------------------------------------------
using Relays = std::vector<std::pair<std::string, std::string>>;  // id, address
Relays parse_relays(const std::string& xml);
/// `resources/community_relay_servers.xml` of the user dir, then of the image.
Relays load_relays();
/// The relay choice of the menu: `global.netplay.relay`, with "custom" shown as the community server whose address is the custom one.
std::string relay_selected(const ConfMap& conf, const Relays& r);
/// ES's callback of USE RELAY SERVER, on system.cfg.
void relay_set(Ctx& cx, const std::string& v, const Relays& r);

// ---- the lobby ---------------------------------------------------------------------------------------------------------------------
/// ES `LobbyAppEntry`: the fields of one room.
struct Room {
  int host_method = 0, mitm_port = 0, port = 0;
  std::string core_name, username, game_crc, mitm_ip, subsystem_name, frontend, created, ip, updated, country, game_name, mitm_session, retroarch_version;
  bool has_password = false, has_spectate_password = false, fixed = false;
};
/// Rooms of the lobby JSON (`[{"fields": {...}}, ...]`). False when it is not JSON or not an array. `relay_only`: NetPlayShowOnlyRelayServerGames
/// (rooms whose host_method is not 3 are dropped).
bool parse_lobby(std::string_view json, bool relay_only, std::vector<Room>& out);
/// The core id ES uses for a lobby core name (its `coreList` for non-Windows, else lower case with `_` for blanks).
std::string core_id(const std::string& lobby_core_name);
/// ES `Utils::String::removeParenthesis` (what is between (...) and [...] goes, then trim).
std::string remove_parentheses(const std::string& s);
/// ES `getFileData`'s `normalizeName`.
std::string normalize_name(const std::string& s);

struct LocalGame {
  std::string name, path;
};
struct LocalSystem {
  std::string name, rom_dir, gamelist, platform;
  std::vector<std::string> exts, cores;  // `cores`: every core of every emulator of the system
  std::vector<LocalGame> games;
  bool need_load = false;  // `games` still has to be read from the ROM folder (on the worker)
};
using Library = std::vector<LocalSystem>;
/// Main thread: the netplay systems with what the matcher needs (the games of the systems that are open, the folder of the others).
Library snapshot_library(Ctx& cx);
/// Worker: reads the games of the systems that were not open.
void load_library(Library& lib);

struct LobbyEntry {
  Room room;
  int sys = -1, game = -1;  // index in the Library of the matched ROM; -1 = none
  bool core_ok = false;     // the matched ROM's system has the room's core (ES `coreExists`)
  bool crc_same = false;    // the ROM's CRC is the room's
};
/// ES `getFileData` over the library: the first system (in order) with a game of that name. `by_crc` is not available (see above).
bool find_game(const Library& lib, const std::string& game_name, const std::string& core_name, int& sys, int& game);
/// ES `coreExists`.
bool core_exists(const LocalSystem& s, const std::string& core_name);
/// Matches every room with the library and sorts them as ES does (the same ROM first, then the core available). `crc_of(path)` returns the
/// CRC of a ROM in upper case hex, or "" (it may hash the file: worker only).
std::vector<LobbyEntry> build_entries(const std::vector<Room>& rooms, const Library& lib, const std::function<std::string(const LocalSystem&, const std::string&)>& crc_of);
/// Text of the state of a matched room: SAME ROM, DIFFERENT ROM, UNKNOWN ROM VERSION (+ UNAVAILABLE CORE). Translated.
std::string state_text(const LobbyEntry& e);

/// How ES puts the rooms in the list (`populateList`): which entries are shown, in which group.
enum class Group : uint8_t { Available, Unavailable };
struct Shown {
  size_t index;  // in the LobbyEntry list
  Group group;
  bool title;    // the group's header goes before this row
};
/// `show_missing` = NetPlayShowMissingGames. ES shows the rooms with a ROM (and a core, unless `show_missing`), then the rooms without ROM when `show_missing`.
std::vector<Shown> layout_rows(const std::vector<LobbyEntry>& entries, bool show_missing);

/// Lobby JSON from `url` (http/https only). Blocking, worker. False with `err` set (translated) on failure; `dry` true when nothing ran.
bool fetch_lobby(Services& sv, const std::string& url, std::string& body, std::string& err);

/// The lobby URL of system.cfg (`global.netplay.lobby`) or the default.
std::string lobby_url(const Ctx& cx);

/// Launch of a matched ROM or of the game of the options menu: `sys`/`game` index `cx.systems` (the system is opened here). Does what a
/// launch from the list does (BIOS check, savestate arguments), with the netplay arguments in place of `%NETPLAY%`. An address that is not
/// valid, or a launch that cannot be built, gives a toast and no transition.
Transition launch_game(Ctx& cx, size_t sys, size_t game, const launch::Netplay& np);
/// The same given the system name and the ROM path (what the lobby keeps).
Transition launch_path(Ctx& cx, const std::string& system, const std::string& rom_path, const launch::Netplay& np);

/// Game options hook (ES: START A NETPLAY GAME): the row that opens `menu.netplay.game`, only for a game whose core supports netplay with
/// `global.netplay` on. Call it from the options of a game.
void game_entry(MenuBuilder& m, Ctx& cx, const std::string& system, const std::string& rom_path);

}  // namespace zf::netplay
