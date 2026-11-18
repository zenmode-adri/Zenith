// The three skinned menus (Aurora, Swiss, Industrial) obey the user's own look of APPEARANCE: SELECTOR STYLE, TITLE ALIGNMENT and, in the game list,
// LIST TEXT CASE. Measured on the pixels the software renderer draws (no eyes):
//  - with "Theme default" every skin, in light and dark, is drawn exactly as before the change (a hash of each picture in tests/data/skin_default.hash,
//    made BEFORE the skins read the choices; ZF_SKIN_SEL_WRITE=1 writes the file again);
//  - `fill` is the skins' own focus (a filled box), `outline`, `underline` and `bar` change the focused row, each one differently;
//  - the label of the focused row keeps its contrast (3:1, large text) in the five states, light and dark;
//  - a choice made while the menu is open is drawn at once (same pixels as a menu opened after the choice);
//  - `center` puts the title in the middle of the page and the picture changes only in the header;
//  - LIST TEXT CASE forces the case of the rows of the skins' game list;
//  - warm frames allocate nothing with every selector.
// ZF_SKIN_SEL_PNG=<folder> saves the pictures of every skin x mode x style at 640x480 (for the contact sheets). Needs theme-light and theme-dark
// (ZF_THEME_DIR).
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

Ctx* make_ctx(const std::string& gen_dir, int w, int h, const std::string& tmp) {
  static std::vector<std::unique_ptr<Ctx>> keep;  // every Ctx stays alive: the caches of the shapes are keyed on the address of the Assets
  keep.push_back(std::make_unique<Ctx>());
  Ctx& cx = *keep.back();
  std::string err;
  CHECK(Theme::load(gen_dir, ratio_name(unsigned(w), unsigned(h)), cx.th, err));
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
  r.boxes.clear();
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

std::map<std::string, std::string> g_ref;  // name -> hash of the default look, from tests/data/skin_default.hash
std::map<std::string, std::string> g_new;
bool g_write = false;

const char* const kStyles[] = {"", "fill", "outline", "underline", "bar"};

/// The label box of the focused row of a menu.
bool focus_label(const Screen& s, const std::vector<remake::Box>& boxes, remake::Box& out) {
  const Menu& m = static_cast<const Menu&>(s);
  if (m.cursor() < 0) return false;
  const std::string tag = "\"" + m.entry(size_t(m.cursor())).label.substr(0, 18) + "\"";
  for (const remake::Box& b : boxes)
    if (b.tag == tag) {
      out = b;
      return true;
    }
  return false;
}

void one_rig(const std::string& gen_dir, const std::string& label, int w, int h, const std::string& tmp, const char* png_dir) {
  Ctx& cx = *make_ctx(gen_dir, w, h, tmp);
  const std::string size = std::to_string(w) + "x" + std::to_string(h);
  const std::string name = label + "-" + size;
  const bool industrial = label.find("industrial") != std::string::npos;

  // ---- the five selector styles, title on the left
  uint64_t hs[5] = {};  // the left of the page: the value of the row ("Outline") is on the right and changes with the style
  uint64_t hdr0 = 0;
  for (int k = 0; k < 5; ++k) {
    set_look(cx, tmp, Look{kStyles[k], "", ""});
    auto s = open_appearance(cx);
    if (!s) return;
    const Shot sh = shoot(*s, cx);
    hs[k] = fnv(sh.cv, 0, 0, uint32_t(w * 2 / 5), uint32_t(h));
    const uint64_t whole = fnv(sh.cv);
    if (k == 0) hdr0 = fnv(sh.cv, 0, 0, uint32_t(w), uint32_t(h / 5));
    remake::Box lb{};
    const bool have = focus_label(*s, sh.boxes, lb);
    CHECK(have);
    if (have) {
      // the project's floor for text is 4.5:1 (es_theme_map, remake::legible_on). The skins' own filled focus is kept as it was approved (white on the
      // orange of Industrial is 3.1:1): 3:1 there; the new shapes draw the text of a row at rest, so they must reach 4.5:1
      const double c = box_contrast(sh.cv, lb), need = k >= 2 ? 4.5 : 3.0;
      if (std::getenv("ZF_SKIN_SEL_REPORT")) std::printf("contraste %-22s %-9s %.2f\n", name.c_str(), kStyles[k][0] ? kStyles[k] : "default", c);
      if (c < need) std::printf("MAL %s %s: contraste del texto con foco %.2f (minimo %.1f)\n", name.c_str(), kStyles[k][0] ? kStyles[k] : "default", c, need);
      CHECK(c >= need);
    }
    if (png_dir && w == 640 && h == 480) {
      std::string err;
      const std::string f = std::string(png_dir) + "/" + label + "-sel-" + (kStyles[k][0] ? kStyles[k] : "default") + ".png";
      CHECK(save_png(sh.cv, f, err));
    }
    if (k == 0) {
      if (g_write) g_new[name] = hex(whole);
      else {
        const auto it = g_ref.find(name);
        CHECK(it != g_ref.end());
        if (it != g_ref.end() && it->second != hex(whole)) std::printf("MAL %s: con Theme default ya no se dibuja como antes (%s != %s)\n", name.c_str(), hex(whole).c_str(), it->second.c_str());
        if (it != g_ref.end()) CHECK(it->second == hex(whole));
      }
    }
  }
  CHECK(hs[1] == hs[0]);  // fill is what the skins already have: a filled box in their focus colour
  for (int a = 0; a < 5; ++a)
    for (int b = a + 1; b < 5; ++b) {
      const bool same = hs[a] == hs[b];
      if (same != (a == 0 && b == 1)) std::printf("MAL %s: estilos '%s' y '%s' %s\n", name.c_str(), kStyles[a], kStyles[b], same ? "iguales" : "distintos");
      CHECK(same == (a == 0 && b == 1));
    }

  // ---- the title centred
  set_look(cx, tmp, Look{"", "center", ""});
  {
    auto s = open_appearance(cx);
    if (!s) return;
    const Shot sh = shoot(*s, cx);
    CHECK(fnv(sh.cv, 0, 0, uint32_t(w), uint32_t(h / 5)) != hdr0);  // the header changes
    float c = -1;
    if (industrial) {  // the title is on the bar of the panel
      for (const remake::Box& b : sh.boxes)
        if (b.tag == "titulo-barra") c = b.x + b.w / 2.0f;
    } else if (sh.hb.valid) {
      c = (sh.hb.title_x0 + sh.hb.title_x1) / 2.0f;
    }
    if (std::fabs(c - float(w) / 2.0f) > 1.5f) std::printf("MAL %s: titulo centrado en %.1f, la pagina en %.1f\n", name.c_str(), double(c), double(w) / 2.0);
    CHECK(std::fabs(c - float(w) / 2.0f) <= 1.5f);
    if (png_dir && w == 640 && h == 480) {
      std::string err;
      CHECK(save_png(sh.cv, std::string(png_dir) + "/" + label + "-center.png", err));
    }
  }

  // ---- live: a choice made with the menu open is drawn at once, as a menu opened after it
  set_look(cx, tmp, Look{});
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
    // the value of the row ("Outline") is read when the menu is made: the pictures are compared on the left of the page, where the marker is
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
    // the count of Industrial ("02 / 21") is one frame late by design of that skin and sits on the right: the pictures are compared on the left 70 %
    const bool same = fnv(after.cv, 0, 0, uint32_t(w * 7 / 10), uint32_t(h / 5)) == fnv(ref.cv, 0, 0, uint32_t(w * 7 / 10), uint32_t(h / 5));
    if (!same) std::printf("MAL %s: el titulo centrado en vivo no se dibuja como al abrir el menu despues\n", name.c_str());
    CHECK(same);
  }

  // ---- warm frames allocate nothing, with every selector and with the title centred
  for (const Look& l : {Look{"outline", "", ""}, Look{"underline", "center", ""}, Look{"bar", "", ""}, Look{"fill", "center", ""}}) {
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
    if (g_allocs != before) std::printf("MAL %s '%s'/'%s': %zu reservas en 30 fotogramas\n", name.c_str(), l.sel.c_str(), l.align.c_str(), g_allocs - before);
    CHECK(g_allocs == before);
  }
  set_look(cx, tmp, Look{});

  // ---- LIST TEXT CASE in the game list of the skin
  if (w == 640 && h == 480) {
    remake::state().inline_decode = true;
    remake::state().system_theme = "snes";
    const std::vector<std::string> mixed = {"Super Mario World", "zelda: A Link", "Final Fantasy"};
    std::vector<std::string> low, up;
    for (const std::string& n : mixed) low.push_back(utf8_lower(n)), up.push_back(utf8_upper(n));
    auto list = [&](const std::string& lcase, const std::vector<std::string>& names, bool detail) {
      set_look(cx, tmp, Look{"", "", lcase});
      std::vector<Op> ops;
      DetailedData d;
      d.name = "Super Mario World";
      remake::list_view(cx.th, cx.assets, cx.w, cx.h, "Super Nintendo", names, 0, cx.status, detail ? &d : nullptr, ops);
      return fnv(cpu_render(ops, cx.assets, uint32_t(w), uint32_t(h)));
    };
    for (const bool detail : {false, true}) {
      const uint64_t base = list("", mixed, detail), lo = list("lower", mixed, detail), upr = list("upper", mixed, detail);
      CHECK(base != lo);
      CHECK(base != upr);
      CHECK(lo != upr);
      const uint64_t lo_ref = list("", low, detail), up_ref = list("", up, detail);
      if (!detail) {  // the rows: the same pixels as the names written in that case
        if (lo != lo_ref) std::printf("MAL %s: LIST TEXT CASE lower no baja las filas\n", name.c_str());
        if (upr != up_ref) std::printf("MAL %s: LIST TEXT CASE upper no sube las filas\n", name.c_str());
        CHECK(lo == lo_ref);
        CHECK(upr == up_ref);
      }
    }
    set_look(cx, tmp, Look{});
  }
}

}  // namespace

int main() {
  const std::string light = find_base("theme-light"), dark = find_base("theme-dark");
  if (light.empty() || dark.empty()) {
    std::printf("sin carpetas del tema: se omite\n");
    return zt::done();
  }
  g_write = std::getenv("ZF_SKIN_SEL_WRITE") != nullptr;
  const std::string ref_path = std::string(ZF_TEST_DATA) + "/skin_default.hash";
  if (!g_write) {
    std::ifstream in(ref_path);
    std::string k, v;
    while (in >> k >> v) g_ref[k] = v;
    CHECK(!g_ref.empty());
  }
  const char* png_dir = std::getenv("ZF_SKIN_SEL_PNG");
  char tmpl[] = "/tmp/zf-test-skinsel-XXXXXX";
  const std::string root = mkdtemp(tmpl);
  i18n::set_language("", ui_language_options());
  const int sizes[][2] = {{480, 320}, {640, 480}, {720, 720}};
  const struct {
    const char* scheme;
    const char* carousel;
  } cases[] = {{"", "aurora"}, {"dark", "aurora"}, {"", "swiss"}, {"dark", "swiss"}, {"", "industrial"}, {"dark", "industrial"}};
  for (const auto& cs : cases) {
    const std::string label = std::string(cs.carousel) + "-" + (*cs.scheme ? "dark" : "light");
    const std::string es = root + "/es-theme-iconic";
    fs::create_directories(es);
    std::ofstream(es + "/theme.xml") << "<theme><view name=\"system\"><carousel name=\"systemcarousel\"><type>horizontal</type></carousel></view>"
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
      one_rig(gen.dir, label, sz[0], sz[1], root, png_dir);
    }
  }
  if (g_write) {
    std::ofstream out(ref_path);
    for (const auto& [k, v] : g_new) out << k << " " << v << "\n";
    std::printf("escrito %s (%zu)\n", ref_path.c_str(), g_new.size());
  }
  return zt::done();
}
