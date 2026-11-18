// The user's own look of APPEARANCE (SELECTOR STYLE, TITLE ALIGNMENT, LIST TEXT CASE) over EVERY theme zfront draws: Zenith (Minimal) light and dark,
// Zenith Next light and dark, XMB, Art Book, Iconic light and dark, TechDweeb and the three skins of Iconic (Aurora, Swiss, Industrial) light and dark.
// "All the themes can be bent": with the choice empty ("Theme default") a theme is drawn exactly as it always was, with a choice made it obeys. Measured on
// the pixels the software renderer draws, never by eye:
//  - default: a hash of each picture (menu, choice dialog, game list with and without details, system carousel) in tests/data/theme_default.hash, made
//    BEFORE any theme read the choices (ZF_THEME_SEL_WRITE=1 writes the file again);
//  - menus: `fill` is the theme's own focus (same pixels as the default), outline / underline / bar are three more shapes, all different; the label of the
//    focused row keeps 4.5:1 in the three new ones; the title centred is centred on the page and changes only the header;
//  - choice dialogs: the five selectors and the centred title change the picture;
//  - game list: LIST TEXT CASE forces the case of the rows (same pixels as the names written in that case);
//  - a choice made while the menu is open is drawn at once (same pixels as a menu opened after it);
//  - warm frames allocate nothing with every selector.
// ZF_THEME_SEL_REPORT=1 prints the table "does it change" of every theme x setting x place. ZF_THEME_SEL_PNG=<folder> saves the pictures at 640x480.
// Needs theme-light and theme-dark (ZF_THEME_DIR).
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "alloc_nothrow.hpp"
#include "app.hpp"
#include "bootargs.hpp"
#include "check.hpp"
#include "cpu.hpp"
#include "es_theme.hpp"
#include "gfx.hpp"
#include "i18n.hpp"
#include "menu.hpp"
#include "menu_ui.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "remake.hpp"
#include "scene.hpp"
#include "status.hpp"
#include "theme.hpp"
#include "ui_dialogs.hpp"
#include "ui_prefs.hpp"
#include "ui_style.hpp"
#include "util.hpp"

using namespace zf;
namespace fs = std::filesystem;

// Allocation counter (this test binary only): the steady-state frame of a menu must not allocate.
static size_t g_allocs = 0;
void* operator new(size_t n) {
  ++g_allocs;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

std::string find_base(const char* name) {
  if (const char* e = std::getenv("ZF_THEME_DIR")) {
    const std::string p = std::string(e) + "/" + name;
    if (fs::exists(p + "/theme.xml")) return p;
  }
  for (const char* d : {"themes/", "../themes/", "./"}) {
    const std::string p = std::string(d) + name;
    if (fs::exists(p + "/theme.xml")) return p;
  }
  return {};
}

void write(const std::string& p, const std::string& t) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream(p) << t;
}

uint64_t fnv(const Canvas& cv, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
  uint64_t h = 1469598103934665603ull;
  x1 = std::min(x1, cv.w), y1 = std::min(y1, cv.h);
  for (uint32_t y = y0; y < y1; ++y)
    for (uint32_t x = x0; x < x1; ++x)
      for (int c = 0; c < 4; ++c) h = (h ^ cv.px[(size_t(y) * cv.w + x) * 4 + size_t(c)]) * 1099511628211ull;
  return h;
}
uint64_t fnv(const Canvas& cv) { return fnv(cv, 0, 0, cv.w, cv.h); }

std::string hex(uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
  return b;
}

double lin(uint8_t c) {
  const double s = double(c) / 255.0;
  return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
}
double lum(uint32_t rgb) { return 0.2126 * lin(uint8_t(rgb >> 16)) + 0.7152 * lin(uint8_t(rgb >> 8)) + 0.0722 * lin(uint8_t(rgb)); }

/// Contrast (WCAG) of the ink of a text box against what is behind it: the background is the most frequent colour of the box, the ink the colour
/// that is farthest from it in luminance.
double box_contrast(const Canvas& cv, const remake::Box& b) {
  std::unordered_map<uint32_t, int> seen;
  const int x0 = std::max(0, int(b.x)), y0 = std::max(0, int(b.y)), x1 = std::min(int(cv.w), int(std::ceil(b.x + b.w))), y1 = std::min(int(cv.h), int(std::ceil(b.y + b.h)));
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      const uint8_t* p = &cv.px[(size_t(y) * cv.w + size_t(x)) * 4];
      ++seen[uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]];
    }
  if (seen.empty()) return 0.0;
  uint32_t bg = 0;
  int best = -1;
  for (const auto& [c, n] : seen)
    if (n > best) best = n, bg = c;
  const double lb = lum(bg);
  double far = 0.0, lf = lb;
  for (const auto& [c, n] : seen) {
    (void)n;
    const double d = std::fabs(lum(c) - lb);
    if (d > far) far = d, lf = lum(c);
  }
  return (std::max(lb, lf) + 0.05) / (std::min(lb, lf) + 0.05);
}

struct Look {
  std::string sel, align, lcase;
};

/// The user's choices as main.cpp applies them at start (system.cfg, then the theme's variables) and ui::relayout_all as APPEARANCE does it live.
void set_look(Ctx& cx, const std::string& tmp, const Look& l) {
  std::string cfg = "system.language=en_US\naudio.volume=40\n";
  if (!l.sel.empty()) cfg += "zfront.sel_style=" + l.sel + "\n";
  if (!l.align.empty()) cfg += "zfront.header_align=" + l.align + "\n";
  if (!l.lcase.empty()) cfg += "zfront.list_case=" + l.lcase + "\n";
  write(tmp + "/system.cfg", cfg);
  cx.conf = SystemConf::load(tmp + "/system.cfg");
  for (const boot::UserThemeKey& k : boot::user_theme_keys()) boot::set_user_theme(cx.th, k.key, "");
  boot::apply_user_theme(cx.th, [&](const std::string& k) { return cx.conf.get_or(k, ""); });
  ui::relayout_all();
}

Ctx* make_ctx(const std::string& dir, int w, int h, const std::string& tmp) {
  static std::vector<std::unique_ptr<Ctx>> keep;  // every Ctx stays alive: the caches of the shapes are keyed on the address of the Assets
  keep.push_back(std::make_unique<Ctx>());
  Ctx& cx = *keep.back();
  std::string err;
  CHECK(Theme::load(dir, ratio_name(unsigned(w), unsigned(h)), cx.th, err));
  CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
  if (const std::string_view alt = cx.th.str("esFontAlt", ""); !alt.empty()) cx.assets.load_alt_font(std::string(alt), err);
  cx.w = float(w), cx.h = float(h);
  cx.sv->cmd.dry_run = true;
  write(tmp + "/es_settings.cfg", "");
  cx.es = EsSettings::load(tmp + "/es_settings.cfg");
  SystemConf::lock_path() = tmp + "/lock";
  oscontract::reset_for_test();
  ui_reset_dry_state();
  oscontract::Paths& p = oscontract::paths();
  p.backlight_dir = tmp + "/sys/class/backlight";
  p.ready_file = tmp + "/ready";
  p.kill_file = tmp + "/kill";
  p.tmp_dir = tmp;
  p.suspend_helper = tmp + "/pm-suspend";
  oscontract::startup(cx.sv, true);
  cx.status = Status::demo("12:04");
  cx.status.bluetooth = true;
  // the THEME SET row shows this value: fixed, so the fingerprints do not follow the image's default theme (boot::kDefaultThemeSet)
  cx.es.set_string("ThemeSet", "es-theme-zenith");
  set_look(cx, tmp, Look{});
  ui_prefs::on_status(cx, cx.status);
  return &cx;
}

/// APPEARANCE with the focus on SELECTOR STYLE (a row in the middle of the list).
std::unique_ptr<Screen> open_appearance(Ctx& cx) {
  auto s = Registry::instance().make("menu.ui", cx, "");
  CHECK(s != nullptr);
  if (!s) return s;
  std::vector<Op> ops;
  s->tick(1.0f, cx);
  s->draw(cx, ops);
  Menu& m = static_cast<Menu&>(*s);
  bool found = false;
  for (int i = 0; i < 40 && !found; ++i) {
    found = m.cursor() >= 0 && m.entry(size_t(m.cursor())).label == tr("SELECTOR STYLE");
    if (!found) s->handle(Action::Down, cx);
  }
  CHECK(found);
  return s;
}

struct Shot {
  Canvas cv;
  std::vector<remake::Box> boxes;
  ui::HeaderBox hb;
};

Shot shoot(Screen& s, Ctx& cx) {
  Shot r;
  remake::audit_sink() = &r.boxes;
  ui::header_box() = ui::HeaderBox{};
  std::vector<Op> ops;
  for (int i = 0; i < 3; ++i) s.tick(1.0f, cx);
  s.draw(cx, ops);
  remake::audit_sink() = nullptr;
  r.hb = ui::header_box();
  r.cv = cpu_render(ops, cx.assets, uint32_t(cx.w), uint32_t(cx.h));
  return r;
}

std::map<std::string, std::string> g_ref;  // name -> hash of the default look, from tests/data/theme_default.hash
std::map<std::string, std::string> g_new;
bool g_write = false;
bool g_report = false;
/// With the theme's own look ("Theme default") the focused row must at least be readable: 3:1. (The looks already approved by the owner sit between 3 and
/// 4.5: Industrial, Zenith and Next in light.)
constexpr double kMinDefault = 3.0;

const char* const kStyles[] = {"", "fill", "outline", "underline", "bar"};
const char* name_of(int k) { return kStyles[k][0] ? kStyles[k] : "default"; }

/// The label box of the focused row of a menu / of the dialog option `label`.
bool label_box(const std::vector<remake::Box>& boxes, const std::string& label, remake::Box& out) {
  const std::string tag = "\"" + label.substr(0, 18) + "\"";
  for (const remake::Box& b : boxes)
    if (b.tag == tag) {
      out = b;
      return true;
    }
  return false;
}

/// The default look must be what it always was: the whole picture, compared with the hash made before the themes read the choices.
void check_default(const std::string& key, uint64_t h) {
  if (g_write) {
    g_new[key] = hex(h);
    return;
  }
  const auto it = g_ref.find(key);
  CHECK(it != g_ref.end());
  if (it != g_ref.end() && it->second != hex(h)) std::printf("MAL %s: con Theme default ya no se dibuja como antes (%s != %s)\n", key.c_str(), hex(h).c_str(), it->second.c_str());
  if (it != g_ref.end()) CHECK(it->second == hex(h));
}

std::string cell(uint64_t a, uint64_t b) { return a == b ? "no" : "SI"; }

struct Fixture {
  std::vector<std::string> mixed = {"Super Mario World", "zelda: A Link", "Final Fantasy", "Sonic the Hedgehog"};
  std::vector<SystemItem> items{5};
  Fixture() {
    const char* sys[][2] = {{"snes", "Super Nintendo"}, {"megadrive", "Sega Mega Drive"}, {"psp", "PlayStation Portable"}, {"gba", "Game Boy Advance"}, {"nes", "Nintendo Entertainment System"}};
    for (size_t i = 0; i < items.size(); ++i) {
      items[i].theme_name = sys[i][0];
      items[i].full_name = sys[i][1];
      items[i].games = 19;
    }
  }
};

uint64_t dialog_hash(Ctx& cx, Shot* keep = nullptr, double* contrast = nullptr) {
  std::vector<Option> opts;
  for (int i = 0; i < 6; ++i) opts.push_back({"v" + std::to_string(i), "Option number " + std::to_string(i)});
  ui::ChoiceDialog d("PICK ONE", opts, "v1", [](Ctx&, const std::string&) {});
  Shot sh = shoot(d, cx);
  if (contrast) {  // the fill selector: ink of the focused option on the fill of the selector, over the card (the same colours the dialog draws with)
    const ui::Style st = ui::Style::from(cx.th);
    if (st.sel_style == ui::SelStyle::Fill) {
      const float al = float(st.cyan_soft[3]) / 255.0f;
      uint32_t fill = 0;
      for (int c = 0; c < 3; ++c) fill = fill << 8 | uint32_t(std::lround(float(st.cyan_soft[size_t(c)]) * al + float(st.panel[size_t(c)]) * (1.0f - al)));
      const uint32_t ink = uint32_t(st.sel_ink[0]) << 16 | uint32_t(st.sel_ink[1]) << 8 | st.sel_ink[2];
      const double a = lum(fill), b = lum(ink);
      *contrast = (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
    } else {
      *contrast = -1.0;
    }
  }
  const uint64_t h = fnv(sh.cv);
  if (keep) *keep = std::move(sh);
  return h;
}

/// The game list: with the theme's own screen (remake for the themes that have one). `detail` adds the column of the game.
uint64_t list_hash(Ctx& cx, const Fixture& fx, const std::vector<std::string>& names, bool detail, Canvas* keep = nullptr, const std::string& sys = "Super Nintendo",
                   double* contrast = nullptr) {
  Common common;
  std::string err;
  if (!remake::active(cx.th)) CHECK(load_common(cx.th, cx.assets, common, err));
  remake::state().inline_decode = true;
  remake::state().system_theme = "snes";
  DetailedData d;
  d.name = "Super Mario World";
  d.year = 1994;
  d.genre = "Platform";
  d.players = "1-2";
  d.rating = 0.7f;
  d.desc = "A short description of the game.";
  d.desc_time = -1;
  std::vector<Op> ops;
  list_view(cx.th, cx.assets, common, cx.w, cx.h, sys, names, 1, cx.status, detail ? &d : nullptr, nullptr, ops);
  (void)fx;
  Canvas cv = cpu_render(ops, cx.assets, uint32_t(cx.w), uint32_t(cx.h));
  if (contrast) {  // the name of the focused row (the same text is also drawn on the page, on the right: the worst of the boxes counts)
    std::vector<remake::Box> boxes;
    remake::audit_sink() = &boxes;
    std::vector<Op> ops2;
    list_view(cx.th, cx.assets, common, cx.w, cx.h, sys, names, 1, cx.status, detail ? &d : nullptr, nullptr, ops2);
    remake::audit_sink() = nullptr;
    const std::string tag = "\"" + names[1].substr(0, 18) + "\"";
    double worst = 1e9;
    for (const remake::Box& b : boxes) {
      if (b.tag == tag || b.tag == "selector") worst = std::min(worst, box_contrast(cv, b));
    }
    *contrast = worst > 1e8 ? -1.0 : worst;
  }
  const uint64_t h = fnv(cv);
  if (keep) *keep = std::move(cv);
  return h;
}

uint64_t carousel_hash(Ctx& cx, const Fixture& fx) {
  Common common;
  std::string err;
  if (!remake::active(cx.th)) CHECK(load_common(cx.th, cx.assets, common, err));
  remake::state().inline_decode = true;
  remake::state().system_theme = "snes";
  RecentRow none;
  none.decode_inline = true;
  InfoFade fade;
  fade.item = 0;
  std::vector<Op> ops;
  system_view(cx.th, cx.assets, common, cx.w, cx.h, fx.items, 0, 0.0f, cx.status, &none, fade, ops);
  return fnv(cpu_render(ops, cx.assets, uint32_t(cx.w), uint32_t(cx.h)));
}

/// Allocations of 30 warm frames of the game list (`detail`: with the game's column) or of the system carousel, at rest.
size_t scene_allocs(Ctx& cx, const Fixture& fx, int what) {
  Common common;
  std::string err;
  if (!remake::active(cx.th)) CHECK(load_common(cx.th, cx.assets, common, err));
  remake::state().inline_decode = true;
  remake::state().system_theme = "snes";
  DetailedData d;
  d.name = "Super Mario World";
  d.year = 1994;
  d.genre = "Platform";
  d.players = "1-2";
  d.rating = 0.7f;
  d.desc = "A short description of the game.";
  d.desc_time = -1;
  RecentRow none;
  none.decode_inline = true;
  InfoFade fade;
  fade.item = 0;
  std::vector<Op> ops;
  const auto frame = [&] {
    ops.clear();
    if (what == 2) system_view(cx.th, cx.assets, common, cx.w, cx.h, fx.items, 0, 0.0f, cx.status, &none, fade, ops);
    else list_view(cx.th, cx.assets, common, cx.w, cx.h, "Super Nintendo", fx.mixed, 1, cx.status, what == 1 ? &d : nullptr, nullptr, ops);
  };
  for (int i = 0; i < 3; ++i) frame();
  const size_t before = g_allocs;
  for (int i = 0; i < 30; ++i) frame();
  return g_allocs - before;
}

void save(const Canvas& cv, const std::string& f) {
  std::string err;
  CHECK(save_png(cv, f, err));
}

void one_rig(const std::string& dir, const std::string& label, int w, int h, const std::string& tmp, const char* png_dir) {
  Ctx& cx = *make_ctx(dir, w, h, tmp);
  const Fixture fx;
  const std::string size = std::to_string(w) + "x" + std::to_string(h);
  const std::string name = label + "-" + size;
  const bool png = png_dir && w == 640 && h == 480;
  const bool industrial = label.find("industrial") != std::string::npos;
  std::map<std::string, std::string> tbl;

  // ---- the five selector styles: menu (left of the page, where the marker is), dialog, game list, carousel
  uint64_t hs[5] = {}, dl[5] = {}, ls[5] = {}, cs[5] = {};
  uint64_t hdr0 = 0;
  for (int k = 0; k < 5; ++k) {
    set_look(cx, tmp, Look{kStyles[k], "", ""});
    auto s = open_appearance(cx);
    if (!s) return;
    const Shot sh = shoot(*s, cx);
    hs[k] = fnv(sh.cv, 0, 0, uint32_t(w * 2 / 5), uint32_t(h));
    if (k == 0) {
      hdr0 = fnv(sh.cv, 0, 0, uint32_t(w), uint32_t(h / 5));
      check_default(name + "-menu", fnv(sh.cv));
    }
    remake::Box lb{};
    const Menu& m = static_cast<const Menu&>(*s);
    const bool have = m.cursor() >= 0 && label_box(sh.boxes, m.entry(size_t(m.cursor())).label, lb);
    CHECK(have);
    if (have) {
      const double c = box_contrast(sh.cv, lb);
      if (g_report) std::printf("contraste %-26s %-9s %.2f\n", name.c_str(), name_of(k), c);
      if (k < 2 && c < kMinDefault) std::printf("MAL %s %s: contraste del texto con foco del menu %.2f (minimo %.1f con el aspecto del tema)\n", name.c_str(), name_of(k), c, kMinDefault);
      if (k < 2) CHECK(c >= kMinDefault);
      if (k >= 2 && c < 4.5) std::printf("MAL %s %s: contraste del texto con foco %.2f (minimo 4.5)\n", name.c_str(), name_of(k), c);
      if (k >= 2) CHECK(c >= 4.5);
    }
    if (png) save(sh.cv, std::string(png_dir) + "/" + label + "-sel-" + name_of(k) + ".png");

    double dc = 0;
    Shot dsh;
    dl[k] = dialog_hash(cx, png ? &dsh : nullptr, &dc);
    if (k == 0) check_default(name + "-dialog", dl[k]);
    if (g_report) std::printf("contraste-dialogo %-26s %-9s %.2f\n", name.c_str(), name_of(k), dc);
    if (k < 2 && dc >= 0 && dc < kMinDefault) std::printf("MAL %s %s: contraste de la opcion con foco del dialogo %.2f (minimo %.1f con el aspecto del tema)\n", name.c_str(), name_of(k), dc, kMinDefault);
    if (k < 2) CHECK(dc >= kMinDefault);
    if (k >= 2 && dc >= 0 && dc < 4.5) std::printf("MAL %s %s: contraste de la opcion con foco del dialogo %.2f (minimo 4.5)\n", name.c_str(), name_of(k), dc);
    if (k >= 2 && dc >= 0) CHECK(dc >= 4.5);
    if (png && k >= 2) save(dsh.cv, std::string(png_dir) + "/extra/" + label + "-dialog-" + name_of(k) + ".png");

    Canvas lcv;
    ls[k] = list_hash(cx, fx, fx.mixed, false, png ? &lcv : nullptr);
    if (k < 2) {
      double lc = -1;
      list_hash(cx, fx, fx.mixed, false, nullptr, "Super Nintendo", &lc);
      if (g_report) std::printf("contraste-lista %-26s %-9s %.2f\n", name.c_str(), name_of(k), lc);
      if (lc < kMinDefault) std::printf("MAL %s %s: contraste de la fila con foco de la lista %.2f (minimo %.1f con el aspecto del tema)\n", name.c_str(), name_of(k), lc, kMinDefault);
      CHECK(lc >= kMinDefault);
    }
    if (k == 0) {
      check_default(name + "-list", ls[k]);
      check_default(name + "-listdetail", list_hash(cx, fx, fx.mixed, true));
    }
    if (png && (k >= 2 || k == 0)) save(lcv, std::string(png_dir) + "/extra/" + label + "-list-" + name_of(k) + ".png");
    cs[k] = carousel_hash(cx, fx);
    if (k == 0) check_default(name + "-carousel", cs[k]);
  }
  CHECK(hs[1] == hs[0]);  // fill is the theme's own focus
  CHECK(dl[1] == dl[0]);
  for (int a = 0; a < 5; ++a)
    for (int b = a + 1; b < 5; ++b) {
      const bool same = hs[a] == hs[b];
      if (same != (a == 0 && b == 1)) std::printf("MAL %s: menu, estilos '%s' y '%s' %s\n", name.c_str(), kStyles[a], kStyles[b], same ? "iguales" : "distintos");
      CHECK(same == (a == 0 && b == 1));
    }
  for (int a = 0; a < 5; ++a)
    for (int b = a + 1; b < 5; ++b) {
      const bool same = dl[a] == dl[b];
      if (same != (a == 0 && b == 1)) std::printf("MAL %s: dialogo, estilos '%s' y '%s' %s\n", name.c_str(), kStyles[a], kStyles[b], same ? "iguales" : "distintos");
      CHECK(same == (a == 0 && b == 1));
    }
  for (int k = 2; k < 5; ++k) {
    tbl[std::string("menu-") + kStyles[k]] = cell(hs[0], hs[k]);
    tbl[std::string("dialogo-") + kStyles[k]] = cell(dl[0], dl[k]);
    tbl[std::string("lista-") + kStyles[k]] = cell(ls[0], ls[k]);
    tbl[std::string("carrusel-") + kStyles[k]] = cell(cs[0], cs[k]);
  }

  // ---- the title centred: menu and dialog
  set_look(cx, tmp, Look{"", "center", ""});
  {
    auto s = open_appearance(cx);
    if (!s) return;
    const Shot sh = shoot(*s, cx);
    tbl["menu-titulo"] = cell(fnv(sh.cv, 0, 0, uint32_t(w), uint32_t(h / 5)), hdr0);
    CHECK(fnv(sh.cv, 0, 0, uint32_t(w), uint32_t(h / 5)) != hdr0);  // the header changes
    float c = -1;
    if (industrial) {
      for (const remake::Box& b : sh.boxes)
        if (b.tag == "titulo-barra") c = b.x + b.w / 2.0f;
    } else if (sh.hb.valid) {
      c = (sh.hb.title_x0 + sh.hb.title_x1) / 2.0f;
    }
    if (std::fabs(c - float(w) / 2.0f) > 1.5f) std::printf("MAL %s: titulo centrado en %.1f, la pagina en %.1f\n", name.c_str(), double(c), double(w) / 2.0);
    CHECK(std::fabs(c - float(w) / 2.0f) <= 1.5f);
    if (png) save(sh.cv, std::string(png_dir) + "/" + label + "-center.png");
  }
  tbl["dialogo-titulo"] = cell(dialog_hash(cx), dl[0]);
  CHECK(dialog_hash(cx) != dl[0]);

  // ---- live: a choice made with the menu open is drawn at once, as a menu opened after it
  for (const char* sel : {"outline", "underline", "bar"}) {
    set_look(cx, tmp, Look{});
    auto live = open_appearance(cx);
    if (!live) return;
    const Shot before = shoot(*live, cx);
    boot::set_user_theme(cx.th, "zfront.sel_style", sel);
    ui::relayout_all();
    const Shot after = shoot(*live, cx);
    CHECK(fnv(after.cv) != fnv(before.cv));
    set_look(cx, tmp, Look{sel, "", ""});
    auto fresh = open_appearance(cx);
    const Shot ref = shoot(*fresh, cx);
    const bool same = fnv(after.cv, 0, 0, uint32_t(w * 2 / 5), uint32_t(h)) == fnv(ref.cv, 0, 0, uint32_t(w * 2 / 5), uint32_t(h));
    if (!same) std::printf("MAL %s: '%s' en vivo no se dibuja como al abrir el menu despues\n", name.c_str(), sel);
    CHECK(same);
  }
  {  // and the title, live
    set_look(cx, tmp, Look{});
    auto live = open_appearance(cx);
    if (!live) return;
    const Shot before = shoot(*live, cx);
    boot::set_user_theme(cx.th, "zfront.header_align", "center");
    ui::relayout_all();
    const Shot after = shoot(*live, cx);
    CHECK(fnv(after.cv) != fnv(before.cv));
    set_look(cx, tmp, Look{"", "center", ""});
    auto fresh = open_appearance(cx);
    const Shot ref = shoot(*fresh, cx);
    const bool same = fnv(after.cv, 0, 0, uint32_t(w * 7 / 10), uint32_t(h / 5)) == fnv(ref.cv, 0, 0, uint32_t(w * 7 / 10), uint32_t(h / 5));
    if (!same) std::printf("MAL %s: el titulo centrado en vivo no se dibuja como al abrir el menu despues\n", name.c_str());
    CHECK(same);
  }

  // ---- warm frames allocate nothing (the rule of the project: zero allocations per frame at rest), with the theme's own look and with every selector
  // and with the title centred; the game list and the system carousel too
  for (const Look& l : {Look{}, Look{"outline", "", ""}, Look{"underline", "center", ""}, Look{"bar", "", ""}, Look{"fill", "center", ""}}) {
    set_look(cx, tmp, l);
    auto s = open_appearance(cx);
    if (!s) return;
    std::vector<Op> ops;
    for (int i = 0; i < 3; ++i) {
      s->tick(1.0f, cx);
      ops.clear();
      s->draw(cx, ops);
    }
    const size_t before = g_allocs;
    for (int i = 0; i < 30; ++i) {
      ops.clear();
      s->draw(cx, ops);
    }
    const size_t used = g_allocs - before;
    if (g_report) std::printf("reservas-menu %s '%s'/'%s': %zu\n", name.c_str(), l.sel.c_str(), l.align.c_str(), used);
    if (used) std::printf("MAL %s '%s'/'%s': %zu reservas en 30 fotogramas del menu en reposo (debe ser 0)\n", name.c_str(), l.sel.c_str(), l.align.c_str(), used);
    CHECK(used == 0);
  }
  set_look(cx, tmp, Look{});
  for (int what = 0; g_report && what < 3; ++what) {
    const size_t used = scene_allocs(cx, fx, what);
    const char* const nm[] = {"lista", "lista-detalle", "carrusel"};
    if (g_report) std::printf("reservas-%s %s: %zu\n", nm[what], name.c_str(), used);  // report only: the list and the carousel are rebuilt on each draw in every theme
  }

  // ---- LIST TEXT CASE in the game list
  {
    std::vector<std::string> low, up;
    for (const std::string& n : fx.mixed) low.push_back(utf8_lower(n)), up.push_back(utf8_upper(n));
    set_look(cx, tmp, Look{});
    const uint64_t base = list_hash(cx, fx, fx.mixed, false);
    set_look(cx, tmp, Look{"", "", "lower"});
    const uint64_t lo = list_hash(cx, fx, fx.mixed, false);
    set_look(cx, tmp, Look{"", "", "upper"});
    const uint64_t upr = list_hash(cx, fx, fx.mixed, false);
    set_look(cx, tmp, Look{});
    const uint64_t lo_ref = list_hash(cx, fx, low, false, nullptr, utf8_lower("Super Nintendo")), up_ref = list_hash(cx, fx, up, false, nullptr, utf8_upper("Super Nintendo"));
    tbl["lista-minusculas"] = cell(base, lo);
    tbl["lista-mayusculas"] = cell(base, upr);
    if (base == lo) std::printf("MAL %s: LIST TEXT CASE lower no cambia la lista\n", name.c_str());
    CHECK(base != lo);
    CHECK(lo != upr);
    // the plain Zenith list has its rows in capitals by default: `lower` is checked against `upper` and the default, not against names written by hand
    const bool rows_upper_by_default = !remake::active(cx.th);
    tbl["lista-por-defecto"] = rows_upper_by_default ? "MAYUSCULAS" : "como-se-llama";
    if (rows_upper_by_default) {  // upper still changes the name in the right column
      if (upr == base) std::printf("MAL %s: LIST TEXT CASE upper no cambia la columna del juego\n", name.c_str());
      CHECK(upr != base);
    } else {
      // the rows (and the title) written in that case: the same pixels; the selected name on the right follows the same rule
      if (lo != lo_ref) std::printf("MAL %s: LIST TEXT CASE lower no baja las filas\n", name.c_str());
      if (upr != up_ref) std::printf("MAL %s: LIST TEXT CASE upper no sube las filas\n", name.c_str());
      CHECK(lo == lo_ref);
      CHECK(upr == up_ref);
    }
    set_look(cx, tmp, Look{"", "", "lower"});
    const uint64_t lo_d = list_hash(cx, fx, fx.mixed, true);
    set_look(cx, tmp, Look{});
    tbl["lista-detalle-minusculas"] = cell(list_hash(cx, fx, fx.mixed, true), lo_d);
  }
  set_look(cx, tmp, Look{});

  if (g_report) {
    std::printf("TABLA %-26s", name.c_str());
    for (const auto& [k, v] : tbl) std::printf(" %s=%s", k.c_str(), v.c_str());
    std::printf("\n");
  }
}

}  // namespace

int main() {
  const std::string light = find_base("theme-light"), dark = find_base("theme-dark");
  if (light.empty() || dark.empty()) {
    std::printf("sin carpetas del tema: se omite\n");
    return zt::done();
  }
  g_write = std::getenv("ZF_THEME_SEL_WRITE") != nullptr;
  g_report = std::getenv("ZF_THEME_SEL_REPORT") != nullptr;
  const std::string ref_path = std::string(ZF_TEST_DATA) + "/theme_default.hash";
  if (!g_write) {
    std::ifstream in(ref_path);
    std::string k, v;
    while (in >> k >> v) g_ref[k] = v;
    CHECK(!g_ref.empty());
  }
  const char* png_dir = std::getenv("ZF_THEME_SEL_PNG");
  if (png_dir) fs::create_directories(std::string(png_dir) + "/extra");
  char tmpl[] = "/tmp/zf-test-themesel-XXXXXX";
  const std::string root = mkdtemp(tmpl);
  i18n::set_language("", ui_language_options());
  const int sizes[][2] = {{480, 320}, {640, 480}, {720, 720}};
  const char* only = std::getenv("ZF_THEME_SEL_ONLY");  // one rig, to look at it
  const auto wanted = [&](const std::string& l) { return !only || l == only; };

  for (const int dk : {0, 1}) {  // Zenith (Minimal): the folders of the package, light and dark
    const std::string label = std::string("zenith-") + (dk ? "dark" : "light");
    if (!wanted(label)) continue;
    for (const auto& sz : sizes) {
      ui::set_access(ui::TextSize::Normal, false);
      one_rig(dk ? dark : light, label, sz[0], sz[1], root, png_dir);
    }
  }
  {  // Zenith Next: the same folders with zenStyle = next, as the package makes them
    const std::string next = root + "/next";
    for (const int dk : {0, 1}) {
      const std::string label = std::string("next-") + (dk ? "dark" : "light");
      const std::string to = next + "/" + (dk ? "es-theme-zenith-next-dark" : "es-theme-zenith-next");
      fs::create_directories(next);
      fs::copy(fs::canonical(dk ? dark : light), to, fs::copy_options::recursive);
      std::ifstream in(to + "/theme.xml");
      std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const size_t at = xml.find("<variables>");
      CHECK(at != std::string::npos);
      if (at == std::string::npos) continue;
      xml.insert(at + 11, "<zenStyle>next</zenStyle>");
      std::ofstream(to + "/theme.xml") << xml;
      if (!wanted(label)) continue;
      for (const auto& sz : sizes) {
        ui::set_access(ui::TextSize::Normal, false);
        one_rig(to, label, sz[0], sz[1], root, png_dir);
      }
    }
  }
  const struct {
    const char* folder;
    const char* scheme;
    const char* carousel;
    const char* label;
  } cases[] = {{"es-theme-xmb-fcamod", "", "", "xmb"},      {"es-theme-art-book-next", "", "", "artbook"},    {"es-theme-iconic", "", "", "iconic-light"},
               {"es-theme-iconic", "dark", "", "iconic-dark"}, {"es-theme-techdweeb", "", "", "techdweeb"},   {"es-theme-iconic", "", "aurora", "aurora-light"},
               {"es-theme-iconic", "dark", "aurora", "aurora-dark"}, {"es-theme-iconic", "", "swiss", "swiss-light"}, {"es-theme-iconic", "dark", "swiss", "swiss-dark"},
               {"es-theme-iconic", "", "industrial", "industrial-light"}, {"es-theme-iconic", "dark", "industrial", "industrial-dark"}};
  for (const auto& cs : cases) {
    if (!wanted(cs.label)) continue;
    std::string es = root + "/" + cs.folder;
    const char* real = std::getenv("ZF_THEME_SEL_REAL");  // the real ES themes (their artwork) for the contact sheets; the hashes are of the fixtures
    if (real && fs::exists(std::string(real) + "/" + cs.folder + "/theme.xml")) es = std::string(real) + "/" + cs.folder;
    else fs::create_directories(es);
    if (es.rfind(root, 0) == 0) std::ofstream(es + "/theme.xml") << "<theme><view name=\"system\"><carousel name=\"systemcarousel\"><type>horizontal</type></carousel></view>"
                                        "<subset name=\"color-scheme\"><include name=\"light\"/><include name=\"dark\"/></subset></theme>";
    const std::string choice = cs.scheme;
    estheme::Options opt;
    opt.carousel = cs.carousel;
    if (!choice.empty()) opt.setting = [&choice](const std::string& k) -> const std::string* { return k == "subset.color-scheme" ? &choice : nullptr; };
    estheme::Generated gen;
    std::string err;
    CHECK(estheme::generate(es, estheme::Bases{light, dark}, root + "/gen", opt, gen, err));
    for (const auto& sz : sizes) {
      ui::set_access(ui::TextSize::Normal, false);
      one_rig(gen.dir, cs.label, sz[0], sz[1], root, png_dir);
    }
  }
  if (g_write) {
    std::ofstream out(ref_path);
    for (const auto& [k, v] : g_new) out << k << " " << v << "\n";
    std::printf("escrito %s (%zu)\n", ref_path.c_str(), g_new.size());
  }
  std::error_code ec;
  fs::remove_all(root, ec);
  return zt::done();
}
