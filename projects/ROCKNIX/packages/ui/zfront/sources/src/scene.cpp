#include "scene.hpp"

#include "scan.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

#include "covers.hpp"
#include "i18n.hpp"
#include "motion.hpp"
#include "services.hpp"
#include "ui_prefs.hpp"
#include "ui_style.hpp"
#include "util.hpp"

namespace zf {

namespace {

// Carousel parameters of the Zenith theme (`theme.xml`, `<carousel name="systemcarousel">`).
constexpr float kLogoScale = 1.23f;
constexpr float kScaledSpacing = 1.21f;
constexpr float kMaxLogoCount = 5.0f;

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
                       {"custom-collections", N_("COLLECTIONS"), {}},
                       {"ebook", N_("BOOKS"), {}}};
  static unsigned gen = ~0u;
  if (gen != i18n::generation()) {
    gen = i18n::generation();
    for (L& l : labels) l.shown = utf8_cap_first(tr(l.text));
  }
  for (const L& l : labels)
    if (theme_name == l.key) return l.shown.c_str();
  return nullptr;
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

// text image of `s`, cut (no ellipsis, like the scissor ES applies to list rows) until it fits in `max` pixels
ImgId clip_text(Assets& a, std::string_view s, float px, Color color, float max) {
  std::vector<size_t> ends;
  for (size_t i = 0; i < s.size();) {
    utf8_next(s, i);
    ends.push_back(i);
  }
  size_t n = ends.size();
  auto width_of = [&](size_t count) { return std::ceil(a.measure(s.substr(0, count == 0 ? 0 : ends[count - 1]), px)) + 1.0f; };
  while (n > 0 && width_of(n) > max) --n;
  return a.text(s.substr(0, n == 0 ? 0 : ends[n - 1]), px, color);
}

void push_avatar(const Theme& th, Assets& a, const Common& c, float w, float h, std::vector<Op>& ops) {
  const Img& img = a.img(c.avatar);
  auto [aw, ah] = fit(float(img.w), float(img.h), th.f("avW") * w, th.f("avH") * h);
  ops.push_back(Op::image(c.avatar, th.f("avX") * w, th.f("avY") * h - ah / 2.0f, aw, ah));
}

}  // namespace

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
  return help_for(c, {{"leftright", N_("CHOOSE")}, {"A", N_("SELECT")}, {"start", N_("MENU")}});
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

void status_bar(const Theme& th, Assets& a, float w, float h, const Status& st, std::vector<Op>& ops) {
  static thread_local BarCache c;
  const bool bt = ui_prefs::battery_text();
  bool hit = c.valid && c.uid == a.uid() && c.w == w && c.h == h && c.batt_text == bt && c.st == st && c.dir == th.dir();
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
  Color txt = th.color("txt");
  float right = w + th.f("stackX") * w;
  float top = th.f("stackY") * h, sh = th.f("stackH") * h;
  float cy = top + sh / 2.0f;
  float sep = th.f("stackSep") * w;
  // clock (hidden while the system time is not set) and battery percentage (only with a battery)
  std::string pct = st.battery && ui_prefs::battery_text() ? std::to_string(st.battery->percent) + "%" : std::string();
  struct T {
    const std::string* text;
    float px;
  } texts[] = {{&st.time, std::floor(th.f("fClock") * h)}, {&pct, std::floor(th.f("fBatt") * h)}};
  for (const T& t : texts) {
    if (t.text->empty()) continue;
    ImgId id = a.text(*t.text, t.px, txt);
    float tw = float(a.img(id).w), thh = float(a.img(id).h);
    ops.push_back(Op::image(id, right - tw, cy - thh / 2.0f, tw, thh));
    right -= tw + sep;
  }
  const char* icons[2] = {st.battery ? Status::battery_icon(*st.battery) : nullptr, st.wifi_icon()};
  for (const char* icon : icons) {
    if (!icon) continue;
    ImgId id;
    if (a.svg(th.path(std::string("./assets/icons/") + icon), uint32_t(std::lround(sh)), id)) {
      float iw = float(a.img(id).w), ih = float(a.img(id).h);
      ops.push_back(Op::image(id, right - iw, cy - ih / 2.0f, iw, ih, txt));
      right -= iw + sep;
    }
  }
}

// Help bar: a floating pill, see ui::help_pill. The icons are the theme's help-<icon>.png masks, tinted.
void help_bar(const Theme& th, Assets& a, float w, float h, const HelpPrompts& prompts, std::vector<Op>& ops) {
  const float px = std::floor(th.f("fHelp") * h);
  std::vector<ui::HelpItem> items;
  for (const auto& [icon, text] : prompts) {
    ImgId iid;
    if (!a.file(th.path(std::string("./assets/help-") + icon + ".png"), iid)) continue;
    ImgId tid = a.text(text, px, Color{255, 255, 255, 255});
    items.push_back({iid, float(a.img(iid).w), float(a.img(iid).h), tid, float(a.img(tid).w), float(a.img(tid).h), std::string(icon) == "A"});
  }
  ui::help_pill(a, ui::Style::from(th), w, th.f("helpY") * h, px, items, ops);
}

/// Muted colour of a system's bar under the carousel, by maker; anything unlisted gets the theme's own accent.
/// Where the row of "continue playing" sits under the hairline `hair_y` (one place for the drawing and for the keys). `fits` is false
/// when the screen has not the room: on a short screen the row is left out, never squeezed.
namespace {
struct RecentGeom {
  float px = 0, label_y = 0, label_h = 0, tile_top = 0, cap_h = 0, tile_h = 0, tile_w = 0, gap = 0;
  size_t fit_n = 0;
  ImgId label = 0;
  bool fits = false;
};
}  // namespace

static float row_margin(float w) { return std::round(0.044f * w); }  // same side margin as the status bar

/// y of the hairline under the system's name and count.
static float hairline_y(const Theme& th, Assets& a, float w, float h, const SystemItem& cur) {
  ImgId name = a.text(cur.full_name, std::round(th.f("fSys") * h * 1.75f), th.color("txt"));
  const float bar_h = std::round(float(a.img(name).h) * 0.95f);
  return std::round(th.f("sysInfoY") * h - carousel_lift(w, h) + bar_h / 2.0f + 0.011f * h);  // follows the name when the carousel is lifted
}

static RecentGeom recent_geom(const Theme& th, Assets& a, float w, float h, float mx, float hair_y) {
  RecentGeom g;
  g.px = std::round(th.f("fSys") * h);
  g.label = a.text(i18n::sentence_case(tr("CONTINUE PLAYING")), g.px, th.color("dim"));
  g.label_h = float(a.img(g.label).h);
  g.label_y = std::round(hair_y + 0.012f * h);
  g.tile_top = std::round(g.label_y + g.label_h + 0.008f * h);
  g.cap_h = std::round(g.px * 2.0f + 0.008f * h);
  const float limit = th.f("helpY") * h - 0.05f * h;  // clear of the help pill
  g.tile_h = std::min(std::round(0.125f * h), std::floor(limit - g.tile_top - g.cap_h));
  if (g.tile_h < std::round(0.085f * h)) return g;
  g.tile_w = std::round(g.tile_h * 16.0f / 9.0f), g.gap = std::round(0.022f * w);
  g.fit_n = size_t(std::max(0.0f, (w - 2.0f * mx + g.gap) / (g.tile_w + g.gap)));
  g.fits = true;
  return g;
}

std::vector<size_t> recent_visible(const Theme& th, Assets& a, float w, float h, const SystemItem& cur, const RecentRow& r) {
  std::vector<size_t> out;
  if (r.items.empty() || cur.games == kCountPending) return out;
  const float hair_y = hairline_y(th, a, w, h, cur);
  const RecentGeom g = recent_geom(th, a, w, h, row_margin(w), hair_y);
  if (!g.fits) return out;
  for (size_t i = 0; i < r.items.size() && out.size() < g.fit_n; ++i)
    if (!covers::failed(r.items[i].shot)) out.push_back(i);
  return out;
}

/// "Continue playing": the newest savestate screenshots of the selected system, left aligned under the hairline. Drawn only when the
/// screen has the room (about 0.2 of its height). The tile `r.focus` carries a ring in the theme's cyan, like the carousel's selector.
static void recent_row(const Theme& th, Assets& a, const ui::Style& st, float w, float h, float mx, float hair_y, RecentRow& r, std::vector<Op>& ops) {
  static ui::Shapes shapes;
  const RecentGeom g = recent_geom(th, a, w, h, mx, hair_y);
  if (!g.fits) return;
  const float px = g.px, tile_top = g.tile_top, tile_h = g.tile_h, tile_w = g.tile_w, gap = g.gap;
  const Color accent = st.dark ? st.cyan : st.cyan_dark;  // caption (readable on the light panel)
  r.box_w = uint32_t(tile_w) - 2, r.box_h = uint32_t(tile_h) - 2;
  ops.push_back(Op::image(g.label, mx, g.label_y, float(a.img(g.label).w), g.label_h));
  size_t k = 0;
  for (size_t idx = 0; idx < r.items.size(); ++idx) {
    const RecentItem& it = r.items[idx];
    if (k >= g.fit_n) break;
    if (covers::failed(it.shot)) continue;
    const float x = mx + float(k) * (tile_w + gap);
    const bool foc = int(idx) == r.focus && r.focus_a > 0.0f;
    if (foc) {  // 2 px ring outside the tile + the tile's own border in the same colour (the capture covers 1 px of the border)
      shapes.rrect(a, ops, x - 2.0f, tile_top - 2.0f, tile_w + 4.0f, tile_h + 4.0f, 5, ui::with_alpha(st.cyan, r.focus_a));
      shapes.framed(a, ops, x, tile_top, tile_w, tile_h, 3, st.panel, ui::mix(st.line, st.cyan, r.focus_a), 1.0f);
    } else {
      shapes.framed(a, ops, x, tile_top, tile_w, tile_h, 3, st.panel, st.line, 1.0f);
    }
    ImgId id;
    const bool have = r.decode_inline ? a.file_fit(it.shot, r.box_w, r.box_h, id) : covers::ready(a, it.shot, r.box_w, r.box_h, id);
    if (!have && r.decode_inline) covers::mark_failed(it.shot);
    if (have) {
      const Img& im = a.img(id);
      const float iw = float(im.w), ih = float(im.h);
      ops.push_back(Op::image(id, std::round(x + (tile_w - iw) / 2.0f), std::round(tile_top + (tile_h - ih) / 2.0f), iw, ih));
    }
    const float cy0 = tile_top + tile_h + std::round(0.008f * h);
    ImgId nm = clip_text(a, short_title(it.name), px, foc && r.focus_a >= 0.5f ? accent : st.txt, tile_w);
    ops.push_back(Op::image(nm, x, cy0, float(a.img(nm).w), float(a.img(nm).h)));
    if (!it.ago.empty()) {
      ImgId ag = a.text(it.ago, std::round(px * 0.9f), st.dim);
      ops.push_back(Op::image(ag, x, cy0 + float(a.img(nm).h), float(a.img(ag).w), float(a.img(ag).h)));
    }
    ++k;
  }
}

static Color system_color(const std::string& t, const ui::Style& st) {
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

void carousel_span(float cam, int n, int& first, int& last) {
  const int center = int(std::floor(cam));
  const int half = int(std::min(kMaxLogoCount, float(n))) / 2;
  // at rest tiles are drawn up to `half + 1` away, but the next one is already visible (partly) from 0.4 tiles before it gets there
  first = center - half - 2;
  last = center + half + 2;
}

void system_view(const Theme& th, Assets& a, const Common& c, float w, float h, const std::vector<SystemItem>& items, int cursor_abs, float cam,
                 const Status& status, RecentRow* recent, const InfoFade& fade, std::vector<Op>& ops) {
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
  float spacing = (car_w - logo_w * kMaxLogoCount) / kMaxLogoCount + logo_w;
  float diff_x = (spacing * kLogoScale - spacing) / 2.0f * kScaledSpacing;
  float nominal = logo_w * kLogoScale;  // container size at scale 1.0
  Color tile_tx = th.color("tileTxt");

  int first = 0, last = 0;
  carousel_span(cam, n, first, last);
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
    float scale = std::min(std::max(1.0f + (kLogoScale - 1.0f) * (1.0f - std::fabs(dist)), 1.0f), kLogoScale);
    float cont = nominal * scale / kLogoScale;  // container size in px
    float k = cont / nominal;                   // text/children scale
    float side = sel ? 0.0f : (i < cursor_abs ? -diff_x : diff_x);
    float cx = ccx + dist * spacing + side;
    float cy = ccy;
    if (cx + cont * 0.75f < 0.0f || cx - cont * 0.75f > w) continue;
    float ty = cy - cont / 2.0f;
    // tile and frame images are 1.5x the container, centred
    float big = cont * 1.5f;
    ops.push_back(Op::image(c.tile, cx - big / 2.0f, cy - big / 2.0f, big, big));
    float fa = std::min(std::max(1.0f - std::fabs(dist), 0.0f), 1.0f);
    if (fa > 0.0f) ops.push_back(Op::image(c.frame, cx - big / 2.0f, cy - big / 2.0f, big, big, {255, 255, 255, uint8_t(fa * 255.0f)}));
    // logo (maxSize 0.9 x 0.5 of the container) or the system name when the theme has no logo
    std::string logo_path = th.path("./assets/logos/" + it.theme_name + ".webp");
    ImgId id;
    if (file_exists(logo_path)) {
      if (a.file(logo_path, id)) {
        auto [lw, lh] = fit(float(a.img(id).w), float(a.img(id).h), 0.90f * cont, 0.5f * cont);
        ops.push_back(Op::image(id, cx - lw / 2.0f, cy - lh / 2.0f, lw, lh));
      }
    } else {
      id = a.text(it.full_name, th.f("fLogo") * h * k, tile_tx);
      float tw = float(a.img(id).w), thh = float(a.img(id).h);
      ops.push_back(Op::image(id, cx - tw / 2.0f, cy - thh / 2.0f, tw, thh));
    }
    if (const char* label = icon_label(it.theme_name)) {
      id = a.text(label, std::round(th.f("fLabel") * h * k * 4.0f) / 4.0f, tile_tx);
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
  float mx = row_margin(w);
  float cy = th.f("sysInfoY") * h - lift;
  ImgId name = a.text(cur.full_name, std::round(th.f("fSys") * h * 1.75f), th.color("txt"));
  float nw = float(a.img(name).w), nh = float(a.img(name).h);
  float bar_w = std::max(3.0f, std::round(0.0056f * w)), bar_h = std::round(nh * 0.95f);
  ops.push_back(Op::rect(mx, std::round(cy - bar_h / 2.0f), bar_w, bar_h, system_color(cur.theme_name, st)));
  ops.push_back(Op::image(name, mx + bar_w + std::round(0.0222f * w), cy - nh / 2.0f, nw, nh));
  ImgId info = a.text(games_text(cur.games), th.f("fSys") * h, th.color("dim"));
  float iw = float(a.img(info).w), ih = float(a.img(info).h);
  ops.push_back(Op::image(info, w - mx - iw, cy - ih / 2.0f, iw, ih));
  const float hair_y = hairline_y(th, a, w, h, cur);
  ops.push_back(Op::rect(mx, hair_y, w - 2.0f * mx, std::max(1.0f, std::round(h / 720.0f)), st.line));
  if (recent && !recent->items.empty()) recent_row(th, a, st, w, h, mx, hair_y, *recent, ops);
  motion::fade(ops, info_from, fade.alpha);
}

void list_view(const Theme& th, Assets& a, const Common& c, float w, float h, const std::string& system_name, const std::vector<std::string>& names,
               size_t cursor, const Status& status, const DetailedData* detail, const std::vector<std::string>* marks, std::vector<Op>& ops) {
  ops.push_back(Op::rect(0, 0, w, h, th.color("bg")));
  push_avatar(th, a, c, w, h, ops);
  ImgId t = a.text(system_name, std::floor(th.f("fTitle") * h), th.color("txt"));
  float tw = float(a.img(t).w), thh = float(a.img(t).h);
  ops.push_back(Op::image(t, th.f("nameX") * w, th.f("avY") * h - thh / 2.0f, tw, thh));
  status_bar(th, a, w, h, status, ops);
  help_bar(th, a, w, h, list_help(), ops);
  if (names.empty()) return;

  float lx = th.f("listX") * w, ly = th.f("listPosY") * h;
  float lw = th.f("listW") * w, lh = th.f("listH") * h;
  size_t lines = size_t(th.f("listLines"));
  float row_h = lh / float(lines);
  float px = std::floor(th.f("fList") * h);
  float margin = th.f("listHM") * w;
  size_t half = lines / 2;
  size_t start = cursor > half ? cursor - half : 0;
  size_t max_start = names.size() > lines ? names.size() - lines : 0;
  start = std::min(start, max_start);
  ImgId pill;
  bool have_pill = a.file(th.path(th.str("pill")), pill);
  size_t end = std::min(names.size(), start + lines);
  for (size_t idx = start, row = 0; idx < end; ++idx, ++row) {
    float top = ly + float(row) * row_h;
    bool sel = idx == cursor;
    if (sel && have_pill) ops.push_back(Op::image(pill, lx, top + th.f("selOffY") * h, lw, th.f("selH") * h));
    Color col = sel ? th.color("cyanDark") : th.color("dimDark");
    // "12 min ago" on the right of the row (only for games played recently); the name gives up that much room
    float room = lw - 2.0f * margin;
    if (marks && idx < marks->size() && !(*marks)[idx].empty()) {
      ImgId m = a.text((*marks)[idx], std::floor(px * 0.78f), sel ? col : th.color("dim"));
      float mw = float(a.img(m).w), mh = float(a.img(m).h);
      ops.push_back(Op::image(m, lx + lw - margin - mw, std::floor(top + row_h / 2.0f - mh / 2.0f), mw, mh));
      room -= mw + margin;
    }
    ImgId id = clip_text(a, utf8_upper(names[idx]), px, col, room);
    float iw = float(a.img(id).w), ih = float(a.img(id).h);
    ops.push_back(Op::image(id, lx + margin, std::floor(top + row_h / 2.0f - ih / 2.0f), iw, ih));
  }
  if (detail) {
    detailed_column(th, a, w, h, *detail, ops);
    return;
  }
  // selected game name in the right column
  float cx = th.f("colX") * w, cw = th.f("colW") * w;
  ImgId id = clip_text(a, names[cursor], std::floor(th.f("fName") * h), th.color("txt"), cw);
  float iw = float(a.img(id).w), ih = float(a.img(id).h);
  ops.push_back(Op::image(id, cx, th.f("nameY") * h + th.f("nameH") * h / 2.0f - ih / 2.0f, iw, ih));
}

}  // namespace zf
