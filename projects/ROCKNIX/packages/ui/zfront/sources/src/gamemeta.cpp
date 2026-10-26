#include "gamemeta.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "app.hpp"

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

}  // namespace

void game_played(Ctx& cx, size_t sys, size_t game, uint64_t secs, time_t now) {
  if (sys >= cx.systems.size() || game >= cx.systems[sys].games.size()) return;
  System& s = cx.systems[sys];
  Game& g = s.games[game];
  std::string iso = iso_local(now ? now : ::time(nullptr));
  g.meta.playcount = add_capped(g.meta.playcount, 1);
  g.meta.lastplayed = pack_lastplayed(iso);
  g.meta.gametime = add_capped(g.meta.gametime, secs);

  auto res = std::make_shared<Result>();
  std::string xml = s.gamelist, dir = s.rom_dir, path = g.path;
  Ctx* c = &cx;  // the services (and so the job queue) die with `cx`: `done` never runs after it
  cx.sv->jobs.post(
      [=] { res->ok = gamelist_update_after_play(xml, dir, path, iso, res->err, &res->edit, secs); },
      [=] {
        if (sys >= c->systems.size()) return;
        if (res->ok) {
          c->systems[sys].apply_edit(res->edit);
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
          return;
        }
        std::fprintf(stderr, "zfront: favourite not saved for %s%s%s\n", path.c_str(), res->err.empty() ? "" : ": ", res->err.c_str());
        if (Game* gg = find_game(sy, game, path)) gg->meta.favorite = before;
        c->sv->toasts.push(tr("COULD NOT SAVE GAME DATA"), 4000);
      });
}

}  // namespace zf
