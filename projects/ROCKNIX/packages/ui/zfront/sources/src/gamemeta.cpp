#include "gamemeta.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "app.hpp"
#include "collections.hpp"
#include "colsys.hpp"
#include "gamelist_game.hpp"
#include "i18n.hpp"
#include "util.hpp"

namespace zf {

std::string iso_local(time_t t) {
  struct tm tm;
  ::localtime_r(&t, &tm);
  char iso[32];
  std::strftime(iso, sizeof iso, "%Y%m%dT%H%M%S", &tm);
  return iso;
}

uint64_t pack_lastplayed(const std::string& iso) {
  uint64_t v = 0;
  int n = 0;
  for (char c : iso)
    if (c >= '0' && c <= '9') {
      v = v * 10 + uint64_t(c - '0');
      ++n;
    }
  return n == 14 ? v : 0;
}

namespace {

struct Result {
  bool ok = false;
  std::string err;
  GamelistEdit edit;
};

// The edited game after a job: found again by path, `hint` first (nothing moves in practice).
Game* find_game(System& s, size_t hint, const std::string& path) {
  if (hint < s.games.size() && s.games[hint].path == path) return &s.games[hint];
  for (Game& g : s.games)
    if (g.path == path) return &g;
  return nullptr;
}

uint32_t add_capped(uint32_t cur, uint64_t add) { return uint32_t(std::min<uint64_t>(uint64_t(cur) + add, 0x7fffffffu)); }

// A game that had no `<game>` element got one: its span, so the details (description, images) are readable at once.
void set_span(System& s, size_t hint, const std::string& path, const GamelistEdit& e) {
  if (e.elem_end <= e.elem_begin || e.elem_end > 0xffffffffull) return;
  Game* g = find_game(s, hint, path);
  if (g && g->meta.span_end == 0) {
    g->meta.span_begin = uint32_t(e.elem_begin);
    g->meta.span_end = uint32_t(e.elem_end);
  }
}

}  // namespace

void game_played(Ctx& cx, size_t sys, size_t game, uint64_t secs, time_t now) {
  if (sys >= cx.systems.size() || game >= cx.systems[sys].games.size()) return;
  System& s = cx.systems[sys];
  Game& g = s.games[game];
  std::string iso = iso_local(now ? now : ::time(nullptr));
  g.meta.playcount = add_capped(g.meta.playcount, 1);
  g.meta.lastplayed = pack_lastplayed(iso);
  g.meta.gametime = add_capped(g.meta.gametime, secs);
  games_changed();  // LAST PLAYED, NEVER PLAYED and the marks of an open list follow

  auto res = std::make_shared<Result>();
  std::string xml = s.gamelist, dir = s.rom_dir, path = g.path;
  Ctx* c = &cx;  // the services (and so the job queue) die with `cx`: `done` never runs after it
  cx.sv->jobs.post(
      [=] { res->ok = gamelist_update_after_play(xml, dir, path, iso, res->err, &res->edit, secs); },
      [=] {
        if (sys >= c->systems.size()) return;
        if (res->ok) {
          c->systems[sys].apply_edit(res->edit);
          set_span(c->systems[sys], game, path, res->edit);
        } else {
          std::fprintf(stderr, "zfront: gamelist not updated for %s%s%s\n", path.c_str(), res->err.empty() ? "" : ": ", res->err.c_str());
          c->sv->toasts.push(tr("COULD NOT SAVE GAME DATA"), 4000);
        }
      });
}

void game_set_favorite(Ctx& cx, size_t sys, size_t game, bool fav) {
  if (sys >= cx.systems.size() || game >= cx.systems[sys].games.size()) return;
  System& s = cx.systems[sys];
  Game& g = s.games[game];
  bool before = g.meta.favorite;
  g.meta.favorite = fav;
  games_changed();  // FAVORITES and an open collection list follow

  auto res = std::make_shared<Result>();
  std::string xml = s.gamelist, dir = s.rom_dir, path = g.path;
  Ctx* c = &cx;
  cx.sv->jobs.post(
      [=] { res->ok = gamelist_set_favorite(xml, dir, path, fav, res->err, &res->edit); },
      [=] {
        if (sys >= c->systems.size()) return;
        System& sy = c->systems[sys];
        if (res->ok) {
          sy.apply_edit(res->edit);
          set_span(sy, game, path, res->edit);
          return;
        }
        std::fprintf(stderr, "zfront: favourite not saved for %s%s%s\n", path.c_str(), res->err.empty() ? "" : ": ", res->err.c_str());
        if (Game* gg = find_game(sy, game, path)) gg->meta.favorite = before;
        games_changed();
        c->sv->toasts.push(tr("COULD NOT SAVE GAME DATA"), 4000);
      });
}

// ---- generation ------------------------------------------------------------------------------------------------------------
namespace {
unsigned g_games_gen = 0;

long game_index(const System& s, const std::string& path) {
  for (size_t i = 0; i < s.games.size(); ++i)
    if (s.games[i].path == path) return long(i);
  return -1;
}
}  // namespace

unsigned games_generation() { return g_games_gen; }
void games_changed() {
  ++g_games_gen;
  colsys::request();  // the members of the collections shown in the carousel may have changed
}

// ---- edit form -------------------------------------------------------------------------------------------------------------
bool valid_release_date(const std::string& d) {
  if (d.empty()) return true;
  if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
  for (size_t i = 0; i < d.size(); ++i)
    if (i != 4 && i != 7 && (d[i] < '0' || d[i] > '9')) return false;
  int y = std::atoi(d.substr(0, 4).c_str()), m = std::atoi(d.substr(5, 2).c_str()), day = std::atoi(d.substr(8, 2).c_str());
  return y >= 1 && m >= 1 && m <= 12 && day >= 1 && day <= 31;
}

namespace {

bool truthy(const std::string& v) { return v == "true" || v == "TRUE" || v == "True" || v == "1"; }

// `19910823T000000` -> `1991-08-23`; anything that is not a date -> ""
std::string date_from_xml(const std::string& raw) {
  std::string digits;
  for (char c : raw) {
    if (c == 'T') break;
    if (c >= '0' && c <= '9') digits.push_back(c);
  }
  if (digits.size() < 8) return std::string();
  std::string d = digits.substr(0, 4) + "-" + digits.substr(4, 2) + "-" + digits.substr(6, 2);
  return valid_release_date(d) ? d : std::string();
}

std::string date_to_xml(const std::string& d) { return d.empty() ? std::string() : d.substr(0, 4) + d.substr(5, 2) + d.substr(8, 2) + "T000000"; }

}  // namespace

bool game_form_load(const Ctx& cx, size_t sys, const std::string& path, GameForm& out) {
  if (sys >= cx.systems.size()) return false;
  const System& s = cx.systems[sys];
  long i = game_index(s, path);
  if (i < 0) return false;
  const Game& g = s.games[size_t(i)];
  auto f = gamelist_read_fields(s.gamelist, s.rom_dir, g.path, g.meta, {"desc", "genre", "players", "developer", "publisher", "releasedate", "sortname"});
  auto get = [&](const char* k) {
    auto it = f.find(k);
    return it == f.end() ? std::string() : it->second;
  };
  GameForm r;
  r.name = g.name;
  r.desc = get("desc");
  r.genre = get("genre");
  r.players = get("players");
  r.developer = get("developer");
  r.publisher = get("publisher");
  r.releasedate = date_from_xml(get("releasedate"));
  r.sortname = get("sortname");
  r.kidgame = g.meta.kid;
  r.rating = (int(g.meta.rating_pct) + 5) / 10;
  r.favorite = g.meta.favorite;
  r.hidden = g.meta.hidden;
  out = std::move(r);
  return true;
}

void game_edit(Ctx& cx, size_t sys, const std::string& path, const GameForm& before, const GameForm& after) {
  if (sys >= cx.systems.size() || before == after) return;
  System& s = cx.systems[sys];
  long gi = game_index(s, path);
  if (gi < 0) return;
  Game& g = s.games[size_t(gi)];
  std::vector<std::pair<std::string, std::string>> fields;
  auto text = [&](const char* tag, const std::string& a, const std::string& b) {
    if (a != b) fields.push_back({tag, b});
  };
  text("name", before.name, after.name);
  text("desc", before.desc, after.desc);
  text("genre", before.genre, after.genre);
  text("players", before.players, after.players);
  text("developer", before.developer, after.developer);
  text("publisher", before.publisher, after.publisher);
  text("sortname", before.sortname, after.sortname);
  if (before.kidgame != after.kidgame) fields.push_back({"kidgame", after.kidgame ? "true" : "false"});  // ES MetaData.cpp:83
  if (before.releasedate != after.releasedate) fields.push_back({"releasedate", date_to_xml(after.releasedate)});
  if (before.rating != after.rating) {
    char b[32];
    std::snprintf(b, sizeof b, "%f", double(after.rating) / 10.0);  // ES: "0.700000"
    fields.push_back({"rating", b});
  }
  if (before.favorite != after.favorite) fields.push_back({"favorite", after.favorite ? "true" : "false"});
  if (before.hidden != after.hidden) fields.push_back({"hidden", after.hidden ? "true" : "false"});
  // memory (what it was is kept: a failed write puts it back, as `game_set_favorite` does)
  struct Mem {
    std::string name;
    std::string_view genre, players;
    int32_t release_year;
    uint8_t rating_pct;
    bool favorite, hidden, kid;
  };
  const Mem old{g.name, g.meta.genre, g.meta.players, g.meta.release_year, g.meta.rating_pct, g.meta.favorite, g.meta.hidden, g.meta.kid};
  g.meta.kid = after.kidgame;
  g.name = after.name.empty() ? path_stem(g.path) : after.name;
  g.meta.genre = after.genre.empty() ? std::string_view() : gamelist_intern(after.genre);
  g.meta.players = after.players.empty() ? std::string_view() : gamelist_intern(after.players);
  g.meta.release_year = after.releasedate.empty() ? -1 : std::atoi(after.releasedate.substr(0, 4).c_str());
  g.meta.rating_pct = uint8_t(std::clamp(after.rating, 0, 10) * 10);
  g.meta.favorite = after.favorite;
  g.meta.hidden = after.hidden;
  games_changed();

  auto res = std::make_shared<Result>();
  std::string xml = s.gamelist, dir = s.rom_dir, rom = g.path;
  Ctx* c = &cx;
  const size_t hint = size_t(gi);
  cx.sv->jobs.post([=] { res->ok = gamelist_set_fields(xml, dir, rom, fields, res->err, &res->edit); },
                   [=] {
                     if (sys >= c->systems.size()) return;
                     if (res->ok) {
                       c->systems[sys].apply_edit(res->edit);
                       set_span(c->systems[sys], hint, rom, res->edit);
                       games_changed();
                       return;
                     }
                     std::fprintf(stderr, "zfront: game data not saved for %s%s%s\n", rom.c_str(), res->err.empty() ? "" : ": ", res->err.c_str());
                     if (Game* gg = find_game(c->systems[sys], hint, rom)) {  // the list must not show what is not on disk
                       gg->name = old.name;
                       gg->meta.genre = old.genre;
                       gg->meta.players = old.players;
                       gg->meta.release_year = old.release_year;
                       gg->meta.rating_pct = old.rating_pct;
                       gg->meta.favorite = old.favorite;
                       gg->meta.hidden = old.hidden;
                       gg->meta.kid = old.kid;
                     }
                     games_changed();
                     c->sv->toasts.push(tr("COULD NOT SAVE GAME DATA"), 4000);
                   });
}

// ---- delete ----------------------------------------------------------------------------------------------------------------
namespace {

std::string real_path(const std::string& p) {
  char buf[PATH_MAX];
  return ::realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}

bool has_dotdot(const std::string& p) {
  size_t i = 0;
  while (i <= p.size()) {
    size_t j = p.find('/', i);
    if (j == std::string::npos) j = p.size();
    if (p.compare(i, j - i, "..") == 0 && j - i == 2) return true;
    i = j + 1;
  }
  return false;
}

/// `path` is a file the delete may remove: inside `rom_dir` (lexically and once the symlinks of its folder are resolved), never `rom_dir`
/// itself, not a directory. `exists` tells whether there is something to remove.
bool deletable(const std::string& rom_dir, const std::string& root_real, const std::string& path, bool* exists, std::string* why) {
  auto no = [&](const char* m) {
    if (why) *why = m;
    return false;
  };
  *exists = false;
  std::string pre = rom_dir.back() == '/' ? rom_dir : rom_dir + "/";
  if (path.size() <= pre.size() || path.compare(0, pre.size(), pre) != 0) return no("outside the ROM folder");
  if (has_dotdot(path)) return no("path with ..");
  size_t slash = path.find_last_of('/');
  std::string parent = real_path(path.substr(0, slash));
  if (parent.empty()) return true;  // the folder is gone: nothing to remove
  if (parent != root_real && parent.compare(0, root_real.size() + 1, root_real + "/") != 0) return no("folder resolves outside the ROM folder");
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) return true;
  if (S_ISDIR(st.st_mode)) return no("is a directory");
  *exists = true;
  return true;
}

}  // namespace

// Another entry of the gamelist still names this file (two games sharing one picture): it stays.
static bool still_named(const std::string& xml_text, std::string dir, const std::string& file) {
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  if (file.size() <= dir.size() + 1 || file.compare(0, dir.size(), dir) != 0) return false;
  const std::string rel = file.substr(dir.size() + 1);
  std::string some, all;  // with the escapes a writer must use, and with every one it may
  for (char ch : rel) {
    const char* e = ch == '&' ? "&amp;" : ch == '<' ? "&lt;" : ch == '>' ? "&gt;" : nullptr;
    const char* f = e ? e : ch == '\'' ? "&apos;" : ch == '"' ? "&quot;" : nullptr;
    if (e) some += e; else some += ch;
    if (f) all += f; else all += ch;
  }
  return xml_text.find(rel) != std::string::npos || xml_text.find(some) != std::string::npos || xml_text.find(all) != std::string::npos;
}

DeleteResult game_delete(Ctx& cx, size_t sys, const std::string& path, std::string& why) {
  auto refuse = [&](const char* m) {
    why = m;
    std::fprintf(stderr, "zfront: delete refused for %s: %s\n", path.c_str(), m);
    return DeleteResult::Refused;
  };
  if (sys >= cx.systems.size() || path.empty()) return refuse("no such game");
  System& s = cx.systems[sys];
  long gi = game_index(s, path);
  if (gi < 0) return refuse("game not in the system");
  if (s.rom_dir.empty() || s.rom_dir == "/") return refuse("ROM folder not set");
  const std::string root_real = real_path(s.rom_dir);
  if (root_real.empty() || root_real == "/") return refuse("ROM folder not valid");
  bool rom_exists = false;
  std::string reason;
  if (!deletable(s.rom_dir, root_real, path, &rom_exists, &reason)) return refuse(reason.c_str());

  // the files: the ROM, then the media the gamelist names (the paths of every media field of ES's metadata)
  std::vector<std::string> files;
  if (rom_exists) files.push_back(path);
  const Game& g = s.games[size_t(gi)];
  static const std::vector<std::string> kMedia = {"image", "video", "marquee", "thumbnail", "fanart", "titleshot", "manual", "magazine", "map",
                                                  "cartridge", "boxart", "boxback", "wheel", "mix"};
  for (const auto& kv : gamelist_read_fields(s.gamelist, s.rom_dir, g.path, g.meta, kMedia)) {
    if (kv.second.empty()) continue;
    std::string m = resolve_rel_path(kv.second, s.rom_dir);
    bool ex = false;
    if (m == path || !deletable(s.rom_dir, root_real, m, &ex, nullptr) || !ex) continue;  // outside the folder, a directory or not there: left alone
    if (std::find(files.begin(), files.end(), m) == files.end()) files.push_back(m);
  }

  // memory at once
  s.games.erase(s.games.begin() + gi);
  if (s.count > 0) --s.count;
  games_changed();

  struct Out {
    bool ok = true, collections = false, listed = false;
    std::string err;
    GamelistEdit edit;
  };
  auto res = std::make_shared<Out>();
  std::string xml = s.gamelist, dir = s.rom_dir;
  Ctx* c = &cx;
  cx.sv->jobs.post(
      [=] {
        // the list first: a game that cannot be taken out of it keeps its files (it would come back as a broken entry)
        std::string e;
        if (!gamelist_remove_game(xml, dir, path, e, &res->edit)) {
          res->ok = false;
          res->listed = true;
          res->err = e;
          return;
        }
        std::string left;
        read_file(xml, left);
        for (const std::string& f : files) {
          if (f != path && still_named(left, dir, f)) continue;
          if (::unlink(f.c_str()) != 0 && errno != ENOENT) {
            res->ok = false;
            res->err = f + ": cannot remove";
          }
        }
        for (const collections::Custom& cu : collections::list_custom()) {
          if (cu.dynamic) continue;
          bool changed = false;
          std::string e;
          if (collections::contains(cu.name, path) && collections::remove_game(cu.name, path, e, &changed) && changed) res->collections = true;
        }
      },
      [=] {
        if (sys < c->systems.size()) c->systems[sys].apply_edit(res->edit);
        if (res->collections) collections::set_reload_needed(true);
        if (res->listed) collections::set_rescan_needed(true);  // nothing was deleted: the game comes back
        games_changed();
        if (!res->ok) {
          std::fprintf(stderr, "zfront: delete incomplete for %s: %s\n", path.c_str(), res->err.c_str());
          c->sv->toasts.push(tr("COULD NOT SAVE GAME DATA"), 4000);
        }
      });
  return DeleteResult::Ok;
}

}  // namespace zf
