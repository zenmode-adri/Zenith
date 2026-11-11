#include "detailed.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <unordered_set>

#include "covers.hpp"
#include "gamelist.hpp"
#include "gamemeta.hpp"
#include "remake.hpp"
#include "services.hpp"
#include "util.hpp"
#include "video.hpp"

namespace zf {

namespace {

/// A game file that is itself a picture (the Screenshots system): shown as its own image when the gamelist has none,
/// as ES does.
bool is_picture(const std::string& path) {
  static const char* const kExt[] = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp"};
  const std::string e = path_ext_lower(path);
  for (const char* x : kExt)
    if (e == x) return true;
  return false;
}

bool g_demo = false;

/// Cap of the wrapped description (lines). A longer text ends with an ellipsis; 128 lines are ~4 MB at 720x720.
constexpr size_t kMaxDescLines = 128;
constexpr float kDescSpacing = 1.3f;  // md_description lineSpacing of the theme
constexpr size_t kDetailsCache = 16;  // a screen of rows with pictures (XMB)

/// Fit an image of `iw x ih` inside `mw x mh` keeping its aspect ratio (up or down, like ES `maxSize`).
void fit(float iw, float ih, float mw, float mh, float& ow, float& oh) {
  float s = std::min(mw / iw, mh / ih);
  ow = iw * s;
  oh = ih * s;
}

void centred_text(Assets& a, std::vector<Op>& ops, std::string_view s, float px, Color color, float x, float y, float w, float h) {
  ImgId id = a.text_ellipsis(s, px, color, w);
  float tw = float(a.img(id).w), th = float(a.img(id).h);
  ops.push_back(Op::image(id, x, std::floor(y + h / 2.0f - th / 2.0f + 0.5f), tw, th));
}

/// Genre row: like `centred_text` while it fits; wider than its box it is cut at the edge and moves (`genre_scroll_offset`).
void genre_text(Assets& a, std::vector<Op>& ops, const DetailedData& d, std::string_view s, float px, Color color, float x,
                float y, float w, float h) {
  ImgId id = a.text(s, px, color);
  float tw = float(a.img(id).w), th = float(a.img(id).h);
  if (tw <= w) {
    centred_text(a, ops, s, px, color, x, y, w, h);
    return;
  }
  d.genre_w = tw;
  d.genre_speed = px * 4.0f;  // like ES: the alphabet in about 4 s
  float gy = std::floor(y + h / 2.0f - th / 2.0f + 0.5f);
  float off = d.desc_time < 0 ? 0.0f : float(genre_scroll_offset(d.desc_time, tw, d.genre_speed));
  ops.push_back(Op::clip(x, gy, w, th));
  ops.push_back(Op::image(id, x - off, gy, tw, th));
  float again = x - off + tw + d.genre_speed * float(kGenreGap);
  if (again < x + w) ops.push_back(Op::image(id, again, gy, tw, th));
  ops.push_back(Op::unclip());
}

/// Five stars at `x,y` (`sh` tall); `rating` in 0..1, halves are cut like ES. 0 = five empty stars.
void stars(const Theme& th, Assets& a, std::vector<Op>& ops, float x, float y, float sh, float rating) {
  Color cyan = th.color("cyan");
  uint32_t hpx = uint32_t(std::max(std::lround(sh), 1L));
  ImgId off = 0, on = 0;
  if (!a.svg(th.path("./assets/icons/icon-star-unfilled.svg"), hpx, off) || !a.svg(th.path("./assets/icons/icon-star-filled.svg"), hpx, on)) return;
  a.pin(off);
  a.pin(on);
  float sw = float(a.img(off).w);
  float value = std::round(std::min(std::max(rating, 0.0f), 1.0f) * 5.0f * 2.0f) / 2.0f;
  for (int i = 0; i < 5; ++i) {
    float sx = x + float(i) * sw;
    float fill = std::min(std::max(value - float(i), 0.0f), 1.0f);
    if (fill >= 1.0f) {
      ops.push_back(Op::image(on, sx, y, sw, sh, cyan));
    } else {
      ops.push_back(Op::image(off, sx, y, sw, sh, cyan));
      if (fill > 0.0f) {
        ops.push_back(Op::clip(sx, y, sw * fill, sh));
        ops.push_back(Op::image(on, sx, y, sw, sh, cyan));
        ops.push_back(Op::unclip());
      }
    }
  }
}

struct CachedDetails {
  std::string key;
  GameDetails det;
  uint64_t used = 0;
};
std::vector<CachedDetails> g_details;
uint64_t g_tick = 0;
unsigned g_details_gen = 0;  // games_generation() the entries were read at

const GameDetails& details_cached(const System& sys, const Game& g) {
  if (g_details_gen != games_generation()) {  // a game was edited, scraped or deleted: its description and pictures may have changed
    g_details.clear();
    g_details_gen = games_generation();
  }
  std::string key = sys.gamelist;
  key.push_back('\n');
  key += g.path;
  ++g_tick;
  for (CachedDetails& c : g_details)
    if (c.key == key) {
      c.used = g_tick;
      return c.det;
    }
  GameDetails det = sys.gamelist.empty() ? GameDetails{} : gamelist_details(sys.gamelist, sys.rom_dir, g.path, g.meta);
  if (g_details.size() < kDetailsCache) {
    g_details.push_back({std::move(key), std::move(det), g_tick});
    return g_details.back().det;
  }
  CachedDetails* oldest = &g_details[0];
  for (CachedDetails& c : g_details)
    if (c.used < oldest->used) oldest = &c;
  *oldest = {std::move(key), std::move(det), g_tick};
  return oldest->det;
}

std::string demo_cover(const std::string& name) {
  // ZF_DEMO_COVER_DIR: `<dir>/<0..23>.png` per game (distinct files, so the bench decodes a cover per game)
  if (std::getenv("ZF_NOCOVER")) return {};
  // "Game n" (bench) -> <dir>/n.png, else a hash: distinct files, so the bench decodes a different cover per game
  if (const char* dir = std::getenv("ZF_DEMO_COVER_DIR")) {
    size_t n = name.rfind("Game ", 0) == 0 ? size_t(std::atol(name.c_str() + 5)) : std::hash<std::string>()(name) % 24;
    return std::string(dir) + "/" + std::to_string(n) + ".png";
  }
  if (const char* e = std::getenv("ZF_DEMO_COVER")) return e;
  if (std::getenv("ZF_NOCOVER")) return {};
  // tests/data/cover-box.png next to this source tree
  return path_dir(path_dir(__FILE__)) + "/tests/data/cover-box.png";
}

/// Sample data for the harness: the Zelda entry of the reference capture; the others vary so that the omitted-row paths
/// are exercised.
void demo_data(const std::string& name, DetailedData& d) {
  std::string image = demo_cover(name);
  d.name = name;
  if (name.rfind("Zelda", 0) == 0) {
    d.image = image;
    d.desc =
        "Link's latest challenge is set on the mysterious Koholint, a place where dreams and reality collide. As a castaway, Link must find a way to escape "
        "from the island and return to his beloved homeland of Hyrule. This will not be an easy task: he must wake the legendary Wind Fish by collecting "
        "eight instruments scattered through the dungeons of the island.";
    d.year = 1998;
    d.genre = "Adventure";
    d.players = "1";
    d.rating = 1.0f;
    return;
  }
  if (name.rfind("Game ", 0) == 0) {  // bench game: everything present, description of 1-3 kB (deterministic)
    static const char* const kWords[] = {"the", "island", "hero", "must", "find", "a", "way", "through", "dark", "dungeons", "while", "ancient", "magic", "wakes",
                                         "and", "enemies", "gather", "to", "stop", "journey", "across", "kingdom", "of", "legendary", "sword", "castle", "friends"};
    uint32_t x = uint32_t(std::atol(name.c_str() + 5)) * 2654435761u + 12345u;
    size_t words = 150 + (x >> 8) % 300;
    d.desc.clear();
    for (size_t i = 0; i < words; ++i) {
      x = x * 1664525u + 1013904223u;
      if (i) d.desc += (i % 17 == 0) ? ". " : " ";
      d.desc += kWords[(x >> 16) % (sizeof kWords / sizeof *kWords)];
    }
    d.desc += ".";
    d.image = image;
    d.year = 1990 + int32_t(x % 20);
    d.genre = "Platform";
    d.players = "1-2";
    d.rating = float((x >> 4) % 11) / 10.0f;
    return;
  }
  size_t k = name.size();
  d.image = k % 3 == 0 ? std::string() : image;
  d.desc = k % 4 == 0 ? std::string() : "A short description of the game.";
  d.year = k % 2 == 0 ? int32_t(1990 + k % 10) : -1;
  d.genre = k % 5 == 0 ? std::string() : "Platform";
  d.players = "1-2";
  d.rating = k % 3 == 1 ? 0.0f : 0.7f;
}

}  // namespace

void detailed_cache_clear() {
  std::vector<CachedDetails>().swap(g_details);  // also gives the buffer back
  g_tick = 0;
}

size_t detailed_cache_size() { return g_details.size(); }

int desc_scroll_offset(double t, int max_off) {
  if (max_off <= 0 || t < kDescDelay) return 0;
  double period = kDescDelay + kDescHold + double(max_off - 1) * kDescStep;
  double tc = std::fmod(t, period);
  if (tc < kDescDelay) return 0;
  long k = long(std::floor((tc - kDescDelay) / kDescStep)) + 1;
  return int(std::min<long>(k, max_off));
}

double desc_scroll_next(double t, int max_off) {
  if (max_off <= 0) return std::numeric_limits<double>::infinity();
  double period = kDescDelay + kDescHold + double(max_off - 1) * kDescStep;
  double tc = t < 0 ? 0 : std::fmod(t, period);
  if (tc < kDescDelay) return kDescDelay - tc;
  long k = long(std::floor((tc - kDescDelay) / kDescStep)) + 1;
  if (k >= max_off) return period - tc;  // holding at the end until it jumps back
  return std::max(kDescDelay + double(k) * kDescStep - tc, 1e-4);
}

int genre_scroll_offset(double t, float w, float speed) {
  if (w <= 0 || t < kGenreDelay) return 0;
  double period = kGenreLoopDelay + double(w) / double(speed) + kGenreGap;
  if (t - kGenreDelay >= period * kGenreLoops) return 0;
  double tc = std::fmod(t - kGenreDelay, period);
  if (tc < kGenreLoopDelay) return 0;
  return int(std::floor((tc - kGenreLoopDelay) * double(speed)));
}

double genre_scroll_next(double t, float w, float speed) {
  if (w <= 0) return std::numeric_limits<double>::infinity();
  double px = 1.0 / double(speed);
  if (t < kGenreDelay) return kGenreDelay - t + kGenreLoopDelay + px;
  double period = kGenreLoopDelay + double(w) / double(speed) + kGenreGap;
  if (t - kGenreDelay >= period * kGenreLoops) return std::numeric_limits<double>::infinity();
  double tc = std::fmod(t - kGenreDelay, period);
  if (tc < kGenreLoopDelay) return kGenreLoopDelay - tc + px;
  double moved = tc - kGenreLoopDelay;
  double next = (std::floor(moved * double(speed)) + 1.0) * px - moved;
  return std::max(std::min(next, period - tc), 1e-4);
}

std::string detailed_image(const System& sys, size_t idx) {
  if (idx >= sys.games.size()) return {};
  return detailed_image(sys, sys.games[idx]);
}

std::string detailed_image(const System& sys, const Game& g) {
  if (g_demo) return {};
  const GameDetails& det = details_cached(sys, g);
  return det.image.empty() ? det.thumbnail : det.image;
}

bool detailed_enabled() { return true; }

void detailed_set_demo(bool on) { g_demo = on; }

bool detailed_for(const System& sys, size_t idx, bool settled, DetailedData& out) {
  if (idx >= sys.games.size()) return false;
  return detailed_for(sys, sys.games[idx], settled, out);
}

bool detailed_for(const System& sys, const Game& g, bool settled, DetailedData& out) {
  out.desc_time = -1;
  out.show_rating = sys.theme != "imageviewer";
  if (g_demo) {
    demo_data(g.name, out);
  } else {
    out.name = g.name;
    out.year = g.meta.release_year;
    out.genre = g.meta.genre;
    out.players = g.meta.players;
    out.rating = g.meta.rating();
    out.image.clear();
    out.desc.clear();
    out.video.clear();
    if (settled) {
      const GameDetails& det = details_cached(sys, g);
      out.image = det.image.empty() ? det.thumbnail : det.image;
      out.video = det.video;
      if (out.image.empty() && is_picture(g.path)) out.image = g.path;
      out.desc = det.desc;
    }
  }
  if (!settled) {  // cursor still moving: no cover decode nor description wrap yet
    out.image.clear();
    out.video.clear();
    out.desc.clear();
  }
  out.name_up = utf8_upper(out.name);
  return true;
}

int detail_list_cols(const Theme& th, float w, float h) {
  const remake::Grid g(w, h);
  return std::clamp(int(std::lround((th.f("listW") * w + g.gutter()) / (g.colw() + g.gutter()))), 1, 11);
}

void detailed_column(const Theme& th, Assets& a, float w, float h, const DetailedData& d, std::vector<Op>& ops) {
  // on the cells: the column starts on the grid line after the list, everything in it is measured in whole cells from the top margin
  const remake::Grid g(w, h);
  const float u = float(g.u);
  const int lc = detail_list_cols(th, w, h);
  const auto line = [&](float y) { return float(g.top()) + std::round((y - float(g.top())) / u) * u; };
  const auto cells = [&](float v, float least = 1.0f) { return std::max(least, std::round(v / u)) * u; };
  float cx = g.col(lc), cw = g.span(12 - lc);
  const float cover_y = line(th.f("listY") * h);
  Color txt = th.color("txt"), dim = th.color("dim");
  d.desc_max_scroll = 0;
  d.genre_w = 0;

  // cover: decoded once at its drawing size, anchored top-left (origin 0 0). On the device it is decoded on the job
  // thread: while it is not there the area stays empty and the caller is told (`cover_missing`).
  d.cover_missing = false;
  const float cover_w = std::min(cw, cells(th.f("coverW") * w)), cover_h = cells(th.f("coverH") * h);
  video::box_offer(cx, cover_y, cover_w, cover_h);  // where the list preview plays
  if (!d.image.empty() && !covers::failed(d.image)) {
    float bw = cover_w, bh = cover_h;
    uint32_t mw = uint32_t(std::lround(bw)), mh = uint32_t(std::lround(bh));
    ImgId id = 0;
    bool have = d.decode_inline ? a.file_fit(d.image, mw, mh, id) : covers::ready(a, d.image, mw, mh, id);
    if (have) {
      float iw, ih;
      fit(float(a.img(id).w), float(a.img(id).h), bw, bh, iw, ih);
      ops.push_back(Op::image(id, cx, cover_y, iw, ih));
    } else if (d.decode_inline) {
      covers::mark_failed(d.image);
    } else {
      d.cover_missing = true;
      d.cover_w = mw;
      d.cover_h = mh;
    }
  }

  if (th.str("listCase", "") == "lower") {  // theme variable listCase (docs/theme-carousel.md): the name follows the list rows
    centred_text(a, ops, utf8_lower(d.name), std::floor(th.f("fName") * h), txt, cx, line(th.f("nameY") * h), cw, cells(th.f("nameH") * h));
  } else if (d.name_up.empty()) {
    centred_text(a, ops, utf8_upper(d.name), std::floor(th.f("fName") * h), txt, cx, line(th.f("nameY") * h), cw, cells(th.f("nameH") * h));
  } else {
    centred_text(a, ops, d.name_up, std::floor(th.f("fName") * h), txt, cx, line(th.f("nameY") * h), cw, cells(th.f("nameH") * h));
  }

  // meta rows; rows without data are skipped, the rating row is drawn unless the system has no ratings at all
  float label_w = std::min(cw, cells(th.f("lblW") * w)), val_x = cx + cells(th.f("valX") * w - th.f("colX") * w, 0.0f);
  float val_w = cx + cw - val_x;
  float m1 = line(th.f("meta1") * h), step = cells((th.f("meta2") - th.f("meta1")) * h), mh = cells(th.f("metaH") * h);
  float fm = std::floor(th.f("fMeta") * h);
  // a translated label wider than the theme's column pushes the values right ("Lanzamiento" at 480x320), for all the games alike
  float widest = 0;
  for (const char* l : {tr("Released"), tr("Genre"), tr("Players"), tr("Rating")}) widest = std::max(widest, a.measure(l, fm));
  if (widest + 4.0f > label_w) {
    const float need = std::ceil((widest + 4.0f) / u) * u;  // a whole number of cells
    val_w -= cx + need - val_x;
    val_x = cx + need;
    label_w = need;
  }
  char year[16] = "";
  if (d.year >= 0) std::snprintf(year, sizeof year, "%d", int(d.year));
  struct Row {
    const char* label;
    std::string_view value;
  };
  const Row rows[3] = {{tr("Released"), year}, {tr("Genre"), trim(d.genre)}, {tr("Players"), trim(d.players)}};
  int slot = 0;
  for (const Row& r : rows) {
    if (r.value.empty()) continue;
    float y = m1 + float(slot) * step;
    centred_text(a, ops, r.label, fm, dim, cx, y, label_w, mh);
    if (&r == &rows[1]) {
      genre_text(a, ops, d, r.value, fm, txt, val_x, y, val_w, mh);
    } else {
      centred_text(a, ops, r.value, fm, txt, val_x, y, val_w, mh);
    }
    ++slot;
  }
  if (d.show_rating) {
    float y = m1 + float(slot) * step;
    centred_text(a, ops, tr("Rating"), fm, dim, cx, y, label_w, mh);
    // starY - meta4 is the star offset inside its row
    stars(th, a, ops, val_x, y + (th.f("starY") - th.f("meta4")) * h, th.f("starH") * h, d.rating);
    ++slot;
  }

  // description: moves up with the rows that were omitted, shows a window of `descH` over the full wrapped text
  if (th.str("descVisible") == "true" && !trim(d.desc).empty()) {
    float freed = float(4 - slot) * step;
    float top = line(th.f("descY") * h) - freed, height = cells(th.f("descH") * h) + freed;
    float px = std::floor(th.f("fDesc") * h);
    float lh = a.line_height(px);
    float step_px = std::max(std::round(lh * kDescSpacing), 1.0f);
    // ES centres each line in its (taller) slot: half the extra leading above the first line
    float lead = std::ceil((step_px - lh) / 2.0f);
    // ES wraps at the integer width of the box (`int sx = size.x`) and a word fits only while the line is strictly narrower
    ImgId id = a.text_wrap(d.desc, px, dim, std::floor(cw) - 1.0f / 64.0f, kMaxDescLines, kDescSpacing);
    float y0 = std::round(top), win = std::round(top + height) - y0;
    int content = int(lead) + int(a.img(id).h);
    int max_off = std::max(content - int(win), 0);
    d.desc_max_scroll = max_off;
    int off = d.desc_time < 0 ? 0 : desc_scroll_offset(d.desc_time, max_off);
    ops.push_back(Op::clip(cx, y0, cw, win));
    ops.push_back(Op::image(id, cx, y0 + lead - float(off), float(a.img(id).w), float(a.img(id).h)));
    ops.push_back(Op::unclip());
  }
}

}  // namespace zf
