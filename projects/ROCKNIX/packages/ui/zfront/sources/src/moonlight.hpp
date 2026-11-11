// MODULE `moonlight` (ES: GuiMoonlight, GAME SETTINGS > TOOLS > MOONLIGHT GAME STREAMING): the commands, the files and the parsers.
//
// What ES does and what is the same here:
//   QUIT CURRENT GAME     `moonlight quit <ip>`                                        (ip = system.cfg `moonlight.host`)
//   PAIR WITH SERVER      `[QT_QPA_PLATFORM=wayland ]moonlight pair -pin <4 digits> <ip>`; the output line "Connect to <ip>..." sets `moonlight.host`,
//                         "Succesfully paired" is the success; then, with the Qt build (`Moonlight.conf` exists) client.pem and key.pem are
//                         extracted from the config into the cache
//   UPDATE MOONLIGHT GAMES  GET https://<ip>:47984/applist?... with the client certificate (ES's own HttpReq, no `moonlight list`), then one
//                         .sh per app in /storage/roms/moonlight, its box art (appasset) in images/ and a gamelist.xml
//   UNPAIR WITH SERVER    removes the pairing files
//   the launch script     `moonlight stream -app "<title>" -platform sdl <ip>` (embedded build) or `QT_QPA_PLATFORM=wayland moonlight stream <ip> "<title>" --quit-after`
//
// Deliberate differences, each a bug of ES or a safety rule:
//  * ES tests the embedded build with `ss << popen(..)`, which prints the FILE pointer and never matches: it always uses the Qt line. Here
//    the output of `moonlight -v` is read (word "Embedded").
//  * ES empties /storage/roms/moonlight BEFORE it asks the server and leaves it empty when the server does not answer. Here nothing is
//    removed until the application list was read.
//  * ES removes the pairing with `rm -r "~/.cache/..."`, whose quoted `~` is never expanded: nothing was removed. Here the folders under
//    /storage are removed.
//  * Titles come from the network and go into a shell script: `\`, `"`, `$` and backtick are escaped inside the quotes, control characters
//    become blanks. The server address goes into a shell line, so only letters, digits, `.`, `:` and `-` are accepted.
#pragma once
#include <string>
#include <vector>

#include "services.hpp"

namespace zf::moonlight {

/// Where things are; `root` is prefixed to every path (tests).
struct Paths {
  std::string root;
  std::string conf_qt = "/storage/.config/Moonlight Game Streaming Project/Moonlight.conf";
  std::string config_qt_dir = "/storage/.config/Moonlight Game Streaming Project";
  std::string cache_qt = "/storage/.cache/Moonlight Game Streaming Project";
  std::string cache_embedded = "/storage/.cache/moonlight";
  std::string roms = "/storage/roms/moonlight";
  std::string layout_db = "/storage/.config/moonlight/gamecontrollerdb.txt";
  std::string rooted(const std::string& p) const { return root + p; }
};
Paths& paths();

/// The Qt build is installed (its config file exists): ES then uses the Qt command lines and certificate folder.
bool qt_build();
/// Folder of client.pem and key.pem, with a trailing slash (ES `MakeRequest`).
std::string cert_dir();

/// `moonlight -v` printed the embedded build.
bool is_embedded(const std::string& version_output);
/// A server address we accept in a command line.
bool valid_host(const std::string& ip);

std::string quit_command(const std::string& ip);
std::string pair_command(const std::string& pin, const std::string& ip, bool embedded);
/// "Connect to 10.1.10.217..." -> the address (ES `ParseServerIp`).
bool parse_server_ip(const std::string& line, std::string* ip);
/// The pairing succeeded (ES compares the whole line).
bool paired_ok(const std::string& output);

struct App {
  std::string title, id;
};
/// ES `UpdateMoonlightGames`: `<root><App><AppTitle/><ID/></App>...</root>`. False when it is not that document.
bool parse_applist(const std::string& xml, std::vector<App>& out);
/// Name of the script of an app: the title with `/` as blanks (and control characters as blanks).
std::string script_name(const std::string& title);
/// Text of `<name>.sh`.
std::string script_text(const std::string& ip, const std::string& title, bool qt);
/// gamelist.xml of the apps.
/// `old_xml`: the gamelist being replaced; what it says of a game that is still there, other than path / name / image, is kept.
std::string gamelist_text(const std::vector<App>& apps, const std::string& old_xml = std::string());
/// `-----BEGIN ...` of `Moonlight.conf` for `key` ("certificate" or "key"): the value of the `@ByteArray(...)` with its `\n` escapes as line
/// breaks (ES `extractClient` / `extractKey`). Empty when the config has no such line.
std::string extract_pem(const std::string& conf, const std::string& key);
/// A random uuid in the shape ES builds for its requests.
std::string new_guid();
/// `https://<ip>:47984/<command>?<args>&uniqueid=0123456789ABCDEF&uuid=<guid>` (ES `MakeUrl`).
std::string make_url(const std::string& ip, const std::string& command, std::string args, const std::string& guid);
/// The curl line that asks the server (output to `out_file`, or `-o -` is not used: the answer goes to a file).
std::string request_command(const std::string& curl, const std::string& url, const std::string& out_file);

struct UpdateResult {
  bool ok = false;
  size_t apps = 0;
};

/// UPDATE MOONLIGHT GAMES, blocking (worker): asks the server, then replaces the folder's files. `ip` must be valid.
UpdateResult update_games(zf::Services& sv, const std::string& ip);
/// Removes the pairing (`qt` build: the config and cache folders, else the embedded cache). True when something was there.
bool unpair();
/// Writes client.pem / key.pem from `Moonlight.conf` when the Qt build keeps its pairing there and the files are missing.
void extract_certs();

}  // namespace zf::moonlight
