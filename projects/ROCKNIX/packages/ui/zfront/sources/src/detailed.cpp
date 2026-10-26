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
#include "services.hpp"
#include "util.hpp"

namespace zf {

namespace {

bool g_demo = false;

/// Cap of the wrapped description (lines). A longer text ends with an ellipsis; 128 lines are ~4 MB at 720x720.
constexpr size_t kMaxDescLines = 128;
constexpr float kDescSpacing = 1.3f;  // md_description lineSpacing of the theme
constexpr size_t kDetailsCache = 8;

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

const GameDetails& details_cached(const System& sys, const Game& g) {
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

bool detailed_enabled() { return true; }

void detailed_set_demo(bool on) { g_demo = on; }

bool detailed_for(const System& sys, size_t idx, bool settled, DetailedData& out) {
  if (idx >= sys.games.size()) return false;
  const Game& g = sys.games[idx];
  out.desc_time = -1;
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
    if (settled) {
      const GameDetails& det = details_cached(sys, g);
      out.image = det.image.empty() ? det.thumbnail : det.image;
      out.desc = det.desc;
    }
  }
  if (!settled) {  // cursor still moving: no cover decode nor description wrap yet
    out.image.clear();
    out.desc.clear();
  }
  out.name_up = utf8_upper(out.name);
  return true;
}

void detailed_column(const Theme& th, Assets& a, float w, float h, const DetailedData& d, std::vector<Op>& ops) {
  float cx = th.f("colX") * w, cw = th.f("colW") * w;
  Color txt = th.color("txt"), dim = th.color("dim");
  d.desc_max_scroll = 0;

  // cover: decoded once at its drawing size, anchored top-left (origin 0 0). On the device it is decoded on the job
  // thread: while it is not there the area stays empty and the caller is told (`cover_missing`).
  d.cover_missing = false;
  if (!d.image.empty() && !covers::failed(d.image)) {
    float bw = th.f("coverW") * w, bh = th.f("coverH") * h;
    uint32_t mw = uint32_t(std::lround(bw)), mh = uint32_t(std::lround(bh));
    ImgId id = 0;
    bool have = d.decode_inline ? a.file_fit(d.image, mw, mh, id) : covers::ready(a, d.image, mw, mh, id);
    if (have) {
      float iw, ih;
      fit(float(a.img(id).w), float(a.img(id).h), bw, bh, iw, ih);
      ops.push_back(Op::image(id, cx, th.f("listY") * h, iw, ih));
    } else if (d.decode_inline) {
      covers::mark_failed(d.image);
    } else {
      d.cover_missing = true;
      d.cover_w = mw;
      d.cover_h = mh;
    }
  }

  if (d.name_up.empty()) {
    centred_text(a, ops, utf8_upper(d.name), std::floor(th.f("fName") * h), txt, cx, th.f("nameY") * h, cw, th.f("nameH") * h);
  } else {
    centred_text(a, ops, d.name_up, std::floor(th.f("fName") * h), txt, cx, th.f("nameY") * h, cw, th.f("nameH") * h);
  }

  // meta rows; rows without data are skipped, the rating row is always drawn
  float label_w = th.f("lblW") * w, val_x = th.f("valX") * w, val_w = th.f("valW") * w;
  float m1 = th.f("meta1") * h, step = (th.f("meta2") - th.f("meta1")) * h, mh = th.f("metaH") * h;
  float fm = std::floor(th.f("fMeta") * h);
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
    centred_text(a, ops, r.value, fm, txt, val_x, y, val_w, mh);
    ++slot;
  }
  {
    float y = m1 + float(slot) * step;
    centred_text(a, ops, tr("Rating"), fm, dim, cx, y, label_w, mh);
    // starY - meta4 is the star offset inside its row
    stars(th, a, ops, val_x, y + (th.f("starY") - th.f("meta4")) * h, th.f("starH") * h, d.rating);
    ++slot;
  }

  // description: moves up with the rows that were omitted, shows a window of `descH` over the full wrapped text
  if (th.str("descVisible") == "true" && !trim(d.desc).empty()) {
    float freed = float(4 - slot) * step;
    float top = th.f("descY") * h - freed, height = th.f("descH") * h + freed;
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
