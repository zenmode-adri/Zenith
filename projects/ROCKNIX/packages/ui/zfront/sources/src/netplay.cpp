#include "netplay.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <map>

#include "json.hpp"
#include "launchcheck.hpp"
#include "menu_game.hpp"
#include <pugixml.hpp>
#include "savestates.hpp"
#include "scraper_hash.hpp"
#include "util.hpp"

namespace zf::netplay {

// ---- settings and support -------------------------------------------------------------------------------------------------------------

bool enabled(const Ctx& cx) { return cx.conf.get_or("global.netplay", "") == "1"; }  // SystemConf::getBool: default off

bool system_supported(const System& sys) {
  const mg::FeatureDb& db = mg::features();
  if (!db.loaded()) return sys.command.find("%NETPLAY%") != std::string::npos;  // ES, without es_features.cfg
  for (const auto& e : sys.emus)
    for (const std::string& c : e.second)
      if (db.resolve(sys, e.first, c).has(mg::F_netplay)) return true;
  return false;
}

bool activated(const Ctx& cx) {
  for (const System& s : cx.systems)
    if (system_supported(s)) return true;
  return false;
}

bool game_supported(const Ctx& cx, const System& sys, const Game& game) {
  if (!enabled(cx)) return false;
  const mg::FeatureDb& db = mg::features();
  if (!db.loaded()) return sys.command.find("%NETPLAY%") != std::string::npos;
  mg::Pick p = mg::resolved_pick(sys, cx.conf.map(), path_filename(game.path));
  return db.resolve(sys, p.emu, p.core).has(mg::F_netplay);
}

void ensure_nickname(Ctx& cx) {
  if (!cx.conf.get_or("global.netplay.nickname", "").empty()) return;
  std::string err;
  cx.conf.persist("global.netplay.nickname", launch::default_nickname(), &err);
}

// ---- relays ---------------------------------------------------------------------------------------------------------------------------

Relays parse_relays(const std::string& xml_in) {
  Relays out;
  std::string xml = xml_in;
  if (xml.compare(0, 3, "\xEF\xBB\xBF") == 0) xml.erase(0, 3);
  pugi::xml_document doc;
  if (!doc.load_string(xml.c_str())) return out;
  pugi::xml_node root = doc.child("community_relay_servers");
  if (!root) return out;
  for (pugi::xml_node s = root.child("server"); s; s = s.next_sibling("server")) {
    std::string id = s.attribute("id").as_string(), address = s.attribute("address").as_string();
    if (!id.empty() && !address.empty()) out.emplace_back(id, address);
  }
  return out;
}

Relays load_relays() {
  std::string text;
  for (const std::string& p : {mg::env().rooted(mg::env().user_dir + "/resources/community_relay_servers.xml"),
                               mg::env().rooted("/usr/config/emulationstation/resources/community_relay_servers.xml")})
    if (read_file(p, text)) return parse_relays(text);
  return {};
}

std::string relay_selected(const ConfMap& conf, const Relays& r) {
  auto get = [&](const char* k) {
    auto it = conf.find(k);
    return it == conf.end() ? std::string() : it->second;
  };
  std::string sel = get("global.netplay.relay");
  if (sel == "custom") {
    const std::string custom = get("global.netplay.customserver");
    for (const auto& kv : r)
      if (kv.second == custom) {
        sel = kv.first;
        break;
      }
  }
  if (sel.empty() || sel == "nyc" || sel == "madrid" || sel == "montreal" || sel == "saopaulo" || sel == "custom") return sel;
  for (const auto& kv : r)
    if (kv.first == sel) return sel;
  return std::string();  // ES: no option matches, the first (NONE) is shown
}

// GuiNetPlaySettings::addRelayServerOptions, the selection callback
void relay_set(Ctx& cx, const std::string& v, const Relays& r) {
  const std::string original = cx.conf.get_or("global.netplay.customserver", "");
  const std::string backup = cx.conf.get_or("global.netplay.customserver.backup", "");
  auto community = std::find_if(r.begin(), r.end(), [&](const auto& kv) { return kv.first == v; });
  auto is_community_address = [&](const std::string& a) {
    return std::any_of(r.begin(), r.end(), [&](const auto& kv) { return kv.second == a; });
  };
  if (community != r.end()) {
    mg::conf_put(cx, "global.netplay.relay", "custom");
    mg::conf_put(cx, "global.netplay.customserver", community->second);
    if (!is_community_address(original)) mg::conf_put(cx, "global.netplay.customserver.backup", original);
  } else if (v == "custom") {
    mg::conf_put(cx, "global.netplay.relay", v);
    if (!backup.empty()) mg::conf_put(cx, "global.netplay.customserver", backup);
    else if (is_community_address(original)) mg::conf_put(cx, "global.netplay.customserver", "");
  } else {
    mg::conf_put(cx, "global.netplay.relay", v);
  }
}

// ---- the lobby ------------------------------------------------------------------------------------------------------------------------

std::string lobby_url(const Ctx& cx) {
  std::string u = cx.conf.get_or("global.netplay.lobby", "");
  return u.empty() ? std::string(kLobbyUrl) : u;
}

bool parse_lobby(std::string_view js, bool relay_only, std::vector<Room>& out) {
  out.clear();
  json::Value v;
  if (!json::parse(js, v) || !v.is_array()) return false;
  for (const json::Value& item : v.arr) {
    const json::Value* f = item.is_object() ? item.find("fields") : nullptr;
    if (!f || !f->is_object()) continue;
    Room r;
    r.host_method = int(f->integer("host_method", 0));
    if (relay_only && r.host_method != 3) continue;
    r.core_name = f->text("core_name");
    r.username = f->text("username");
    r.game_crc = f->text("game_crc");
    r.mitm_ip = f->text("mitm_ip");
    r.subsystem_name = f->text("subsystem_name");
    r.frontend = f->text("frontend");
    r.created = f->text("created");
    r.ip = f->text("ip");
    r.updated = f->text("updated");
    r.country = f->text("country");
    r.game_name = f->text("game_name");
    r.mitm_session = f->text("mitm_session");
    r.retroarch_version = f->text("retroarch_version");
    r.has_password = f->boolean("has_password");
    r.has_spectate_password = f->boolean("has_spectate_password");
    r.fixed = f->boolean("fixed");
    r.mitm_port = int(f->integer("mitm_port", 0));
    r.port = int(f->integer("port", 0));
    out.push_back(std::move(r));
  }
  return true;
}

// GuiNetPlay.cpp `coreList` (the non-Windows one): lobby core name -> core id of this OS
std::string core_id(const std::string& name) {
  static const std::pair<const char*, const char*> kList[] = {
      {"Beetle NeoPop", "mednafen_ngp"}, {"Beetle PCE Fast", "pce"},    {"Beetle SuperGrafx", "mednafen_supergrafx"},
      {"Beetle VB", "vb"},               {"Beetle WonderSwan", "mednafen_wswan"}, {"DOSBox-SVN", "dosbox"},
      {"EightyOne", "81"},               {"FB Alpha 2012 Neo Geo", "fbalpha2012_neogeo"}, {"FB Alpha 2012", "fbalpha2012"},
      {"FB Alpha", "fbalpha"},           {"FinalBurn Neo", "fbneo"},    {"Game & Watch", "gw"},
      {"Genesis Plus GX", "genesisplusgx"}, {"MAME 2000", "imame4all"}, {"MAME 2003 (0.78)", "mame2003"},
      {"MAME 2003-Plus", "mame078plus"}, {"MAME 2010", "mame0139"},     {"MAME 2014", "mame2014"},
      {"PCSX-ReARMed", "pcsx_rearmed"},  {"Snes9x 2002", "pocketsnes"}, {"Snes9x 2010", "snes9x_next"},
      {"TGB Dual", "tgbdual"},           {"VICE x64", "vice"}};
  for (const auto& kv : kList)
    if (name == kv.first) return kv.second;
  std::string s = name;
  std::replace(s.begin(), s.end(), ' ', '_');
  return utf8_lower(s);
}

std::string remove_parentheses(const std::string& in) {
  static const char kRemove[4] = {'(', ')', '[', ']'};
  std::string s = in;
  bool done = false;
  while (!done) {
    done = true;
    for (int i = 0; i < 4; i += 2) {
      size_t end = s.find_first_of(kRemove[i + 1]);
      size_t start = s.find_last_of(kRemove[i], end);
      if (start != std::string::npos && end != std::string::npos) {
        s.erase(start, end - start + 1);
        done = false;
      }
    }
  }
  return std::string(trim(s));
}

static std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) s.replace(p, from.size(), to);
  return s;
}

std::string normalize_name(const std::string& name) {
  std::string r = utf8_lower(name);
  r = replace_all(r, "_", " ");
  r = replace_all(r, ".", "");
  r = replace_all(r, "'", "");
  return remove_parentheses(r);
}

bool core_exists(const LocalSystem& s, const std::string& core_name) {
  if (core_name.empty()) return false;
  const std::string id = core_id(core_name);
  return std::find(s.cores.begin(), s.cores.end(), id) != s.cores.end();
}

bool find_game(const Library& lib, const std::string& game_name, const std::string& core_name, int& sys, int& game) {
  const std::string low_core = utf8_lower(core_id(core_name));
  const std::string norm = normalize_name(game_name);
  const std::string norm_nospace = replace_all(norm, " ", "");
  for (size_t si = 0; si < lib.size(); ++si) {
    const LocalSystem& s = lib[si];
    bool has_core = false;  // ES: only systems that have the room's core (by lower case name)
    for (const std::string& c : s.cores) has_core = has_core || utf8_lower(c) == low_core;
    if (!has_core) continue;
    for (size_t gi = 0; gi < s.games.size(); ++gi) {
      const LocalGame& g = s.games[gi];
      const std::string by_name = normalize_name(g.name);
      if (by_name == norm || normalize_name(path_stem(g.path)) == norm || replace_all(by_name, " ", "") == norm_nospace) {
        sys = int(si);
        game = int(gi);
        return true;
      }
    }
  }
  return false;
}

std::vector<LobbyEntry> build_entries(const std::vector<Room>& rooms, const Library& lib,
                                 const std::function<std::string(const LocalSystem&, const std::string&)>& crc_of) {
  std::vector<LobbyEntry> out;
  out.reserve(rooms.size());
  for (const Room& r : rooms) {
    LobbyEntry e;
    e.room = r;
    int s = -1, g = -1;
    if (find_game(lib, r.game_name, r.core_name, s, g)) {
      e.sys = s;
      e.game = g;
      const LocalSystem& ls = lib[size_t(s)];
      e.core_ok = core_exists(ls, r.core_name);
      std::string crc = crc_of ? crc_of(ls, ls.games[size_t(g)].path) : std::string();
      e.crc_same = !crc.empty() && utf8_upper(crc) == utf8_upper(r.game_crc);
    }
    out.push_back(std::move(e));
  }
  // ES sorts "same ROM first, then core available" with a comparator that is not a strict ordering; this is the ordering it means
  std::stable_sort(out.begin(), out.end(), [](const LobbyEntry& a, const LobbyEntry& b) {
    if (a.crc_same != b.crc_same) return a.crc_same;
    return a.core_ok && !b.core_ok;
  });
  return out;
}

std::string state_text(const LobbyEntry& e) {
  std::string s;
  if (e.sys >= 0) {
    if (e.crc_same) s = tr("SAME ROM");
    else if (e.room.game_crc == "00000000") s = tr("UNKNOWN ROM VERSION");
    else s = tr("DIFFERENT ROM");
    if (!e.core_ok) s += std::string("  ") + tr("UNAVAILABLE CORE");
  }
  return s;
}

std::vector<Shown> layout_rows(const std::vector<LobbyEntry>& entries, bool show_missing) {
  std::vector<Shown> out;
  bool any = false;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].sys < 0) continue;
    if (!show_missing && !entries[i].core_ok) continue;
    out.push_back({i, Group::Available, !any});
    any = true;
  }
  if (show_missing) {
    bool first = true;
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i].sys >= 0) continue;
      out.push_back({i, Group::Unavailable, first});
      first = false;
    }
  }
  return out;
}

// ---- library --------------------------------------------------------------------------------------------------------------------------

Library snapshot_library(Ctx& cx) {
  Library lib;
  for (const System& s : cx.systems) {
    if (!system_supported(s)) continue;
    LocalSystem ls;
    ls.name = s.name;
    ls.rom_dir = s.rom_dir;
    ls.gamelist = s.gamelist;
    ls.platform = s.platform;
    ls.exts = s.exts;
    for (const auto& e : s.emus)
      for (const std::string& c : e.second) ls.cores.push_back(c);
    if (s.loaded || !s.games.empty()) {
      ls.games.reserve(s.games.size());
      for (const Game& g : s.games) ls.games.push_back({g.name, g.path});
    } else {
      ls.need_load = true;
    }
    lib.push_back(std::move(ls));
  }
  return lib;
}

void load_library(Library& lib) {
  for (LocalSystem& s : lib) {
    if (!s.need_load) continue;
    for (const Game& g : load_games(s.rom_dir, s.exts, s.gamelist)) s.games.push_back({g.name, g.path});
    s.need_load = false;
  }
}

// ---- fetch ----------------------------------------------------------------------------------------------------------------------------

bool fetch_lobby(Services& sv, const std::string& url, std::string& body, std::string& err) {
  body.clear();
  err.clear();
  if (!starts_with(url, "http://") && !starts_with(url, "https://")) {
    err = std::string(tr("FAILED")) + " : " + url;
    return false;
  }
  const char* zc = std::getenv("ZF_CURL");
  std::string curl = (zc && *zc && ::access(zc, X_OK) == 0) ? zc : "curl";
  const char* base = std::getenv("ZF_NP_TMP");
  std::string tmpl = std::string(base && *base ? base : "/tmp") + "/zf-np-XXXXXX";
  std::vector<char> t(tmpl.begin(), tmpl.end());
  t.push_back('\0');
  if (!::mkdtemp(t.data())) {
    err = tr("NO NETWORK CONNECTION");
    return false;
  }
  const std::string dir = t.data(), out = dir + "/lobby.json";
  CmdResult c = sv.cmd.run(shell_quote(curl) + " -s -f -L -m 30 -o " + shell_quote(out) + " " + shell_quote(url) + " 2>/dev/null", 40000);
  bool ok = c.dry ? false : c.exit_code == 0 && read_file(out, body);
  ::unlink(out.c_str());
  ::rmdir(dir.c_str());
  if (!ok && !c.dry) err = std::string(tr("FAILED")) + " : " + (c.exit_code < 0 || c.exit_code == 6 || c.exit_code == 7 || c.exit_code == 28 ? std::string(tr("NO NETWORK CONNECTION")) : "curl " + std::to_string(c.exit_code));
  return ok;
}

// ---- launch ---------------------------------------------------------------------------------------------------------------------------

Transition launch_game(Ctx& cx, size_t sys, size_t game, const launch::Netplay& np) {
  if (sys >= cx.systems.size()) return Transition::none();
  cx.open_system(sys);
  if (game >= cx.systems[sys].games.size()) return Transition::none();
  if (np.role != launch::NetRole::Off && launch::netplay_args(np, cx.conf.map()).empty()) {
    cx.sv->toasts.push(tr("BAD COMMAND LINE ARGUMENTS"), 4000);
    return Transition::none();
  }
  ensure_nickname(cx);  // before the command is built: ES writes the default nick first
  const System& s = cx.systems[sys];
  const Game& g = s.games[game];
  std::vector<JoyInfo> joys;
  if (cx.pad) joys = cx.pad->joysticks();
  std::string cmd = launch::build_command(s, g, cx.conf.map(), joys, np);
  if (cmd.empty()) return Transition::none();
  cmd += savestates::plain_args(cx, sys, game);
  Transition t = Transition::launch(cmd);
  t.sys = int(sys);
  t.game = int(game);
  return launchcheck::guard(cx, sys, std::move(t));
}

Transition launch_path(Ctx& cx, const std::string& system, const std::string& rom_path, const launch::Netplay& np) {
  for (size_t i = 0; i < cx.systems.size(); ++i) {
    if (cx.systems[i].name != system) continue;
    cx.open_system(i);
    for (size_t j = 0; j < cx.systems[i].games.size(); ++j)
      if (cx.systems[i].games[j].path == rom_path) return launch_game(cx, i, j, np);
  }
  cx.sv->toasts.push(tr("UNKNOWN GAME"), 3000);
  return Transition::none();
}

}  // namespace zf::netplay
