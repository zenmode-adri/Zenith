// Netplay screens (ES: GuiNetPlaySettings, GuiNetPlay, and the START A NETPLAY GAME popup of GuiGameOptions).
//
//   menu.netplay        NETPLAY SETTINGS (under GAMES > ACCOUNTS, as ES): the keys are those of ES (netplay.hpp)
//   menu.netplay.lobby  CONNECT TO NETPLAY: the libretro lobby, filled in by the job worker; REFRESH asks again
//   menu.netplay.join   JOIN GAME / WATCH GAME of one room
//   menu.netplay.game   arg = `mg::game_arg(system, rom)`: HOST / JOIN a game from its options, plus nick, host and port (ES popup)
//
// ES opens the lobby with X on the system carousel; here X is "random system", so the lobby is a row of NETPLAY SETTINGS (shown while
// `global.netplay` is on) instead. Not here: INDEX GAMES and the LAN rooms (see netplay.hpp).
#include <map>
#include <memory>

#include "menu_game_int.hpp"
#include "netplay.hpp"
#include "registry.hpp"
#include "scraper_hash.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf::netplay {
namespace {

using mg::conf_get;
using mg::detail::conf_switch;
using mg::detail::conf_text;

bool adhoc(Ctx& cx) { return conf_get(cx, "wifi.adhoc.enabled") == "1"; }

Entry& es_switch(MenuBuilder& m, Ctx& cx, std::string label, const char* key, std::string hint) {
  Ctx* c = &cx;
  return m.toggle(std::move(label), [c, key] { return c->es.get_bool(key, false); },
                  [c, key](bool on) {
                    c->es.set_bool(key, on);
                    std::string err;
                    if (!c->es.save(&err)) c->sv->toasts.push(tr("Could not save the setting"), 3000);
                  })
      .hint(std::move(hint));
}

Transition not_connected() { return ui::message(tr("YOU ARE NOT CONNECTED TO A NETWORK")); }

// ---- NETPLAY SETTINGS ---------------------------------------------------------------------------------------------------------

std::unique_ptr<Screen> make_settings() {
  auto relays = std::make_shared<Relays>(load_relays());
  MenuSpec s;
  s.title = tr("NETPLAY SETTINGS");
  s.title_key = N_("NETPLAY SETTINGS");
  s.build = [relays](MenuBuilder& m, Ctx& cx) {
    Ctx* c = &cx;
    Refresh rf = m.refresh;
    // zfront: the lobby of ES's carousel shortcut
    if (enabled(cx) && activated(cx)) m.submenu(tr("CONNECT TO NETPLAY"), "menu.netplay.lobby");

    m.header(tr("SETTINGS"));
    m.toggle(tr("ENABLE NETPLAY"), [c] { return mg::conf_get(*c, "global.netplay") == "1"; }, [c, rf](bool on) {
      mg::conf_put(*c, "global.netplay", on ? "1" : "0");  // GuiNetPlaySettings.cpp:24 / :55 (ES also starts its hasher)
      rf();                                             // CONNECT TO NETPLAY comes and goes with it at once
    });
    conf_text(m, cx, tr("NICKNAME"), "global.netplay.nickname");           // :27

    m.header(tr("OPTIONS"));
    if (!adhoc(cx)) {
      conf_text(m, cx, tr("HOST"), "global.netplay.host");  // :35
      conf_text(m, cx, tr("PORT"), "global.netplay.port");  // :36
      std::vector<Option> o = {{"", tr("NONE")}, {"nyc", tr("NEW YORK")}, {"madrid", tr("MADRID")}, {"montreal", tr("MONTREAL")}, {"saopaulo", tr("SAO PAULO")}};
      for (const auto& kv : *relays) o.push_back({kv.first, tr(utf8_upper(kv.first).c_str())});
      o.push_back({"custom", tr("CUSTOM")});
      m.choice(tr("USE RELAY SERVER"), o, [c, relays] { return relay_selected(c->conf.map(), *relays); },
               [c, relays, rf](const std::string& v) {
                 relay_set(*c, v, *relays);
                 rf();  // the CUSTOM RELAY SERVER row depends on it (ES rebuilds the menu)
               })
          .no_cycle();
      if (relay_selected(cx.conf.map(), *relays) == "custom") conf_text(m, cx, tr("CUSTOM RELAY SERVER"), "global.netplay.customserver");  // :191
      es_switch(m, cx, tr("AUTOMATICALLY CREATE LOBBY"), "NetPlayAutomaticallyCreateLobby", tr("Automatically creates a Netplay lobby when starting a game."));
      es_switch(m, cx, tr("SHOW RELAY SERVER GAMES ONLY"), "NetPlayShowOnlyRelayServerGames", tr("Relay server games have a higher chance of successful entry."));
      es_switch(m, cx, tr("SHOW UNAVAILABLE GAMES"), "NetPlayShowMissingGames", tr("Show rooms for games not present on this machine."));
    }

    m.header(tr("GAME INDEXES"));
    es_switch(m, cx, tr("INDEX NEW GAMES AT STARTUP"), "NetPlayCheckIndexesAtStart", std::string());
  };
  s.on_open = [](Ctx& cx, const Refresh&) {
    if (conf_get(cx, "global.netplay.port").empty()) mg::conf_put(cx, "global.netplay.port", std::to_string(kDefaultPort));  // :17-19
  };
  return make_menu(std::move(s));
}

// ---- the lobby ----------------------------------------------------------------------------------------------------------------

struct Lobby {
  enum Phase { Idle, Loading, Done, Failed } phase = Idle;
  std::string error;
  std::vector<LobbyEntry> entries;
  Library lib;
};

struct Fetched {
  bool ok = false;
  std::string err;
  std::vector<LobbyEntry> entries;
  Library lib;
};

void start_fetch(Ctx& cx, const std::shared_ptr<Lobby>& st, const Refresh& refresh) {
  if (st->phase == Lobby::Loading) return;
  st->phase = Lobby::Loading;
  st->error.clear();
  st->entries.clear();
  if (cx.sv->cmd.dry_run) {  // tests and the preview harness do not ask the network
    st->phase = Lobby::Done;
    return;
  }
  auto res = std::make_shared<Fetched>();
  std::shared_ptr<Services> sv = cx.sv;
  Library lib = snapshot_library(cx);
  const std::string url = lobby_url(cx);
  const bool relay_only = cx.es.get_bool("NetPlayShowOnlyRelayServerGames", false);
  sv->jobs.post(
      [sv, res, lib, url, relay_only]() mutable {
        std::string body, err;
        if (!fetch_lobby(*sv, url, body, err)) {
          res->err = err;
          return;
        }
        std::vector<Room> rooms;
        if (!parse_lobby(body, relay_only, rooms)) {
          res->err = std::string(tr("FAILED")) + " : JSON";
          return;
        }
        load_library(lib);
        std::map<std::string, std::string> crcs;  // one hash per ROM, however many rooms play it
        res->entries = build_entries(rooms, lib, [&](const LocalSystem& ls, const std::string& path) {
          auto it = crcs.find(path);
          if (it != crcs.end()) return it->second;
          scraper::Ident id;
          std::string crc;
          if (scraper::identify(sv->cmd, path, scraper::extract_archives_for(ls.platform), id) && id.hashed) crc = utf8_upper(id.crc);
          crcs[path] = crc;
          return crc;
        });
        res->lib = std::move(lib);
        res->ok = true;
      },
      [st, res, refresh] {
        st->phase = res->ok ? Lobby::Done : Lobby::Failed;
        st->error = res->err;
        st->entries = std::move(res->entries);
        st->lib = std::move(res->lib);
        if (refresh) refresh();
      });
}

// ES NetPlayLobbyListEntry: user, country and frontend, then core and RetroArch version
std::string room_info(const Room& r) {
  return r.username + "  " + r.country + "  " + r.frontend + "  /  " + r.core_name + " (" + r.retroarch_version + ")";
}

std::unique_ptr<Screen> make_join(const std::string& system, const std::string& path, const Room& room) {
  MenuSpec s;
  s.title = tr("CONNECT TO NETPLAY");
  s.title_key = N_("CONNECT TO NETPLAY");
  s.build = [system, path, room](MenuBuilder& m, Ctx&) {
    m.info(room.game_name);
    // ES (GuiNetPlay::launchGame): a relayed room (host_method 3) is reached through the relay's address and port
    const bool relay = room.host_method == 3;
    launch::Netplay np;
    np.ip = relay ? room.mitm_ip : room.ip;
    np.port = relay ? room.mitm_port : room.port;
    np.core = core_id(room.core_name);
    m.action(tr("JOIN GAME"), [system, path, np](Ctx& c) {
      launch::Netplay n = np;
      n.role = launch::NetRole::Join;
      return launch_path(c, system, path, n);
    });
    m.action(tr("WATCH GAME"), [system, path, np](Ctx& c) {
      launch::Netplay n = np;
      n.role = launch::NetRole::Watch;
      return launch_path(c, system, path, n);
    });
  };
  return make_menu(std::move(s));
}

std::unique_ptr<Screen> make_lobby() {
  auto st = std::make_shared<Lobby>();
  MenuSpec s;
  s.title = tr("CONNECT TO NETPLAY");
  s.title_key = N_("CONNECT TO NETPLAY");
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    Ctx* c = &cx;
    Refresh rf = m.refresh;
    m.info(tr("Select a game lobby to join"));
    m.action(tr("REFRESH"), [c, st, rf] {
      start_fetch(*c, st, rf);
      rf();
    }).disabled(st->phase == Lobby::Loading);
    if (st->phase == Lobby::Idle || st->phase == Lobby::Loading) {
      m.info(tr("PLEASE WAIT"));
      return;
    }
    if (st->phase == Lobby::Failed) {
      m.info(st->error).danger();
      return;
    }
    const bool show_missing = cx.es.get_bool("NetPlayShowMissingGames", false);
    std::vector<Shown> rows = layout_rows(st->entries, show_missing);
    if (rows.empty()) m.info(tr("NO GAMES FOUND"));
    for (const Shown& r : rows) {
      const LobbyEntry& e = st->entries[r.index];
      if (r.title) m.header(r.group == Group::Available ? (show_missing ? tr("AVAILABLE GAMES") : tr("ONLINE GAMES")) : tr("UNAVAILABLE GAMES"));
      if (r.group == Group::Unavailable) {
        m.info(e.room.game_name, e.room.username).hint(room_info(e.room));
        continue;
      }
      const LocalSystem& ls = st->lib[size_t(e.sys)];
      const LocalGame& g = ls.games[size_t(e.game)];
      std::string sysname = ls.name, path = g.path;
      Room room = e.room;
      m.action(g.name + " [" + ls.name + "]", [sysname, path, room] { return Transition::push(make_join(sysname, path, room)); })
          .value(state_text(e))
          .hint(room_info(e.room));
    }
  };
  s.on_open = [st](Ctx& cx, const Refresh& rf) {
    start_fetch(cx, st, rf);
    if (rf) rf();
  };
  return make_menu(std::move(s));
}

// ---- START A NETPLAY GAME (popup of the game options) -------------------------------------------------------------------------------

std::string game_name(const Ctx& cx, const std::string& system, const std::string& rom) {
  if (const System* s = mg::find_system(cx, system))
    for (const Game& g : s->games)
      if (g.path == rom) return g.name;
  return path_stem(rom);
}

std::unique_ptr<Screen> make_game(const std::string& arg) {
  std::string system, rom;
  if (!mg::split_game_arg(arg, system, rom)) return nullptr;
  MenuSpec s;
  s.title = tr("NETPLAY");
  s.title_key = N_("NETPLAY");
  s.build = [system, rom](MenuBuilder& m, Ctx& cx) {
    Ctx* c = &cx;
    m.info(game_name(cx, system, rom));
    m.header(tr("START GAME"));
    m.action(tr("HOST A NETPLAY GAME"), [c, system, rom]() -> Transition {
      if (!c->status.wifi) return not_connected();  // ES: getIpAddress() == "NOT CONNECTED"
      launch::Netplay np;
      np.role = launch::NetRole::Host;
      return launch_path(*c, system, rom, np);
    });
    // ES passes no address here (its "--connect  --port 0" cannot work): the NETPLAY HOST and PORT rows below are what this row joins
    m.action(tr("JOIN A NETPLAY GAME"), [c, system, rom]() -> Transition {
      if (!c->status.wifi) return not_connected();
      launch::Netplay np;
      np.role = launch::NetRole::Join;
      np.ip = conf_get(*c, "global.netplay.host");
      long port = kDefaultPort;
      if (!conf_get(*c, "global.netplay.port").empty() && !parse_int(conf_get(*c, "global.netplay.port"), port)) port = 0;
      np.port = int(port);
      return launch_path(*c, system, rom, np);
    });
    m.header(tr("OPTIONS"));
    conf_text(m, cx, tr("NETPLAY NICKNAME"), "global.netplay.nickname");
    conf_text(m, cx, tr("PLAYER PASSWORD"), "global.netplay.password");
    if (!adhoc(cx)) {
      conf_switch(m, cx, tr("PUBLICLY ANNOUNCE GAME"), "global.netplay_public_announce", "1", "0");
      conf_text(m, cx, tr("NETPLAY HOST"), "global.netplay.host");
      conf_text(m, cx, tr("NETPLAY PORT"), "global.netplay.port");
    }
  };
  return make_menu(std::move(s));
}

}  // namespace

void game_entry(MenuBuilder& m, Ctx& cx, const std::string& system, const std::string& rom_path) {
  const System* s = mg::find_system(cx, system);
  if (!s) return;
  for (const Game& g : s->games) {
    if (g.path != rom_path) continue;
    if (game_supported(cx, *s, g)) m.submenu(tr("START A NETPLAY GAME"), "menu.netplay.game", mg::game_arg(system, rom_path));
    return;
  }
}

ZF_REGISTER_SCREEN("menu.netplay", [](Ctx&, const std::string&) { return make_settings(); });
ZF_REGISTER_SCREEN("menu.netplay.lobby", [](Ctx&, const std::string&) { return make_lobby(); });
ZF_REGISTER_SCREEN("menu.netplay.game", [](Ctx&, const std::string& arg) { return make_game(arg); });

}  // namespace zf::netplay
