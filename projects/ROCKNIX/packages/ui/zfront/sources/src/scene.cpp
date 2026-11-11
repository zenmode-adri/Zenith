#include "scene.hpp"

#include "scan.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_map>

#include "covers.hpp"
#include "i18n.hpp"
#include "motion.hpp"
#include "remake.hpp"
#include "services.hpp"
#include "ui_prefs.hpp"
#include "ui_style.hpp"
#include "util.hpp"

namespace zf {

namespace {

// Carousel parameters of the Zenith theme (`theme.xml`, `<carousel name="systemcarousel">`). They are the DEFAULTS of the optional
// theme variables carScale, carSpacing, carCount, carTileScale and typeScale, sysBar (see docs/theme-carousel.md): a theme that does not set them
// renders exactly as Zenith always did.
constexpr float kLogoScale = 1.23f;
constexpr float kScaledSpacing = 1.21f;
constexpr int kMaxLogoCount = 5;
constexpr float kTileScale = 1.5f;  // the tile / frame images are this many times the container

/// The carousel structure of a theme, read once per call from its optional variables and clamped to sensible ranges, so that an absurd
/// value in a theme.xml can neither break the layout nor divide by zero.
struct Car {
  float scale = kLogoScale;      // carScale: selected card vs the others (1 = all the same)
  float spacing = kScaledSpacing;  // carSpacing: extra gap between the selected card and its neighbours
  int count = kMaxLogoCount;     // carCount: cards visible at once
  float tile = kTileScale;       // carTileScale: card image / container
  float type = 1.0f;             // typeScale: text of the carousel labels
  bool bar = true;               // sysBar: colour bar before the system's name
};

Car car_of(const Theme& th) {
  Car c;
  c.scale = std::clamp(th.f("carScale", kLogoScale), 1.0f, 1.5f);
  c.spacing = std::clamp(th.f("carSpacing", kScaledSpacing), 0.5f, 2.0f);
  c.count = std::clamp(int(std::lround(th.f("carCount", float(kMaxLogoCount)))), 1, 9);
  c.tile = std::clamp(th.f("carTileScale", kTileScale), 1.0f, 1.8f);
  c.type = std::clamp(th.f("typeScale", 1.0f), 0.6f, 1.4f);
  c.bar = th.f("sysBar", 1.0f) != 0.0f;
  return c;
}

/// listCase: "" keeps the case each text has (rows in capitals, the rest as the games are named), "upper" / "lower" force it on all the list texts.
std::string list_case(const Theme& th, std::string_view s, bool upper_by_default) {
  const std::string_view mode = th.str("listCase", "");
  if (mode == "lower") return utf8_lower(s);
  if (mode == "upper" || upper_by_default) return utf8_upper(s);
  return std::string(s);
}

const Color kWhite = {255, 255, 255, 255};

/// 4:3 screens (640x480, 800x600): the theme's default block puts the carousel and the name under it so low that the "continue
/// playing" row has no room (the band between the hairline and the help pill is ~0.15 of the height, the row needs ~0.2). The band
/// above the carousel is free (~0.2 of the height between the status bar and the selected card), so both move up by this fraction of
/// the screen height: the row then fits at the proportion it has on 720x720 (tiles ~0.1 of the height). Every other ratio keeps the theme's own positions (offset 0).
/// The offset does not depend on whether the row has anything to show, so the carousel never jumps between systems.
constexpr float kLift43 = 0.09f;

float carousel_lift(float w, float h) {
  const bool is43 = std::strcmp(ratio_name(unsigned(std::lround(w)), unsigned(std::lround(h))), "4/3") == 0;
  return is43 ? std::round(kLift43 * h) : 0.0f;
}

/// Word under the logo of a variant system (Hacks, Minis, DSiWare...): the theme names it in
/// `assets/logos/tags/<system>.txt` (theme.xml `zs-logo-tagged` + `zs-tag-*`, which raise the logo and write the word);
/// nullptr for the other systems. Read once per theme and language.
const char* logo_tag(const Theme& th, const std::string& theme_name) {
  static const char* const kWords[] = {N_("Hacks"), N_("Video"), N_("Minis"), N_("DSiWare"), N_("Homebrew"), N_("Vertical")};
  static std::unordered_map<std::string, std::string> cache;  // theme dir + system -> shown word ("" = none)
  static unsigned gen = ~0u;
  if (gen != i18n::generation()) {
    gen = i18n::generation();
    cache.clear();
  }
  std::string key = th.dir() + '\n' + theme_name;
  auto it = cache.find(key);
  if (it == cache.end()) {
    std::string word;
    if (read_file(th.path("./assets/logos/tags/" + theme_name + ".txt"), word)) {
      while (!word.empty() && (word.back() == '\n' || word.back() == '\r' || word.back() == ' ')) word.pop_back();
      for (const char* w : kWords)
        if (word == w) word = tr(w);
    }
    it = cache.emplace(std::move(key), std::move(word)).first;
  }
  return it->second.empty() ? nullptr : it->second.c_str();
}

/// Label drawn under the icon of the "auto" tiles (theme.xml `zs-label-*`, shown if the `.txt` exists): the ES catalog key
/// (`MUSIC`, `all games`...) shown with a capital first letter, translated once per language.
const char* icon_label(const std::string& theme_name) {
  struct L {
    const char* key;
    const char* text;
    std::string shown;
  };
  static L labels[] = {{"music", N_("MUSIC"), {}},
                       {"tools", N_("TOOLS"), {}},
                       {"auto-favorites", N_("FAVORITES"), {}},
                       {"auto-allgames", N_("all games"), {}},
                       {"auto-lastplayed", N_("last played"), {}},
                       {"auto-neverplayed", N_("never played"), {}},
                       {"auto-recentlyadded", N_("recently added"), {}},
                       {"auto-unfinished", N_("unfinished"), {}},
                       {"auto-mostplayedweek", N_("most played this week"), {}},
                       {"custom-collections", N_("COLLECTIONS"), {}},
                       {"ebook", N_("BOOKS"), {}},
                       {"imageviewer", N_("screenshots"), {}},
                       {"mplayer", N_("media player"), {}}};
  static unsigned gen = ~0u;
  if (gen != i18n::generation()) {
    gen = i18n::generation();
    for (L& l : labels) l.shown = utf8_cap_first(tr(l.text));
  }
  for (const L& l : labels)
    if (theme_name == l.key) return l.shown.c_str();
  return nullptr;
}

/// Greedy word wrap of a system name into at most `max_lines` lines of `max_w` px (ES `zs-logo-text`: 0.94 x 0.7 of the container, multiLine, centred);
/// what does not fit goes on the last line, which the caller cuts with an ellipsis.
std::vector<std::string> wrap_centred(Assets& a, const std::string& s, float px, float max_w, int max_lines) {
  std::vector<std::string> rows;
  std::string cur;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = s.find(' ', i);
    if (j == std::string::npos) j = s.size();
    if (j == i) break;
    std::string word = s.substr(i, j - i);
    i = j;
    std::string cand = cur.empty() ? word : cur + " " + word;
    if (cur.empty() || a.measure(cand, px) <= max_w || int(rows.size()) + 1 >= max_lines) {
      cur = cand;
    } else {
      rows.push_back(cur);
      cur = word;
    }
  }
  if (!cur.empty()) rows.push_back(cur);
  return rows;
}

/// "14 games" under the carousel: ES's plural entry `%i GAME` in sentence case.
std::string games_text(uint32_t n) {
  std::string t = i18n::ntr("%i GAME", "%i GAMES", n);
  size_t p = t.find("%i");
  if (p == std::string::npos) p = t.find("%d");
  if (p != std::string::npos) t.replace(p, 2, std::to_string(n));
  return i18n::sentence_case(t);
}

/// Fit an image of `iw x ih` inside `mw x mh` keeping its aspect ratio.
std::pair<float, float> fit(float iw, float ih, float mw, float mh) {
  float s = std::min(mw / iw, mh / ih);
  return {iw * s, ih * s};
}

int rem_euclid(int a, int n) {
  int r = a % n;
  return r < 0 ? r + n : r;
}


// Zenith Minimal on the grid of cells (remake::Grid): the theme's fractions of the screen (theme.xml, shared with ES) say where things go roughly;
// what is drawn lands on the nearest line of the cells. `line_y` is the horizontal line nearest to `y`, `up_cells` the cells that cover `v` px.
float line_y(const remake::Grid& g, float y) { return float(g.top()) + std::round((y - float(g.top())) / float(g.u)) * float(g.u); }
float up_cells(const remake::Grid& g, float v) { return std::ceil(v / float(g.u) - 0.001f) * float(g.u); }
float down_cells(const remake::Grid& g, float v) { return std::floor(v / float(g.u) + 0.001f) * float(g.u); }

/// The avatar of the header: its box in the status bar's row, left on the margin.
std::pair<float, float> avatar_size(const Theme& th, Assets& a, const Common& c, const remake::Grid& g) {
  const Img& img = a.img(c.avatar);
  return fit(float(img.w), float(img.h), std::min(th.f("avW") * g.w, float(g.bar_h())), std::min(th.f("avH") * g.h, float(g.bar_h())));
}

void push_avatar(const Theme& th, Assets& a, const Common& c, float w, float h, std::vector<Op>& ops) {
  const remake::Grid g(w, h);
  auto [aw, ah] = avatar_size(th, a, c, g);
  ops.push_back(Op::image(c.avatar, float(g.margin()), std::round(float(g.status_y()) - ah / 2.0f), aw, ah));
}

}  // namespace

size_t clip_count(Assets& a, std::string_view s, float px, float max) {
  std::vector<size_t> ends;
  for (size_t i = 0; i < s.size();) {
    utf8_next(s, i);
    ends.push_back(i);
  }
  const size_t total = ends.size();
  auto width_of = [&](size_t count) { return std::ceil(a.measure(s.substr(0, count == 0 ? 0 : ends[count - 1]), px)) + 1.0f; };
  if (total == 0 || width_of(total) <= max) return total;  // the usual row fits: one measure
  // The width grows with the prefix, so the longest prefix that fits is found by halving (it was one measure per character, each one
  // reading the whole prefix: quadratic, for every long name on every frame).
  size_t lo = 0, hi = total;  // `hi` does not fit; `lo` does (or is empty, which is kept even when it does not)
  while (hi - lo > 1) {
    const size_t mid = lo + (hi - lo) / 2;
    if (width_of(mid) <= max) lo = mid;
    else hi = mid;
  }
  return lo;
}

// text image of `s`, cut (no ellipsis, like the scissor ES applies to list rows) until it fits in `max` pixels
ImgId clip_text(Assets& a, std::string_view s, float px, Color color, float max) {
  const size_t n = clip_count(a, s, px, max);
  size_t end = 0;
  for (size_t k = 0; k < n; ++k) utf8_next(s, end);
  return a.text(s.substr(0, end), px, color);
}

namespace {
using HelpKeys = std::initializer_list<std::pair<const char*, const char*>>;
/// A help bar: (icon, ES catalog key) pairs; the text is translated and put in sentence case once per language.
struct HelpCache {
  unsigned gen = ~0u;
  std::deque<std::string> text;  // pointers into a deque stay valid while it grows
  HelpPrompts prompts;
};
const HelpPrompts& help_for(HelpCache& c, HelpKeys keys) {
  if (c.gen != i18n::generation()) {
    c.gen = i18n::generation();
    c.text.clear();
    c.prompts.clear();
    for (const auto& k : keys) {
      c.text.push_back(i18n::sentence_case(tr(k.second)));
      c.prompts.push_back({k.first, c.text.back().c_str()});
    }
  }
  return c.prompts;
}
}  // namespace

const HelpPrompts& system_help() {
  static HelpCache c;
  return help_for(c, {{"leftright", N_("CHOOSE")}, {"A", N_("SELECT")}, {"X", N_("RANDOM")}, {"start", N_("MENU")}});
}
const HelpPrompts& recent_help() {
  static HelpCache c;
  return help_for(c, {{"leftright", N_("CHOOSE")}, {"A", N_("LAUNCH")}, {"B", N_("BACK")}});
}
const HelpPrompts& list_help() {
  static HelpCache c;
  return help_for(c, {{"A", N_("LAUNCH")}, {"B", N_("BACK")}, {"X", N_("GAME OPTIONS")}, {"Y", N_("SEARCH")}});
}

bool load_common(const Theme& th, Assets& a, Common& out, std::string& err) {
  struct F {
    const char* rel;
    ImgId* id;
  } files[] = {{"./assets/tile.png", &out.tile}, {"./assets/frame.png", &out.frame}, {"./assets/avatar.png", &out.avatar}};
  for (auto& f : files) {
    if (!a.file(th.path(f.rel), *f.id)) {
      err = th.path(f.rel) + ": cannot load";
      return false;
    }
    a.pin(*f.id);  // the scenes keep these ids across frames: never evict them
  }
  return true;
}

namespace {
// The bar is a pure function of (theme, assets, size, status, battery-text preference). Building it allocates (cache
// keys of the text and icon images, theme paths), and it is drawn every frame, so the last result is kept and copied.
struct BarCache {
  bool valid = false;
  uint64_t uid = 0;
  std::string dir;
  float w = 0, h = 0;
  bool batt_text = false;
  Status st;
  std::vector<Op> ops;
};
}  // namespace

static void build_status_bar(const Theme& th, Assets& a, float w, float h, const Status& st, std::vector<Op>& ops);

// For test_layout: the box of the last op, or of the ops from `from` on.
static void audit_last(const std::vector<Op>& ops, std::string_view tag, char anchor = 'L', bool grid = true) {
  if (remake::audit_sink() && !ops.empty()) remake::audit(ops.back().x, ops.back().y, ops.back().w, ops.back().h, std::string(tag), anchor, grid);
}
static void audit_from(const std::vector<Op>& ops, size_t from, std::string_view tag, char anchor = 'L') {
  if (!remake::audit_sink() || ops.size() <= from) return;
  float l = 1e9f, r = -1e9f, t = 1e9f, b = -1e9f;
  bool clipped = false;
  Op clip{};
  for (size_t i = from; i < ops.size(); ++i) {  // what a clip hides (a description that scrolls) is not on the screen
    if (ops[i].kind == Op::Clip) clipped = true, clip = ops[i];
    if (ops[i].kind == Op::Unclip) clipped = false;
    if (ops[i].kind != Op::Image && ops[i].kind != Op::Rect) continue;
    float x0 = ops[i].x, y0 = ops[i].y, x1 = x0 + ops[i].w, y1 = y0 + ops[i].h;
    if (clipped) x0 = std::max(x0, clip.x), y0 = std::max(y0, clip.y), x1 = std::min(x1, clip.x + clip.w), y1 = std::min(y1, clip.y + clip.h);
    if (x1 <= x0 || y1 <= y0) continue;
    l = std::min(l, x0), r = std::max(r, x1), t = std::min(t, y0), b = std::max(b, y1);
  }
  if (r > l) remake::audit(l, t, r - l, b - t, std::string(tag), anchor, false);
}

void status_bar(const Theme& th, Assets& a, float w, float h, const Status& st, std::vector<Op>& ops) {
  static thread_local BarCache c;
  const bool bt = ui_prefs::battery_text();
  bool hit = c.valid && c.uid == a.uid() && c.w == w && c.h == h && c.batt_text == bt && c.st == st && c.dir == th.dir() && !remake::audit_sink();
  if (hit)
    for (const Op& op : c.ops)
      if (op.kind == Op::Image && a.evicted(op.id)) {  // an image of the bar was evicted: build it again
        hit = false;
        break;
      }
  if (!hit) {
    c.ops.clear();
    build_status_bar(th, a, w, h, st, c.ops);
    c.valid = true;
    c.uid = a.uid();
    c.dir = th.dir();
    c.w = w;
    c.h = h;
    c.batt_text = bt;
    c.st = st;
  }
  const size_t first = ops.size();
  ops.insert(ops.end(), c.ops.begin(), c.ops.end());
  motion::status_range() = {first, ops.size()};
}

static void build_status_bar(const Theme& th, Assets& a, float w, float h, const Status& st, std::vector<Op>& ops) {
  if (remake::active(th)) {
    remake::status(th, a, w, h, st, ops);
    return;
  }
  const size_t bar_from = ops.size();
  Color txt = th.color("txt");
  const remake::Grid grid(w, h);
  float right = grid.w - float(grid.margin());  // the right margin's line
  // Zenith Minimal: the bar in a pill like its help row's (white, a hairline), its right edge on the margin and the icons two cells inside
  const std::string dir = th.dir();
  const bool pill = dir.find("es-theme-zenith") != std::string::npos && dir.find("zenith-next") == std::string::npos;
  const float pad = float(grid.U(2));
  if (pill) right -= pad;
  float sh = std::min(th.f("stackH") * h, float(grid.bar_h()));
  float cy = float(grid.status_y());
  float sep = std::max(float(grid.u), std::round(th.f("stackSep") * w / float(grid.u)) * float(grid.u));
  // clock (hidden while the system time is not set) and battery percentage (only with a battery): the line is the ink of the clock's digits,
  // and the percent and the icons are made exactly that tall on it (test_status_ink.cpp)
  const int clock_px = int(std::floor(th.f("fClock") * h));
  const remake::InkLine ln = remake::ink_line(a, st.time, clock_px, cy);
  const int want = std::max(1, int(std::lround(ln.h)));
  if (!st.time.empty()) {
    ImgId id = a.text(st.time, float(clock_px), txt);
    const float tw = float(a.img(id).w), thh = float(a.img(id).h);
    ops.push_back(Op::image(id, std::round(right - tw), std::round(cy - thh / 2.0f), tw, thh));
    right -= tw + sep;
  }
  const std::string pct = st.battery && ui_prefs::battery_text() ? std::to_string(st.battery->percent) + "%" : std::string();
  if (!pct.empty()) {
    const int px = remake::px_for_ink(a, pct, want, int(std::floor(th.f("fBatt") * h)));
    ImgId id = a.text(pct, float(px), txt);
    int t = 0, b = 0;
    remake::text_ink(a, pct, px, t, b);
    const float tw = float(a.img(id).w), thh = float(a.img(id).h);
    ops.push_back(Op::image(id, std::round(right - tw), ln.top - float(t), tw, thh));
    right -= tw + sep;
  }
  // a theme picture that is measured: made again if the renderer released its pixels, and pinned (see remake::measurable)
  const auto svg_px = [&](const std::string& path, uint32_t size, ImgId& k) {
    if (!a.svg(path, size, k)) return false;
    if (!a.has_pixels(k)) {
      a.forget(k);
      if (!a.svg(path, size, k)) return false;
    }
    a.pin(k);
    return true;
  };
  // the theme's picture at the height whose ink is as tall as the line, and where it goes
  const auto fit_svg = [&](const std::string& path, ImgId& id, float& y) {
    int size = std::max(2, int(std::lround(sh))), err = 1 << 20;
    bool ok = false;
    for (int i = 0; i < 10; ++i) {
      ImgId k;
      int t, b;
      if (!svg_px(path, uint32_t(size), k) || !remake::ink_rows(a.img(k), t, b)) break;
      const int got = b - t + 1, e = got - want;
      if (std::abs(e) < err) err = std::abs(e), id = k, y = ln.top - float(t), ok = true;
      if (e == 0) break;
      const int next = i == 0 ? std::max(2, int(std::lround(float(size) * float(want) / float(got)))) : size - (e > 0 ? 1 : -1);
      if (next == size) break;
      size = next;
    }
    return ok;
  };
  // a battery drawn standing, lying down: turned a quarter clockwise (tip to the right), at the size whose ink, now across, is as tall as the line
  const auto fit_side = [&](const std::string& path, ImgId& id, float& y) {
    int size = std::max(2, int(std::lround(sh))), err = 1 << 20, best = 0;
    for (int i = 0; i < 10; ++i) {
      ImgId k;
      if (!svg_px(path, uint32_t(size), k)) break;
      const Img& im = a.img(k);
      int l = int(im.w), r = -1;
      for (uint32_t yy = 0; yy < im.h; ++yy)
        for (uint32_t xx = 0; xx < im.w; ++xx)
          if (im.rgba[(size_t(yy) * im.w + xx) * 4 + 3] > 127) l = std::min(l, int(xx)), r = std::max(r, int(xx));
      if (r < 0) break;
      const int got = r - l + 1, e = got - want;
      if (std::abs(e) < err) err = std::abs(e), best = size;
      if (e == 0) break;
      const int next = i == 0 ? std::max(2, int(std::lround(float(size) * float(want) / float(got)))) : size - (e > 0 ? 1 : -1);
      if (next == size) break;
      size = next;
    }
    ImgId k;
    if (best == 0 || !svg_px(path, uint32_t(best), k)) return false;
    const std::string key = "side:" + path + ":" + std::to_string(best);
    if (a.find(key, id) && !a.has_pixels(id)) a.forget(id);  // measured below: made again if released
    if (!a.find(key, id)) {
      const Img& im = a.img(k);
      Img rot;
      rot.w = im.h;
      rot.h = im.w;
      rot.rgba.assign(size_t(rot.w) * rot.h * 4, 0);
      for (uint32_t yy = 0; yy < im.h; ++yy)
        for (uint32_t xx = 0; xx < im.w; ++xx)
          std::copy_n(&im.rgba[(size_t(yy) * im.w + xx) * 4], 4, &rot.rgba[(size_t(xx) * rot.w + (im.h - 1 - yy)) * 4]);
      id = a.add(key, std::move(rot));
    }
    a.pin(id);
    int t, b;
    if (!remake::ink_rows(a.img(id), t, b)) return false;
    y = ln.top - float(t);
    return true;
  };
  // charging: the theme's picture of the level, and the bolt drawn here on it, standing and ringed (the theme's own bolt lies down with
  // its battery and is lost in the level)
  const char* icons[2] = {st.battery ? Status::battery_icon(Battery{st.battery->percent, false}) : nullptr, st.wifi_icon()};
  for (const char* icon : icons) {
    if (!icon) continue;
    ImgId id;
    float y;
    const std::string path = th.path(std::string("./assets/icons/") + icon);
    ImgId probe;
    const bool standing = icon == icons[0] && a.svg(path, 64, probe) && a.img(probe).h > a.img(probe).w * 6 / 5;
    if (standing ? fit_side(path, id, y) : fit_svg(path, id, y)) {
      const float iw = float(a.img(id).w), ih = float(a.img(id).h);
      // low and not charging: the battery red, as in the other themes (the red of the menus, for a dark or a light page)
      const bool light = int(txt[0]) + int(txt[1]) + int(txt[2]) > 3 * 128;
      const bool low = icon == icons[0] && st.battery->percent <= 15 && !st.battery->charging;
      ops.push_back(Op::image(id, std::round(right - iw), y, iw, ih, low ? (light ? Color{0xEE, 0x6B, 0x74, 255} : Color{0xC2, 0x34, 0x40, 255}) : txt));
      if (icon == icons[0] && st.battery->charging) {
        // on the middle of the body, not of the picture: the columns as tall as most of the ink, which leaves out the tip
        const Img& im = a.img(id);
        std::vector<int> col(im.w, 0);
        int most = 0;
        for (uint32_t yy = 0; yy < im.h; ++yy)
          for (uint32_t xx = 0; xx < im.w; ++xx)
            if (im.rgba[(size_t(yy) * im.w + xx) * 4 + 3] > 127) most = std::max(most, ++col[xx]);
        int l = 0, r = int(im.w) - 1;
        while (l < r && col[size_t(l)] * 3 < most * 2) ++l;
        while (r > l && col[size_t(r)] * 3 < most * 2) --r;
        remake::status_bolt(a, ops, std::round(right - iw) + float(l + r + 1) / 2.0f, ln.top + ln.h / 2.0f, ln.h, txt, light ? Color{24, 24, 28, 255} : Color{245, 245, 245, 255});
      }
      right -= iw + sep;
    }
  }
  if (pill && ops.size() > bar_from) {
    float left = grid.w;
    for (size_t i = bar_from; i < ops.size(); ++i) left = std::min(left, ops[i].x);
    const float x1 = grid.w - float(grid.margin()), cell = float(grid.u);
    const float x0 = x1 - std::ceil((x1 - left + pad) / cell) * cell;  // whole cells wide
    const float y0 = float(grid.top()), ph = float(grid.bar_h());
    const ui::Style us = ui::Style::from(th);
    static ui::Shapes shapes;
    std::vector<Op> bg;
    shapes.rrect(a, bg, x0, y0 + 2.0f, x1 - x0, ph, us.radius(ph), us.dark ? Color{0, 0, 0, 90} : Color{0, 0, 0, 22});
    shapes.framed(a, bg, x0, y0, x1 - x0, ph, us.radius(ph), us.panel, us.line, 1.0f);
    ops.insert(ops.begin() + std::ptrdiff_t(bar_from), bg.begin(), bg.end());
    if (remake::audit_sink()) remake::audit(x0, y0, x1 - x0, ph, "tarjeta", 'B', false, true);  // a container: what it holds is meant to be on it
  }
  audit_from(ops, bar_from, "barra", 'R');
}

/// Height of the help pill, the same formula as ui::help_pill.
// the same pill as the menus' (ui::Metrics on the grid): whole cells tall, its bottom on the bottom margin
static float help_pill_h(const Theme& th, float w, float h) { return ui::Metrics::from(th, w, h).help_h; }

// Help bar: a floating pill, see ui::help_pill. The icons are the theme's help-<icon>.png masks, tinted.
void help_bar(const Theme& th, Assets& a, float w, float h, const HelpPrompts& prompts, std::vector<Op>& ops) {
  if (!ui_prefs::help_prompts()) return;  // ES ShowHelpPrompts (ON-SCREEN HELP) off
  if (remake::active(th)) {
    remake::menu_help(th, a, w, h, prompts, ops);
    return;
  }
  const float px = std::floor(th.f("fHelp") * h);
  std::vector<ui::HelpItem> items;
  for (const auto& [icon, text] : prompts) {
    ImgId iid;
    if (!a.file(th.path(std::string("./assets/help-") + icon + ".png"), iid)) continue;
    ImgId tid = a.text(text, px, Color{255, 255, 255, 255});
    items.push_back({iid, float(a.img(iid).w), float(a.img(iid).h), tid, float(a.img(tid).w), float(a.img(tid).h), std::string(icon) == "A"});
  }
  // the pill is as tall as the text makes it (same formula as ui::help_pill); its bottom goes on the bottom margin, not centred on the cells' row
  const ui::Metrics m = ui::Metrics::from(th, w, h);
  ui::help_pill(a, ui::Style::from(th), w, h, m.help_y, px, items, ops, &m);
}

/// Muted colour of a system's bar under the carousel, by maker; anything unlisted gets the theme's own accent.
/// Where the row of "continue playing" sits under the hairline `hair_y` (one place for the drawing and for the keys). `fits` is false
/// when the screen has not the room: on a short screen the row is left out, never squeezed.
namespace {
struct RecentGeom {
  float px = 0, label_y = 0, label_h = 0, tile_top = 0, cap_h = 0, tile_h = 0, tile_w = 0, gap = 0;
  size_t fit_n = 0;
  int cols = 3;  // columns of the grid one tile spans
  ImgId label = 0;
  bool fits = false;
};
}  // namespace

static float row_margin(float w, float h) { return float(remake::Grid(w, h).margin()); }

/// Under the carousel: the line the name and the game count are centred on, the height of the bar before the name (an even number of cells,
/// so its top is on a line too) and the line of the hairline under them.
struct InfoGeom {
  float cy = 0, bar_h = 0, hair_y = 0;
};
static InfoGeom info_geom(const Theme& th, Assets& a, float w, float h, const SystemItem& cur) {
  const remake::Grid g(w, h);
  const float u = float(g.u);
  ImgId name = a.text(cur.full_name, std::round(th.f("fSys") * h * 1.75f * car_of(th).type), th.color("txt"));
  InfoGeom r;
  r.bar_h = 2.0f * u * std::max(1.0f, std::round(float(a.img(name).h) * 0.95f / (2.0f * u)));
  r.cy = line_y(g, th.f("sysInfoY") * h - carousel_lift(w, h));  // follows the carousel when it is lifted
  r.hair_y = r.cy + up_cells(g, r.bar_h / 2.0f + 0.011f * h);
  return r;
}

/// y of the hairline under the system's name and count.
static float hairline_y(const Theme& th, Assets& a, float w, float h, const SystemItem& cur) { return info_geom(th, a, w, h, cur).hair_y; }

/// The row of "continue playing" on the cells: the tiles span `cols` columns (16:9 as near as whole cells allow), the first on column 0
/// and each next one on the column after its gap, so every tile's left edge is on a column and its four edges on the lines.
static RecentGeom recent_geom(const Theme& th, Assets& a, float w, float h, float hair_y) {
  const remake::Grid gr(w, h);
  const float u = float(gr.u);
  RecentGeom g;
  g.px = std::round(th.f("fSys") * h);
  g.label = a.text(i18n::sentence_case(tr("CONTINUE PLAYING")), g.px, th.color("dim"));
  g.label_h = float(a.img(g.label).h);
  g.label_y = hair_y + u;
  g.tile_top = line_y(gr, g.label_y + g.label_h + u * 0.75f);
  if (g.tile_top < g.label_y + g.label_h) g.tile_top += u;
  g.cap_h = std::ceil(u * 0.75f + a.line_height(g.px) + a.line_height(std::round(g.px * 0.9f)));  // gap, name and "ago" lines
  const float limit = float(gr.bottom()) - help_pill_h(th, w, h);  // clear of the help pill
  const float cap = std::min(down_cells(gr, 0.125f * h), down_cells(gr, limit - g.tile_top - g.cap_h));
  const float least = std::max(u, std::round(0.085f * h / u) * u);
  // tiles shaped like a screen (16:9): as tall as the room allows, as wide as that makes them, both on whole cells; side by side from the
  // margin with gutters of two cells, as many as the width holds
  const float th_ = cap;
  if (th_ >= least) {
    g.tile_h = th_;
    g.tile_w = std::max(u, std::round(th_ * 16.0f / 9.0f / u) * u);
    g.gap = gr.gutter();
    g.fit_n = size_t((w - 2.0f * float(gr.margin()) + g.gap) / (g.tile_w + g.gap));
    g.cols = 0;
    g.fits = g.fit_n >= 1;
  }
  return g;
}

std::vector<size_t> recent_visible(const Theme& th, Assets& a, float w, float h, const SystemItem& cur, const RecentRow& r) {
  if (remake::active(th)) return remake::recent_tiles(th, w, h, cur, r);
  std::vector<size_t> out;
  if (r.items.empty() || cur.games == kCountPending) return out;
  const float hair_y = hairline_y(th, a, w, h, cur);
  const RecentGeom g = recent_geom(th, a, w, h, hair_y);
  if (!g.fits) return out;
  for (size_t i = 0; i < r.items.size() && out.size() < g.fit_n; ++i)
    if (!covers::failed(r.items[i].shot_key())) out.push_back(i);
  return out;
}

/// "Continue playing": the newest savestate screenshots of the selected system, left aligned under the hairline. Drawn only when the
/// screen has the room (about 0.2 of its height). The tile `r.focus` carries a ring in the theme's cyan, like the carousel's selector.
static void recent_row(const Theme& th, Assets& a, const ui::Style& st, float w, float h, float mx, float hair_y, RecentRow& r, std::vector<Op>& ops) {
  static ui::Shapes shapes;
  const remake::Grid gr(w, h);
  const RecentGeom g = recent_geom(th, a, w, h, hair_y);
  if (!g.fits) return;
  const float px = g.px, tile_top = g.tile_top, tile_h = g.tile_h, tile_w = g.tile_w, gap = g.gap;
  const Color accent = st.dark ? st.cyan : st.cyan_dark;  // caption (readable on the light panel)
  r.box_w = uint32_t(tile_w) - 2, r.box_h = uint32_t(tile_h) - 2;
  ops.push_back(Op::image(g.label, mx, g.label_y, float(a.img(g.label).w), g.label_h));
  audit_last(ops, "\"seguir jugando\"");
  size_t k = 0;
  for (size_t idx = 0; idx < r.items.size(); ++idx) {
    const RecentItem& it = r.items[idx];
    if (k >= g.fit_n) break;
    if (covers::failed(it.shot_key())) continue;
    const float x = float(gr.margin()) + float(k) * (tile_w + gap);
    const bool foc = int(idx) == r.focus && r.focus_a > 0.0f;
    if (foc) {  // 2 px ring outside the tile + the tile's own border in the same colour (the capture covers 1 px of the border)
      shapes.rrect(a, ops, x - 2.0f, tile_top - 2.0f, tile_w + 4.0f, tile_h + 4.0f, 5, ui::with_alpha(st.cyan, r.focus_a));
      shapes.framed(a, ops, x, tile_top, tile_w, tile_h, 3, st.panel, ui::mix(st.line, st.cyan, r.focus_a), 1.0f);
    } else {
      shapes.framed(a, ops, x, tile_top, tile_w, tile_h, 3, st.panel, st.line, 1.0f);
    }
    ImgId id;
    const std::string key = it.shot_key();
    const bool have = r.decode_inline ? a.file_fit(key, r.box_w, r.box_h, id) : covers::ready(a, key, r.box_w, r.box_h, id);
    if (!have && r.decode_inline) covers::mark_failed(key);
    if (have) {
      const Img& im = a.img(id);
      const float iw = float(im.w), ih = float(im.h);
      ops.push_back(Op::image(id, std::round(x + (tile_w - iw) / 2.0f), std::round(tile_top + (tile_h - ih) / 2.0f), iw, ih));
    }
    if (remake::audit_sink()) remake::audit(x, tile_top, tile_w, tile_h, "miniatura", 'B', false, true);  // on the cells from the margin
    const float cy0 = tile_top + tile_h + float(gr.u) * 0.75f;
    ImgId nm = clip_text(a, short_title(it.name), px, foc && r.focus_a >= 0.5f ? accent : st.txt, tile_w);
    ops.push_back(Op::image(nm, x, cy0, float(a.img(nm).w), float(a.img(nm).h)));
    audit_last(ops, "\"miniatura nombre\"", 'L', false);
    if (!it.ago.empty()) {
      ImgId ag = a.text(it.ago, std::round(px * 0.9f), st.dim);
      ops.push_back(Op::image(ag, x, cy0 + float(a.img(nm).h), float(a.img(ag).w), float(a.img(ag).h)));
      audit_last(ops, "\"miniatura hace\"", 'L', false);
    }
    ++k;
  }
}

const char* system_icon_label(const std::string& t) { return icon_label(t); }

Color system_color(const std::string& t, const ui::Style& st) {
  auto is = [&](std::initializer_list<const char*> l) {
    for (const char* x : l)
      if (t == x) return true;
    return false;
  };
  if (is({"gbc", "gbch", "gbc2players", "ngpc", "wonderswancolor"})) return Color{0x6C, 0x4F, 0xD8, 255};
  if (is({"gb", "gbh", "gb2players", "sgb", "gameandwatch", "pokemini"})) return Color{0x7A, 0x8B, 0x5A, 255};
  if (is({"gba", "gbah", "virtualboy"})) return Color{0x4B, 0x3F, 0x9E, 255};
  if (is({"nes", "nesh", "famicom", "fds", "snes", "snesh", "snesna", "snesnah", "sfc", "satellaview", "sufami", "n64", "n64dd", "gc", "wii", "wiiu", "switch", "nds", "3ds"}))
    return Color{0xC2, 0x3B, 0x3B, 255};
  if (is({"psx", "ps2", "ps3", "ps4", "psvita", "psp"})) return Color{0x2F, 0x6F, 0xB5, 255};
  if (is({"mastersystem", "megadrive", "megadrivejp", "genesis", "genh", "segacd", "megacd", "megacdjp", "sega32x", "sega32xjp", "sega32xna", "gamegear", "saturn",
          "saturnjp", "dreamcast", "sg-1000", "naomi", "naomi2"}))
    return Color{0x2C, 0x5F, 0xA8, 255};
  if (is({"pcengine", "pce-cd", "tg16", "tg-cd", "supergrafx", "pcfx"})) return Color{0xD0, 0x7A, 0x2B, 255};
  if (is({"neogeo", "neogeocd", "neogeocdjp", "ngp", "mame", "fbneo", "fba", "arcade", "cps", "cps1", "cps2", "cps3"})) return Color{0x8A, 0x6B, 0x2E, 255};
  if (is({"atari2600", "atari5200", "atari7800", "atari800", "atarilynx", "atarijaguar", "atarist"})) return Color{0xB0, 0x55, 0x2A, 255};
  return st.cyan_dark;
}

void carousel_span(float cam, int n, int& first, int& last, int count) {
  const int center = int(std::floor(cam));
  const int half = std::min(count, n) / 2;
  // at rest tiles are drawn up to `half + 1` away, but the next one is already visible (partly) from 0.4 tiles before it gets there
  first = center - half - 2;
  last = center + half + 2;
}

void system_view(const Theme& th, Assets& a, const Common& c, float w, float h, const std::vector<SystemItem>& items, int cursor_abs, float cam,
                 const Status& status, RecentRow* recent, const InfoFade& fade, std::vector<Op>& ops) {
  if (remake::active(th)) {
    remake::system_view(th, a, w, h, items, cursor_abs, cam, status, recent, fade, ops);
    return;
  }
  ops.push_back(Op::rect(0, 0, w, h, th.color("bg")));
  push_avatar(th, a, c, w, h, ops);
  status_bar(th, a, w, h, status, ops);
  help_bar(th, a, w, h, recent && recent->focus_help ? recent_help() : system_help(), ops);
  if (recent) recent->box_w = recent->box_h = 0;
  if (items.empty()) return;

  int n = int(items.size());
  int cursor = rem_euclid(cursor_abs, n);

  // carousel box: size (carW x 0.5), centred at (0.5, carY)
  float logo_w = th.f("cellW") * w;
  float car_w = th.f("carW") * w;
  const float lift = carousel_lift(w, h);
  float ccx = w / 2.0f, ccy = th.f("carY") * h - lift;
  const Car car = car_of(th);
  // the pitch between cards never drops under one card (a big carCount on a narrow carousel would stack them); Zenith's is 1.15
  float spacing = std::max((car_w - logo_w * float(car.count)) / float(car.count) + logo_w, logo_w);
  float diff_x = (spacing * car.scale - spacing) / 2.0f * car.spacing;
  float nominal = logo_w * car.scale;  // container size at scale 1.0
  Color tile_tx = th.color("tileTxt");

  int first = 0, last = 0;
  carousel_span(cam, n, first, last, car.count);
  std::vector<std::pair<int, bool>> order;
  for (int i = first; i <= last; ++i) {
    if (n == 1 && std::fabs(float(i) - cam) > 1.0f) continue;  // a lone system keeps its three tiles
    order.push_back({i, rem_euclid(i, n) == cursor});
  }
  // selected last, like ES (activePositions)
  std::stable_sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return !x.second && y.second; });

  for (const auto& [i, sel] : order) {
    const SystemItem& it = items[size_t(rem_euclid(i, n))];
    float dist = float(i) - cam;
    float scale = std::min(std::max(1.0f + (car.scale - 1.0f) * (1.0f - std::fabs(dist)), 1.0f), car.scale);
    float cont = nominal * scale / car.scale;  // container size in px
    float k = cont / nominal;                   // text/children scale
    float side = sel ? 0.0f : (i < cursor_abs ? -diff_x : diff_x);
    float cx = ccx + dist * spacing + side;
    float cy = ccy;
    if (cx + cont * 0.75f < 0.0f || cx - cont * 0.75f > w) continue;
    float ty = cy - cont / 2.0f;
    // tile and frame images are `carTileScale` (1.5 in Zenith) x the container, centred
    float big = cont * car.tile;
    ops.push_back(Op::image(c.tile, cx - big / 2.0f, cy - big / 2.0f, big, big));
    float fa = std::min(std::max(1.0f - std::fabs(dist), 0.0f), 1.0f);
    if (fa > 0.0f) ops.push_back(Op::image(c.frame, cx - big / 2.0f, cy - big / 2.0f, big, big, {255, 255, 255, uint8_t(fa * 255.0f)}));
    // logo (maxSize 0.9 x 0.5 of the container, shrunk with the card image: carTileScale / 1.5) or the system name when the theme has no logo
    std::string logo_path = th.path("./assets/logos/" + it.theme_name + ".webp");
    ImgId id;
    if (remake::exists_cached(logo_path)) {  // not a stat per card per frame
      // variant system: the logo goes up (0.86 x 0.40 centred at 0.43) and its word under it at 0.76 (zs-logo-tagged, zs-tag-*)
      const char* tag = logo_tag(th, it.theme_name);
      if (a.file(logo_path, id)) {
        auto [lw, lh] = fit(float(a.img(id).w), float(a.img(id).h), (tag ? 0.86f : 0.90f) * cont * (car.tile / kTileScale), (tag ? 0.40f : 0.5f) * cont * (car.tile / kTileScale));
        ops.push_back(Op::image(id, cx - lw / 2.0f, ty + (tag ? 0.43f : 0.5f) * cont - lh / 2.0f, lw, lh));
      }
      if (tag) {
        id = a.text_ellipsis(tag, std::round(th.f("fLabel") * h * k * car.type * 4.0f) / 4.0f, th.color("cyanDark"), 0.94f * cont);
        float tw = float(a.img(id).w), thh = float(a.img(id).h);
        ops.push_back(Op::image(id, cx - tw / 2.0f, ty + 0.76f * cont - thh / 2.0f, tw, thh));
      }
    } else {
      const float px = th.f("fLogo") * h * k * car.type, lh = a.line_height(px);
      const std::vector<std::string> rows = wrap_centred(a, it.full_name, px, 0.94f * cont, std::max(1, int(0.7f * cont / lh)));
      float y = cy - lh * float(rows.size()) / 2.0f;
      for (const std::string& row : rows) {
        id = a.text_ellipsis(row, px, tile_tx, 0.94f * cont);
        float tw = float(a.img(id).w);
        ops.push_back(Op::image(id, cx - tw / 2.0f, y, tw, float(a.img(id).h)));
        y += lh;
      }
    }
    if (const char* label = icon_label(it.theme_name)) {
      id = a.text(label, std::round(th.f("fLabel") * h * k * car.type * 4.0f) / 4.0f, tile_tx);
      float tw = float(a.img(id).w), thh = float(a.img(id).h);
      ops.push_back(Op::image(id, cx - tw / 2.0f, ty + 0.671f * cont - thh / 2.0f, tw, thh));
    }
  }

  // Under the carousel: the system's name with a flat bar in its colour, the game count on the right, one hairline.
  // (nothing while the background count is pending: no "0 JUEGOS" flash, no layout shift)
  const SystemItem& cur = items[size_t(fade.item >= 0 && fade.item < n ? fade.item : cursor)];
  if (cur.games == kCountPending) return;
  const size_t info_from = ops.size();
  const ui::Style st = ui::Style::from(th);
  const remake::Grid grid(w, h);
  float mx = row_margin(w, h);
  const InfoGeom ig = info_geom(th, a, w, h, cur);
  float cy = ig.cy;
  float bar_w = std::max(2.0f, float(grid.u / 2));
  const float gap = float(grid.u) * 2.0f, name_x = car.bar ? mx + gap : mx;  // sysBar 0: no bar, the name starts at the margin
  ImgId info = a.text(games_text(cur.games), th.f("fSys") * h * car.type, th.color("dim"));
  float iw = float(a.img(info).w), ih = float(a.img(info).h);
  // a long name ends in an ellipsis before the game count (same gap as after the bar); a name that fits is drawn as before
  ImgId name = a.text_ellipsis(cur.full_name, std::round(th.f("fSys") * h * 1.75f * car.type), th.color("txt"), (w - mx - iw) - gap - name_x);
  float nw = float(a.img(name).w), nh = float(a.img(name).h);
  const float bar_h = ig.bar_h;
  if (car.bar) ops.push_back(Op::rect(mx, cy - bar_h / 2.0f, bar_w, bar_h, system_color(cur.theme_name, st)));
  ops.push_back(Op::image(name, name_x, std::round(cy - nh / 2.0f), nw, nh));
  audit_last(ops, "\"nombre sistema\"", 'L', !car.bar);  // next to the bar, which is the thing on the column
  ops.push_back(Op::image(info, w - mx - iw, std::round(cy - ih / 2.0f), iw, ih));
  audit_last(ops, "\"juegos\"", 'R');
  const float hair_y = ig.hair_y;
  ops.push_back(Op::rect(mx, hair_y, w - 2.0f * mx, std::max(1.0f, std::round(h / 720.0f)), st.line));
  if (recent && !recent->items.empty()) recent_row(th, a, st, w, h, mx, hair_y, *recent, ops);
  motion::fade(ops, info_from, fade.alpha);
}

void list_view(const Theme& th, Assets& a, const Common& c, float w, float h, const std::string& system_name, const std::vector<std::string>& names,
               size_t cursor, const Status& status, const DetailedData* detail, const std::vector<std::string>* marks, std::vector<Op>& ops) {
  if (remake::active(th)) {
    remake::list_view(th, a, w, h, system_name, names, cursor, status, detail, ops);
    return;
  }
  ops.push_back(Op::rect(0, 0, w, h, th.color("bg")));
  push_avatar(th, a, c, w, h, ops);
  const size_t status_first = ops.size();
  status_bar(th, a, w, h, status, ops);
  // the title ends in an ellipsis before the wifi / battery / clock stack instead of running into it
  float stack_left = w;
  for (size_t i = status_first; i < ops.size(); ++i)
    if (ops[i].kind == Op::Image && ops[i].w > 0) stack_left = std::min(stack_left, ops[i].x);
  const remake::Grid g(w, h);
  const float u = float(g.u);
  // the header: the avatar on the margin, the title after it on the next line of the cells, both centred on the status bar's row
  const float title_x = float(g.margin()) + up_cells(g, avatar_size(th, a, c, g).first + u / 2.0f);
  ImgId t = a.text_ellipsis(list_case(th, system_name, false), std::floor(th.f("fTitle") * h), th.color("txt"), stack_left - 2.0f * u - title_x);
  float tw = float(a.img(t).w), thh = float(a.img(t).h);
  ops.push_back(Op::image(t, title_x, std::round(float(g.status_y()) - thh / 2.0f), tw, thh));
  audit_last(ops, "titulo", 'L', false);  // after the avatar, which is the thing on the column
  help_bar(th, a, w, h, list_help(), ops);
  if (names.empty()) return;

  // the list on the first columns of the grid, the game's column on the rest; rows of whole cells, the selector as tall as a row
  const int list_cols = detail_list_cols(th, w, h);
  const float lx = g.col(0), lw = g.span(list_cols);
  const float ly = std::max(float(g.content_top()), line_y(g, th.f("listPosY") * h));
  size_t lines = size_t(th.f("listLines"));
  float px = std::floor(th.f("fList") * h);
  float row_h = std::max(down_cells(g, th.f("listH") * h / float(lines)), up_cells(g, px * 1.2f));
  row_h = std::min(row_h, std::max(u, down_cells(g, (float(g.help_top()) - ly) / float(lines))));
  float margin = std::max(u, std::round(th.f("listHM") * w / u) * u);
  const remake::RowWindow rw = remake::row_window(names.size(), cursor, lines);
  const ui::Style lst = ui::Style::from(th);
  ImgId pill;
  bool have_pill = a.file(th.path(th.str("pill")), pill);
  if (rw.moving) ops.push_back(Op::clip(0, ly, w, row_h * float(lines)));
  {
    const float top = ly + rw.sel * row_h;
    if (lst.sel_style != ui::SelStyle::Fill) {  // selStyle: ring / underline / bar instead of the pill image
      static ui::Shapes shapes;
      shapes.selector(a, lst, ops, lx, top, lw, row_h, Color{0, 0, 0, 0}, lst.accent());
    } else if (have_pill) ops.push_back(Op::image(pill, lx, top, lw, row_h));
    if (remake::audit_sink() && !rw.moving) remake::audit(lx, top, lw, row_h, "selector", 'B', true, true);  // its row's text sits on it: not measured apart
  }
  for (size_t idx = rw.first; idx < rw.end; ++idx) {
    float top = ly + (float(idx) - rw.start) * row_h;
    bool sel = idx == rw.lit;
    Color col = sel ? th.color("cyanDark") : th.color("dimDark");
    // "12 min ago" on the right of the row (only for games played recently); the name gives up that much room
    float room = lw - 2.0f * margin;
    if (marks && idx < marks->size() && !(*marks)[idx].empty()) {
      ImgId m = a.text((*marks)[idx], std::floor(px * 0.78f), sel ? col : th.color("dim"));
      float mw = float(a.img(m).w), mh = float(a.img(m).h);
      ops.push_back(Op::image(m, lx + lw - margin - mw, std::floor(top + row_h / 2.0f - mh / 2.0f), mw, mh));
      if (!sel) audit_last(ops, "\"marca\"", 'R', false);
      room -= mw + margin;
    }
    ImgId id = clip_text(a, list_case(th, names[idx], true), px, col, room);
    float iw = float(a.img(id).w), ih = float(a.img(id).h);
    ops.push_back(Op::image(id, lx + margin, std::floor(top + row_h / 2.0f - ih / 2.0f), iw, ih));
    if (!sel) audit_last(ops, "\"fila\"", 'L', false);  // inside its row, which is the thing on the column
  }
  if (rw.moving) ops.push_back(Op::unclip());
  if (detail) {
    const size_t detail_from = ops.size();
    detailed_column(th, a, w, h, *detail, ops);
    audit_from(ops, detail_from, "detalle");
    return;
  }
  // selected game name in the right column
  float cx = g.col(list_cols), cw = g.span(12 - list_cols);
  ImgId id = clip_text(a, list_case(th, names[cursor], false), std::floor(th.f("fName") * h), th.color("txt"), cw);
  float iw = float(a.img(id).w), ih = float(a.img(id).h);
  ops.push_back(Op::image(id, cx, std::round(line_y(g, th.f("nameY") * h) + std::max(u, std::round(th.f("nameH") * h / u) * u) / 2.0f - ih / 2.0f), iw, ih));
  audit_last(ops, "\"nombre juego\"");
}

}  // namespace zf
