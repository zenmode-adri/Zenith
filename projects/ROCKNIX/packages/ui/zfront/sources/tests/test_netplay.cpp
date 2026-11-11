// Netplay without a screen: the %NETPLAY% strings ES puts in the launch line (ROCKNIX build), the lobby JSON of libretro, the ROM
// matcher and the way the rooms are listed. The menus need a running app and are not driven here.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "check.hpp"
#include "launch.hpp"
#include "netplay.hpp"
#include "services.hpp"
#include "systems.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

// Shape of what lobby.libretro.com/list returns (fields as of RetroArch 1.16+; one relayed room, one direct, one private, one odd)
const char* kLobby = R"JSON([
 {"fields": {"username": "ana", "game_name": "Super Mario Kart (USA)", "game_crc": "B7A9A5C8", "core_name": "Snes9x", "core_version": "1.62.3 f6d4d3b",
  "subsystem_name": "N/A", "retroarch_version": "1.16.0", "frontend": "linux arm64", "ip": "203.0.113.7", "port": 55435, "mitm_ip": "",
  "mitm_port": 0, "mitm_session": "", "host_method": 2, "has_password": false, "has_spectate_password": false, "connectable": true, "is_retroarch": true,
  "country": "ES", "created": "2026-11-08T20:15:01Z", "updated": "2026-11-08T20:15:21Z", "fixed": false}, "model": "lobby.room", "pk": 1},
 {"fields": {"username": "bob", "game_name": "Sonic The Hedgehog 2", "game_crc": "00000000", "core_name": "Genesis Plus GX", "ip": "198.51.100.4",
  "port": 55436, "mitm_ip": "mitm.example.net", "mitm_port": 55437, "mitm_session": "abc123", "host_method": 3, "has_password": true,
  "has_spectate_password": false, "country": "US", "created": "x", "updated": "y"}, "model": "lobby.room", "pk": 2},
 {"model": "lobby.room", "pk": 3},
 {"fields": {"username": "eve", "game_name": "Tetris; rm -rf /", "core_name": "Gambatte", "ip": "1.2.3.4;reboot", "port": "55435", "host_method": 1}}
])JSON";

fs::path scratch(const char* tag) {
  fs::path p = fs::temp_directory_path() / (std::string("zf-np-") + tag + "-" + std::to_string(::getpid()));
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

}  // namespace

int main() {
  // ---- %NETPLAY% strings (ES FileData.cpp, ROCKNIX)
  {
    ConfMap conf;
    conf["global.netplay.nickname"] = "Zenith Player";
    conf["global.netplay.port"] = "55435";
    launch::Netplay host;
    host.role = launch::NetRole::Host;
    CHECK_EQ(launch::netplay_args(host, conf), std::string("--host --port 55435 --nick Zenith Player"));
    conf["global.netplay.port"] = "60000";
    CHECK_EQ(launch::netplay_args(host, conf), std::string("--host --port 60000 --nick Zenith Player"));
    conf["global.netplay.port"] = "";  // ES writes the default when its menu opens
    CHECK_EQ(launch::netplay_args(host, conf), std::string("--host --port 55435 --nick Zenith Player"));
    conf["global.netplay.port"] = "5 5;x";
    CHECK_EQ(launch::netplay_args(host, conf), std::string("--host --port 55435 --nick Zenith Player"));

    launch::Netplay join;
    join.role = launch::NetRole::Join;
    join.ip = "203.0.113.7";
    join.port = 55435;
    CHECK_EQ(launch::netplay_args(join, conf), std::string("--connect 203.0.113.7 --port 55435 --nick Zenith Player"));
    launch::Netplay watch = join;
    watch.role = launch::NetRole::Watch;
    CHECK_EQ(launch::netplay_args(watch, conf), std::string("--connect 203.0.113.7 --port 55435 --nick Zenith Player"));

    // off: empty; a bad address or port: empty (it would go into a shell line)
    CHECK_EQ(launch::netplay_args(launch::Netplay(), conf), std::string());
    join.ip = "1.2.3.4;reboot";
    CHECK_EQ(launch::netplay_args(join, conf), std::string());
    join.ip = "$(id)";
    CHECK_EQ(launch::netplay_args(join, conf), std::string());
    join.ip = "10.0.0.1";
    join.port = 0;
    CHECK_EQ(launch::netplay_args(join, conf), std::string());
    join.port = 70000;
    CHECK_EQ(launch::netplay_args(join, conf), std::string());
    CHECK(launch::valid_netplay_host("example.org") && launch::valid_netplay_host("fe80::1") && !launch::valid_netplay_host("") && !launch::valid_netplay_host("a b"));

    // the nick comes from the user: its shell characters are escaped
    conf["global.netplay.nickname"] = "a;b $x";
    join.ip = "10.0.0.1";
    join.port = 1234;
    CHECK_EQ(launch::netplay_args(join, conf), std::string("--connect 10.0.0.1 --port 1234 --nick a\\;b \\$x"));
  }

  // ---- default nickname: "<OS_NAME> Player"
  {
    fs::path d = scratch("nick");
    std::ofstream(d / "os-release") << "NAME=\"x\"\nOS_NAME=\"Zenith\"\nVERSION=1\n";
    CHECK_EQ(launch::default_nickname((d / "os-release").string()), std::string("Zenith Player"));
    CHECK_EQ(launch::default_nickname((d / "missing").string()), std::string("ROCKNIX Player"));
    fs::remove_all(d);
  }

  // ---- build_command: %NETPLAY% in the template, appended when absent, core forced on a join
  {
    System sys;
    sys.name = "snes";
    sys.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR% %NETPLAY%";
    sys.emulator = "retroarch";
    sys.core = "snes9x";
    sys.emus = {{"retroarch", {"snes9x", "bsnes"}}};
    Game g;
    g.name = "Mario Kart";
    g.path = "/r/snes/mk.sfc";
    ConfMap conf;
    conf["global.netplay.nickname"] = "Zenith Player";
    conf["global.netplay.port"] = "55435";
    // off: the token is empty, the rest of the line is the same as before
    std::string off = launch::build_command(sys, g, conf, {});
    CHECK(off.find("%NETPLAY%") == std::string::npos && off.find("--host") == std::string::npos);
    CHECK_EQ(launch::build_command(sys, g, conf, {}, launch::Netplay()), off);
    launch::Netplay host;
    host.role = launch::NetRole::Host;
    std::string h = launch::build_command(sys, g, conf, {}, host);
    CHECK(h.find("--host --port 55435 --nick Zenith Player") != std::string::npos);
    CHECK(h.find("--core=snes9x") != std::string::npos);
    // a template without the token gets it appended
    sys.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR%";
    std::string h2 = launch::build_command(sys, g, conf, {}, host);
    CHECK(h2.find("--host --port 55435 --nick Zenith Player") != std::string::npos);
    CHECK(launch::build_command(sys, g, conf, {}).find("--host") == std::string::npos);
    // join with the room's core: the system has it, so it is forced over the user's choice
    launch::Netplay join;
    join.role = launch::NetRole::Join;
    join.ip = "203.0.113.7";
    join.port = 55435;
    join.core = "bsnes";
    std::string j = launch::build_command(sys, g, conf, {}, join);
    CHECK(j.find("--core=bsnes") != std::string::npos && j.find("--core=snes9x") == std::string::npos);
    CHECK(j.find("--connect 203.0.113.7 --port 55435 --nick Zenith Player") != std::string::npos);
    // a core the system does not have is not forced
    join.core = "doesnotexist";
    CHECK(launch::build_command(sys, g, conf, {}, join).find("--core=snes9x") != std::string::npos);
  }

  // ---- lobby JSON
  {
    std::vector<netplay::Room> rooms;
    CHECK(netplay::parse_lobby(kLobby, false, rooms));
    CHECK_EQ(rooms.size(), size_t(3));  // the entry without "fields" is skipped
    CHECK_EQ(rooms[0].username, std::string("ana"));
    CHECK_EQ(rooms[0].game_name, std::string("Super Mario Kart (USA)"));
    CHECK_EQ(rooms[0].game_crc, std::string("B7A9A5C8"));
    CHECK_EQ(rooms[0].core_name, std::string("Snes9x"));
    CHECK_EQ(rooms[0].ip, std::string("203.0.113.7"));
    CHECK_EQ(rooms[0].port, 55435);
    CHECK_EQ(rooms[0].host_method, 2);
    CHECK(!rooms[0].has_password);
    CHECK_EQ(rooms[1].host_method, 3);
    CHECK_EQ(rooms[1].mitm_ip, std::string("mitm.example.net"));
    CHECK_EQ(rooms[1].mitm_port, 55437);
    CHECK(rooms[1].has_password);
    CHECK_EQ(rooms[2].game_name, std::string("Tetris; rm -rf /"));
    // only relayed rooms
    CHECK(netplay::parse_lobby(kLobby, true, rooms));
    CHECK_EQ(rooms.size(), size_t(1));
    CHECK_EQ(rooms[0].username, std::string("bob"));
    // not a lobby
    CHECK(!netplay::parse_lobby("", false, rooms));
    CHECK(!netplay::parse_lobby("<html>502</html>", false, rooms));
    CHECK(!netplay::parse_lobby("{\"a\":1}", false, rooms));
    CHECK(netplay::parse_lobby("[]", false, rooms) && rooms.empty());
  }

  // ---- names and cores
  {
    CHECK_EQ(netplay::core_id("Snes9x"), std::string("snes9x"));
    CHECK_EQ(netplay::core_id("Genesis Plus GX"), std::string("genesisplusgx"));
    CHECK_EQ(netplay::core_id("FinalBurn Neo"), std::string("fbneo"));
    CHECK_EQ(netplay::core_id("Some New Core"), std::string("some_new_core"));
    CHECK_EQ(netplay::remove_parentheses("Super Mario Kart (USA) [!]"), std::string("Super Mario Kart"));
  }

  // ---- matching and listing
  {
    netplay::Library lib(2);
    lib[0].name = "snes";
    lib[0].cores = {"snes9x", "bsnes"};
    lib[0].games = {{"Super Mario Kart", "/r/snes/Super Mario Kart (USA).sfc"}, {"Zelda", "/r/snes/zelda.sfc"}};
    lib[1].name = "megadrive";
    lib[1].cores = {"genesisplusgx"};
    lib[1].games = {{"Sonic The Hedgehog 2", "/r/md/sonic2.md"}};
    std::vector<netplay::Room> rooms;
    CHECK(netplay::parse_lobby(kLobby, false, rooms));
    int s = -1, g = -1;
    CHECK(netplay::find_game(lib, "Super Mario Kart (USA)", "Snes9x", s, g) && s == 0 && g == 0);
    CHECK(netplay::find_game(lib, "Sonic The Hedgehog 2", "Genesis Plus GX", s, g) && s == 1 && g == 0);
    // the room's core is not in the system: no match even with the same name
    CHECK(!netplay::find_game(lib, "Sonic The Hedgehog 2", "Snes9x", s, g));
    CHECK(!netplay::find_game(lib, "Tetris", "Gambatte", s, g));
    CHECK(netplay::core_exists(lib[0], "Snes9x") && !netplay::core_exists(lib[0], "Gambatte") && !netplay::core_exists(lib[0], ""));

    // CRC: the first room has the same ROM, the second a different one
    auto crc = [](const netplay::LocalSystem&, const std::string& path) { return path.find("sonic2") != std::string::npos ? std::string("11111111") : std::string("B7A9A5C8"); };
    std::vector<netplay::LobbyEntry> e = netplay::build_entries(rooms, lib, crc);
    CHECK_EQ(e.size(), size_t(3));
    int same = -1, diff = -1, none = -1;
    for (size_t i = 0; i < e.size(); ++i) {
      if (e[i].room.username == "ana") same = int(i);
      if (e[i].room.username == "bob") diff = int(i);
      if (e[i].room.username == "eve") none = int(i);
    }
    CHECK(same >= 0 && diff >= 0 && none >= 0);
    if (same >= 0 && diff >= 0 && none >= 0) {
      CHECK(e[same].sys == 0 && e[same].core_ok && e[same].crc_same);
      CHECK(e[diff].sys == 1 && e[diff].core_ok && !e[diff].crc_same);
      CHECK(e[none].sys == -1);
      CHECK(same < diff && diff < none);  // the same ROM first, a missing one last
      CHECK(!netplay::state_text(e[same]).empty() && netplay::state_text(e[same]) != netplay::state_text(e[diff]));
    }
    // layout: without "show missing" the room without ROM is not listed; with it, it goes in the second group
    std::vector<netplay::Shown> a = netplay::layout_rows(e, false), b = netplay::layout_rows(e, true);
    CHECK_EQ(a.size(), size_t(2));
    CHECK_EQ(b.size(), size_t(3));
    if (b.size() == 3) CHECK(b[0].group == netplay::Group::Available && b[2].group == netplay::Group::Unavailable);
  }

  // ---- a lobby that cannot be reached: curl's 'could not resolve / connect / time out' is said as no network, not as a curl number
  {
    fs::path d = fs::temp_directory_path() / ("zf-np-lobby-" + std::to_string(::getpid()));
    fs::remove_all(d);
    fs::create_directories(d);
    for (int code : {6, 7, 28}) {
      std::ofstream(d / "curl.sh") << "#!/bin/sh\nexit " << code << "\n";
      ::chmod((d / "curl.sh").c_str(), 0755);
      ::setenv("ZF_CURL", (d / "curl.sh").c_str(), 1);
      ::setenv("ZF_NP_TMP", d.c_str(), 1);
      Services sv;
      sv.cmd.dry_run = false;
      std::string body, err;
      CHECK(!netplay::fetch_lobby(sv, "http://lobby.invalid/list", body, err));
      CHECK_EQ(err, std::string(tr("FAILED")) + " : " + tr("NO NETWORK CONNECTION"));
    }
    ::unsetenv("ZF_CURL");
    ::unsetenv("ZF_NP_TMP");
    fs::remove_all(d);
  }

  return zt::done();
}
