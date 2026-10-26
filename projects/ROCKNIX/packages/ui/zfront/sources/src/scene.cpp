#include "scene.hpp"

#include "scan.hpp"

#include <algorithm>
#include <cmath>
#include <deque>

#include "i18n.hpp"
#include "services.hpp"
#include "ui_prefs.hpp"
#include "util.hpp"

namespace zf {

namespace {

// Carousel parameters of the Zenith theme (`theme.xml`, `<carousel name="systemcarousel">`).
constexpr float kLogoScale = 1.23f;
constexpr float kScaledSpacing = 1.21f;
constexpr float kMaxLogoCount = 5.0f;

const Color kWhite = {255, 255, 255, 255};

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

/// "14 GAMES" under the carousel: ES's plural entry `%i GAME`, in capitals like the rest of the carousel.
std::string games_text(uint32_t n) {
  std::string t = i18n::ntr("%i GAME", "%i GAMES", n);
  size_t p = t.find("%i");
  if (p == std::string::npos) p = t.find("%d");
  if (p != std::string::npos) t.replace(p, 2, std::to_string(n));
  return utf8_upper(t);
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
/// A help bar: (icon, ES catalog key) pairs; the text is translated and put in capitals once per language.
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
      c.text.push_back(utf8_upper(tr(k.second)));
      c.prompts.push_back({k.first, c.text.back().c_str()});
    }
  }
  return c.prompts;
}
}  // namespace

const HelpPrompts& system_help() {
  static HelpCache c;
  return help_for(c, {{"start", N_("MENU")}, {"B", N_("NAVIGATION BAR")}, {"X", N_("RANDOM")}, {"A", N_("SELECT")}, {"leftright", N_("CHOOSE")}});
}
const HelpPrompts& list_help() {
  static HelpCache c;
  return help_for(c, {{"select", N_("OPTIONS")}, {"start", N_("MENU")}, {"B", N_("BACK")}, {"Y", N_("SEARCH")}, {"X", N_("GAME OPTIONS")}, {"A", N_("LAUNCH")}});
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
  ops.insert(ops.end(), c.ops.begin(), c.ops.end());
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

// Help bar (ES `HelpComponent`): [icon] [text] pairs, icon as tall as 1.25x the capital letter height, centred at
// (0.5, helpY).
void help_bar(const Theme& th, Assets& a, float w, float h, const HelpPrompts& prompts, std::vector<Op>& ops) {
  float px = std::floor(th.f("fHelp") * h);
  Color col = th.color("dimDark");
  float height = std::round(a.letter_height(px) * 1.25f);
  float icon_gap = w * 0.0042f;
  float entry_gap = icon_gap * 2.0f;
  struct E {
    ImgId icon;
    float iw;
    ImgId text;
    float tw;
  };
  std::vector<E> items;
  float total = 0;
  for (const auto& [icon, text] : prompts) {
    ImgId iid;
    if (!a.file(th.path(std::string("./assets/help-") + icon + ".png"), iid)) continue;
    ImgId tid = a.text(text, px, col);
    float iw = float(a.img(iid).w) * height / float(a.img(iid).h);
    float tw = float(a.img(tid).w);
    total += iw + icon_gap + tw + entry_gap;
    items.push_back({iid, iw, tid, tw});
  }
  float x = (w - total) / 2.0f;
  float cy = th.f("helpY") * h;
  for (const E& e : items) {
    ops.push_back(Op::image(e.icon, x, cy - height / 2.0f, e.iw, height, col));
    x += e.iw + icon_gap;
    float th_ = float(a.img(e.text).h);
    ops.push_back(Op::image(e.text, x, cy - th_ / 2.0f, e.tw, th_));
    x += e.tw + entry_gap;
  }
}

void system_view(const Theme& th, Assets& a, const Common& c, float w, float h, const std::vector<SystemItem>& items, int cursor_abs, float cam,
                 const Status& status, std::vector<Op>& ops) {
  ops.push_back(Op::rect(0, 0, w, h, th.color("bg")));
  push_avatar(th, a, c, w, h, ops);
  status_bar(th, a, w, h, status, ops);
  help_bar(th, a, w, h, system_help(), ops);
  if (items.empty()) return;

  int n = int(items.size());
  int cursor = rem_euclid(cursor_abs, n);

  // carousel box: size (carW x 0.5), centred at (0.5, carY)
  float logo_w = th.f("cellW") * w;
  float car_w = th.f("carW") * w;
  float ccx = w / 2.0f, ccy = th.f("carY") * h;
  float logo_count = std::min(kMaxLogoCount, float(n));
  float spacing = (car_w - logo_w * kMaxLogoCount) / kMaxLogoCount + logo_w;
  float diff_x = (spacing * kLogoScale - spacing) / 2.0f * kScaledSpacing;
  float nominal = logo_w * kLogoScale;  // container size at scale 1.0
  Color tile_tx = th.color("tileTxt");

  int center = int(std::floor(cam));
  int half = int(logo_count) / 2;
  std::vector<std::pair<int, bool>> order;
  for (int i = center - half - 1; i <= center + half + 1; ++i) order.push_back({i, rem_euclid(i, n) == cursor});
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

  // "N JUEGOS" under the carousel
  // (nothing while the background count is pending: no "0 JUEGOS" flash, no layout shift)
  if (items[size_t(cursor)].games == kCountPending) return;
  ImgId info = a.text(games_text(items[size_t(cursor)].games), th.f("fSys") * h, th.color("dim"));
  float iw = float(a.img(info).w), ih = float(a.img(info).h);
  ops.push_back(Op::image(info, w / 2.0f - iw / 2.0f, th.f("sysInfoY") * h - ih / 2.0f, iw, ih));
}

void list_view(const Theme& th, Assets& a, const Common& c, float w, float h, const std::string& system_name, const std::vector<std::string>& names,
               size_t cursor, const Status& status, const DetailedData* detail, std::vector<Op>& ops) {
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
    ImgId id = clip_text(a, utf8_upper(names[idx]), px, col, lw - 2.0f * margin);
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
