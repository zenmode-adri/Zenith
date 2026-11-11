// Measures the four remade themes instead of looking at them: every text, picture, status bar and help row of their views, menus and
// battery screens reports its box, and here
// those boxes must keep to the margins, start on the columns of the grid and not touch each other, on every screen size of the consoles.
// Zenith Next always; ZF_LAYOUT_ZENITH=<folder>: also Zenith Minimal (es-theme-zenith, -dark) by the same rules, counted apart. ZF_LAYOUT_REPORT=1 prints the table of measured margins. ZF_ES_THEMES: folder of the real ES themes (otherwise, empty ones).
// ZF_LAYOUT_ACCESS=0 leaves out the LARGE and EXTRA LARGE + high contrast passes (480x320, 640x480, 720x720).
// ZF_LAYOUT_LANG=es with ZF_LOCALE_DIR: measures in that language, longer than English. ZF_LAYOUT_PNG=<dir> and ZF_LAYOUT_PNG_SIZE=WxH: saves each screen with the grid over it.
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
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "cpu.hpp"
#include "es_theme.hpp"
#include "gfx.hpp"
#include "i18n.hpp"
#include "oscontract.hpp"
#include "playstats.hpp"
#include "registry.hpp"
#include "resume_last.hpp"
#include "remake.hpp"
#include "scene.hpp"
#include "status.hpp"
#include "theme.hpp"
#include "ui_dialogs.hpp"
#include "menu_ui.hpp"
#include "ui_prefs.hpp"
#include "ui_style.hpp"
#include "util.hpp"
#include "webpair.hpp"
#include "webtransfer.hpp"

using namespace zf;
namespace fs = std::filesystem;

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

int g_bad = 0;
int g_zenith = 0;
bool g_report_only = false;  // Zenith Minimal is not on the grid: its failures are counted apart and do not fail the test
std::map<std::string, int> g_seen;  // what was measured, by kind: a rule nothing goes through proves nothing
void bad(const char* theme, int w, int h, const char* screen, const std::string& what) {
  ++(g_report_only ? g_zenith : g_bad);
  std::printf("%s %-22s %4dx%-4d %-10s %s\n", g_report_only ? "ZEN" : "MAL", theme, w, h, screen, what.c_str());
}

bool on_column(const remake::Grid& g, float x) {
  for (int i = 0; i <= 12; ++i)
    if (std::fabs(x - g.col(i)) <= 1.5f) return true;
  return false;
}
bool on_column_end(const remake::Grid& g, float r) {
  for (int i = 0; i < 12; ++i)
    if (std::fabs(r - (g.col(i) + g.colw())) <= 1.5f) return true;
  return false;
}

/// On a line of the cells: `v` px from the grid's origin `o` is a whole number of cells (one pixel of rounding allowed).
bool on_cells(const remake::Grid& g, float v, float o) {
  const float d = std::fmod(std::fabs(v - o), float(g.u));
  return d <= 1.0f || float(g.u) - d <= 1.0f;
}

/// Whole cells from the start or the end of a column (or from the margin): where an inset inside a column lands.
bool on_column_cells(const remake::Grid& g, float v) {
  if (on_cells(g, v, float(g.margin()))) return true;
  for (int i = 0; i < 12; ++i)
    if (on_cells(g, v, g.col(i)) || on_cells(g, v, g.col(i) + g.colw())) return true;
  return false;
}

/// The rules. `info` boxes are the ones of this screen.
void check_screen(const char* theme, int w, int h, const char* screen, const remake::Grid& g, const std::vector<remake::Box>& boxes, bool report) {
  const float m = float(g.margin());
  const float mt = float(g.top());
  float l = 1e9f, r = 1e9f, t = 1e9f, b = 1e9f;
  for (const remake::Box& q : boxes) {
    ++g_seen[q.tag.substr(0, q.tag.find(' '))[0] == '"' ? std::string("texto") : q.tag.substr(0, q.tag.find(' '))];
    l = std::min(l, q.x);
    r = std::min(r, float(w) - q.x - q.w);
    t = std::min(t, q.y);
    b = std::min(b, float(h) - q.y - q.h);
    if (q.x < m - 1.0f || q.x + q.w > float(w) - m + 1.0f)
      bad(theme, w, h, screen, "fuera de margen lateral (" + std::to_string(int(m)) + "): " + q.tag + " x=" + std::to_string(int(q.x)) + ".." + std::to_string(int(q.x + q.w)));
    if (q.y < mt - 1.0f || q.y + q.h > float(h) - mt + 1.0f)
      bad(theme, w, h, screen, "fuera de margen vertical (" + std::to_string(int(mt)) + "): " + q.tag + " y=" + std::to_string(int(q.y)) + ".." + std::to_string(int(q.y + q.h)));
    if (q.grid && (q.anchor == 'L' || q.anchor == 'B') && !on_column(g, q.x)) bad(theme, w, h, screen, "fuera de columna: " + q.tag + " x=" + std::to_string(int(q.x)));
    if (q.grid && (q.anchor == 'R' || q.anchor == 'B') && !on_column_end(g, q.x + q.w)) bad(theme, w, h, screen, "fin fuera de columna: " + q.tag + " r=" + std::to_string(int(q.x + q.w)));
    // the cells: every left edge on a vertical line, every right end too; a container on the lines in both directions
    if ((q.anchor == 'L' || q.anchor == 'B') && !on_column_cells(g, q.x)) bad(theme, w, h, screen, "fuera de rejilla: " + q.tag + " x=" + std::to_string(int(q.x)));
    if ((q.anchor == 'R' || q.anchor == 'B') && !on_column_cells(g, q.x + q.w)) bad(theme, w, h, screen, "fin fuera de rejilla: " + q.tag + " r=" + std::to_string(int(q.x + q.w)));
    if (q.cell && (!on_cells(g, q.y, mt) || !on_cells(g, q.y + q.h, mt)))
      bad(theme, w, h, screen, "caja fuera de rejilla: " + q.tag + " y=" + std::to_string(int(q.y)) + ".." + std::to_string(int(q.y + q.h)));
  }
  for (size_t i = 0; i < boxes.size(); ++i)
    for (size_t j = i + 1; j < boxes.size(); ++j) {
      const remake::Box& a = boxes[i];
      const remake::Box& c = boxes[j];
      const float ox = std::min(a.x + a.w, c.x + c.w) - std::max(a.x, c.x), oy = std::min(a.y + a.h, c.y + c.h) - std::max(a.y, c.y);
      if (a.tag == "tarjeta" || c.tag == "tarjeta") continue;  // a card holds things: only its edges are measured
      if (ox > 1.0f && oy > 1.0f) bad(theme, w, h, screen, "se pisan: " + a.tag + " / " + c.tag + " (" + std::to_string(int(ox)) + "x" + std::to_string(int(oy)) + ")");
    }
  if (report) std::printf("    %-22s %4dx%-4d %-10s margen %3d | izq %4d der %4d arr %4d aba %4d\n", theme, w, h, screen, int(m), int(l), int(r), int(t), int(b));
}

void write(const std::string& p, const std::string& t) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream(p) << t;
}

/// A battery with a day of use behind it, so the battery screens draw everything they have.
void fake_battery(const std::string& root) {
  setenv("ZF_BATTERY_ROOT", root.c_str(), 1);
  setenv("ZF_BATTERY_LOG", (root + "/battery.log").c_str(), 1);
  const std::string b = root + "/sys/class/power_supply/battery/";
  write(b + "capacity", "78\n");
  write(b + "status", "Discharging\n");
  write(b + "voltage_avg", "3840000\n");
  write(b + "current_avg", "-810000\n");
  write(b + "charge_now", "2340000\n");
  write(b + "charge_full", "3000000\n");
  write(root + "/sys/class/backlight/x/brightness", "128\n");
  write(root + "/sys/class/backlight/x/max_brightness", "255\n");
  write(root + "/battery.log",
        "S\t1000\t1000\t2900000\t100\t50\t\n"
        "G\t2000\t2000\t2800000\t96\t50\tThe Legend of a very long name that does not fit\n"
        "E\t5600\t5600\t2200000\t76\t50\t\n"
        "G\t5700\t5700\t2190000\t75\t50\tSuper Metroid\n"
        "E\t6600\t6600\t2120000\t73\t50\t\n"
        "S\t7400\t7400\t2100000\t72\t50\t\n");
}

/// With ZF_LAYOUT_PNG=<dir> and ZF_LAYOUT_PNG_SIZE=WxH: the screen saved as a picture with the grid over it, to look at it too.
void save_shot(std::vector<Op> ops, Assets& a, const char* theme, int w, int h, const char* name, const remake::Grid& g) {
  const char* only = std::getenv("ZF_LAYOUT_PNG_SIZE");  // WxH: the pictures are heavy, only of one size
  const char* dir = std::getenv("ZF_LAYOUT_PNG");
  if (!dir || !only || std::to_string(w) + "x" + std::to_string(h) != only) return;
  const Color line{255, 0, 90, 120};
  for (int i = 0; i < 12; ++i) {
    ops.push_back(Op::rect(g.col(i), 0, 1, float(h), line));
    ops.push_back(Op::rect(g.col(i) + g.colw(), 0, 1, float(h), line));
  }
  for (const float y : {float(g.top()), float(g.content_top()), float(g.help_top()), float(g.bottom())}) ops.push_back(Op::rect(0, y, float(w), 1, line));
  std::string file = std::string(dir) + "/" + theme + "-" + std::to_string(w) + "x" + std::to_string(h) + "-" + name + ".png";
  std::string err;
  if (!save_png(cpu_render(ops, a, uint32_t(w), uint32_t(h)), file, err)) std::printf("png: %s\n", err.c_str());
}

/// One screen, as the app would draw it. The title comes from the header box every full screen publishes.
void measure_screen(Ctx& cx, Screen& s, const char* theme, int w, int h, const char* name, const remake::Grid& g, std::vector<remake::Box>& boxes, bool report) {
  boxes.clear();
  ui::header_box() = ui::HeaderBox{};
  std::vector<Op> ops;
  s.tick(1.0f, cx);
  s.draw(cx, ops);
  const ui::HeaderBox hb = ui::header_box();
  if (hb.valid) {
    const bool centred = ui::Style::from(cx.th).header_center;
    boxes.push_back(remake::Box{hb.title_x0, hb.title_y0, hb.title_x1 - hb.title_x0, hb.title_y1 - hb.title_y0, "titulo", centred ? 'C' : 'L', !centred});
  }
  check_screen(theme, w, h, name, g, boxes, report);
  save_shot(std::move(ops), cx.assets, theme, w, h, name, g);
}

/// The menus and the battery screens of a theme: at the top, in the middle and at the end of their lists, plus a list picker.
void measure_menus_once(const std::string& gen_dir, const char* theme, int w, int h, const std::string& tmp, std::vector<remake::Box>& boxes, bool report) {
  static std::vector<std::unique_ptr<Ctx>> keep;  // every Ctx stays alive: the caches of the shapes are keyed on the address of the Assets
  keep.push_back(std::make_unique<Ctx>());
  Ctx& cx = *keep.back();
  std::string err;
  CHECK(Theme::load(gen_dir, ratio_name(unsigned(w), unsigned(h)), cx.th, err));
  CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
  cx.w = float(w), cx.h = float(h);
  cx.sv->cmd.dry_run = true;
  write(tmp + "/system.cfg", "system.language=en_US\naudio.volume=40\n");
  write(tmp + "/es_settings.cfg", "");
  cx.conf = SystemConf::load(tmp + "/system.cfg");
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
  ui_prefs::on_status(cx, cx.status);
  const remake::Grid g{float(w), float(h)};
  setenv("ZF_PLAYLOG", (tmp + "/no-playlog.tsv").c_str(), 1);  // no log and no systems: the empty state of the statistics
  for (const char* name : {"menu.main", "menu.ui", "menu.quit", "menu.system", "menu.g.screen", "menu.g.look", "menu.g.panel", "menu.g.power", "menu.g.power.adv", "menu.g.library", "menu.g.system", "menu.g.system.adv", "menu.g.lang",
                           "menu.sound", "menu.network", "menu.network.wifi", "menu.network.manual", "menu.network.transfer", "menu.network.services", "menu.controllers", "menu.controllers.players", "menu.controllers.btpair", "menu.controllers.btdevices", "menu.controllers.detect",
                           "menu.collections", "menu.collections.systems", "menu.collections.auto", "menu.collections.custom", "menu.game", "menu.game.systems", "menu.game.ai", "menu.game.bios", "menu.library_doctor", "menu.game.latency",
                           "menu.retroachievements", "menu.scraper", "menu.scraper.systems", "menu.scraper.options", "menu.scraper.account", "menu.updates", "menu.netplay", "menu.netplay.lobby", "menu.moonlight", "menu.bezelproject", "menu.system.info", "menu.system_options", "menu.game_options",
                           "menu.savestates", "menu.savebackup", "menu.battery", "menu.battery.usage", "menu.game.stats", "menu.home"}) {
    auto s = Registry::instance().make(name, cx, "");
    if (!s) {  // needs something this test does not have (an argument, a device)
      ++g_seen[std::string("sin abrir ") + name];
      continue;
    }
    ++g_seen["pantallas"];
    int at = 0;
    for (int step : {0, 3, 7, 12}) {
      for (; at < step; ++at) s->handle(Action::Down, cx);
      char tag[48];
      std::snprintf(tag, sizeof tag, "%s@%d", name, step);
      measure_screen(cx, *s, theme, w, h, tag, g, boxes, report);
      if (step == 0) {
      static std::map<std::string, int> dumped;
      if (std::getenv("ZF_LAYOUT_DUMP") && s->is_menu() && w == 720 && h == 720 && dumped[name]++ == 0) {  // the rows of every menu, to study them
        static const char* kinds[] = {"accion", "interruptor", "deslizador", "opcion", "texto", "info", "CABECERA", "separador"};
        const Menu& mm = static_cast<const Menu&>(*s);
        std::printf("DUMP == %s\n", name);
        for (size_t i = 0; i < mm.size(); ++i) {
          const Entry& e = mm.entry(i);
          std::printf("DUMP   %-11s %s%s%s\n", kinds[int(e.kind)], e.label.c_str(), e.value_text.empty() ? "" : "  = ", e.value_text.c_str());
        }
      }
      }
    }
  }
  {  // the statistics with something to show: a long list (scrolls), long names, the five weeks
    setenv("ZF_PLAYLOG", (tmp + "/playlog.tsv").c_str(), 1);
    std::string log;
    const time_t now = ::time(nullptr);
    for (int i = 0; i < 40; ++i) log += playstats::format_line(now - time_t(i) * 20 * 3600, 600 + uint64_t(i) * 97, "snes", "/roms/snes/a.sfc");
    for (int i = 0; i < 6; ++i)  // sessions with the battery they used: the BATTERY USE section of the statistics
      log += playstats::format_line(now - time_t(i) * 3600, 1800, "snes", "/roms/snes/" + std::to_string(i) + ".rom", 40 + i * 30);
    write(tmp + "/playlog.tsv", log);
    const char* sys[][2] = {{"snes", "Super Nintendo"}, {"psp", "PlayStation Portable"}, {"gba", "Game Boy Advance and a very long system name"}};
    for (const auto& sy : sys) {
      System s;
      s.name = sy[0], s.fullname = sy[1], s.loaded = true;
      for (int i = 0; i < 6; ++i) {
        Game gm;
        gm.name = i == 0 ? "The Legend of a very long name that does not fit in any row of any screen at all" : "Game number " + std::to_string(i);
        gm.path = std::string("/roms/") + sy[0] + "/" + std::to_string(i) + ".rom";
        gm.meta.gametime = uint32_t(3600 * (i + 1) + 60 * i + 5);
        gm.meta.playcount = uint32_t(i * 3 + 1);
        s.games.push_back(gm);
      }
      cx.systems.push_back(std::move(s));
    }
    auto st = Registry::instance().make("menu.game.stats", cx, "");
    CHECK(st != nullptr);
    if (st) {
      int at = 0;
      for (int step : {0, 4, 9, 30}) {
        for (; at < step; ++at) st->handle(Action::Down, cx);
        char tag[48];
        std::snprintf(tag, sizeof tag, "estadisticas-llena@%d", step);
        measure_screen(cx, *st, theme, w, h, tag, g, boxes, report);
      }
    }
    cx.systems.clear();
  }
  {  // the QR code of the web transfer (with an address and without one), the paired phones, and the question that pairs one
    write(tmp + "/net/fixture/ips.txt", "192.168.100.100\n");
    setenv("ZF_NET_ROOT", (tmp + "/net").c_str(), 1);
    setenv("ZF_WEBDEVICES", (tmp + "/webdevices").c_str(), 1);
    std::string devs;
    for (int i = 0; i < 10; ++i) {
      std::string token(64, 'a');
      token[0] = char('a' + i);
      devs += token + "\t" + (i == 0 ? std::string("A device with a very long name that does not fit in the row at all") : "Android phone " + std::to_string(i)) + "\t1000\t" +
              std::to_string(::time(nullptr) - time_t(i) * 7000) + "\n";
    }
    write(tmp + "/webdevices", devs);
    webpair::reload_for_test();
    for (const char* name : {"menu.webqr", "menu.webdevices"}) {
      auto s = Registry::instance().make(name, cx, "");
      CHECK(s != nullptr);
      if (!s) continue;
      int at = 0;
      for (int step : {0, 3, 7, 12}) {
        for (; at < step; ++at) s->handle(Action::Down, cx);
        char tag[48];
        std::snprintf(tag, sizeof tag, "%s@%d", name, step);
        measure_screen(cx, *s, theme, w, h, tag, g, boxes, report);
      }
    }
    {  // TRANSFER FILES with the switch on: the rows that only exist while it listens, and the ON of its row in NETWORK
      webtransfer::reset_for_test("1234");
      webtransfer::set_listen_port(0);
      cx.conf.set("zfront.webtransfer", "1");
      CHECK(webtransfer::apply(true));
      for (const char* name : {"menu.network.transfer", "menu.network"}) {
        auto s = Registry::instance().make(name, cx, "");
        CHECK(s != nullptr);
        if (!s) continue;
        int at = 0;
        for (int step : {0, 4, 9}) {
          for (; at < step; ++at) s->handle(Action::Down, cx);
          char tag[48];
          std::snprintf(tag, sizeof tag, "%s-web@%d", name, step);
          measure_screen(cx, *s, theme, w, h, tag, g, boxes, report);
        }
      }
      // the same menu on an image that ships Samba: its switch, then its login rows once it is on
      write(tmp + "/net/usr/sbin/smbd", "");
      for (const char* on : {"0", "1"}) {
        cx.conf.set("samba.enabled", on);
        auto s = Registry::instance().make("menu.network.transfer", cx, "");
        CHECK(s != nullptr);
        if (!s) continue;
        int at = 0;
        for (int step : {0, 6, 9}) {
          for (; at < step; ++at) s->handle(Action::Down, cx);
          char tag[48];
          std::snprintf(tag, sizeof tag, "menu.network.transfer-smb%s@%d", on, step);
          measure_screen(cx, *s, theme, w, h, tag, g, boxes, report);
        }
      }
      cx.conf.set("samba.enabled", "0");
      fs::remove(tmp + "/net/usr/sbin/smbd");
      webtransfer::apply(false);
      cx.conf.set("zfront.webtransfer", "0");
    }
    write(tmp + "/net/fixture/ips.txt", "");
    if (auto s = Registry::instance().make("menu.webqr", cx, "")) measure_screen(cx, *s, theme, w, h, "menu.webqr-sin-ip", g, boxes, report);
    ui::ConfirmOpts co;
    co.title = tr("ALLOW THIS DEVICE?");
    ui::ConfirmDialog ask("An Android phone with a rather long name", nullptr, co, false);
    measure_screen(cx, ask, theme, w, h, "permitir", g, boxes, report);
    unsetenv("ZF_NET_ROOT");
    unsetenv("ZF_WEBDEVICES");
    webpair::reload_for_test();
  }
  auto ui_menu = Registry::instance().make("menu.ui", cx, "");
  CHECK(ui_menu != nullptr);
  if (ui_menu) {
    std::vector<Op> ops;
    ui_menu->tick(1.0f, cx);
    ui_menu->draw(cx, ops);
    Transition t = ui_menu->handle(Action::A, cx);
    if (t.screen) measure_screen(cx, *t.screen, theme, w, h, "lista-opciones", g, boxes, report);
  }
  {  // the countdown of "resume last game at startup": a title with a long name, the picture, the number, the help
    resume::Target rt;
    rt.name = "The Legend of Zelda: Link's Awakening DX (Europe)";
    rt.shot = std::string(ZF_TEST_DATA) + "/cover-box.png";
    resume::ResumeScreen rs(rt);
    measure_screen(cx, rs, theme, w, h, "reanudar", g, boxes, report);
  }
  if (remake::next(cx.th)) {  // Next: the card half a gutter further below the title, and a short menu spread over the viewport with taller rows
    const ui::Metrics m = ui::Metrics::from(cx.th, float(w), float(h));
    CHECK(m.list_top == float(g.content_top()) + std::round(float(g.gutter()) / 2.0f));
    const float cap = std::floor(m.row_h * 1.2f);
    for (const char* name : {"menu.quit", "menu.main", "menu.ui", "menu.g.system", "menu.sound"}) {
      auto s = Registry::instance().make(name, cx, "");
      if (!s || !s->is_menu()) continue;
      std::vector<Op> ops;
      s->tick(1.0f, cx);
      s->draw(cx, ops);
      const Menu& mm = static_cast<const Menu&>(*s);
      if (mm.size() == 0) continue;
      float rh = 0;
      for (size_t i = 0; i < mm.size() && rh == 0; ++i)
        if (mm.entry(i).kind != EntryKind::Header && mm.entry(i).kind != EntryKind::Separator) rh = mm.row_h(i);
      const float content = mm.row_top(mm.size() - 1) + mm.row_h(mm.size() - 1);
      bool ok = rh >= m.row_h && rh <= cap;
      if (content > mm.viewport_h()) ok = rh == m.row_h;  // it scrolls: the theme's row
      else {  // as tall as allowed (the last whole cell under the cap), or the viewport is full (less than a cell left per row)
        const float cell = float(m.u), top_cells = std::floor(cap / cell) * cell;
        ok = ok && (rh == top_cells || mm.viewport_h() - content < float(mm.size()) * cell);
      }
      if (!ok) std::printf("  %s %dx%d %s: fila %.0f (tema %.0f, tope %.0f), contenido %.0f de %.0f\n", theme, w, h, name, rh, m.row_h, cap, content, mm.viewport_h());
      CHECK(ok);
    }
  }
  auto quit = Registry::instance().make("menu.quit", cx, "");
  CHECK(quit != nullptr);
  if (quit) {
    std::vector<Op> ops;
    quit->tick(1.0f, cx);
    quit->draw(cx, ops);
    Transition t = quit->handle(Action::A, cx);
    if (t.screen) measure_screen(cx, *t.screen, theme, w, h, "pregunta", g, boxes, report);
  }
}

/// The menus at the normal text size and, on the three sizes of the consoles that matter most (480x320, 640x480, 720x720), also at LARGE and at
/// EXTRA LARGE with high contrast (accessibility: text 1.15x and 1.3x, rows taller and fewer, the same grid and the same margins). The
/// variant is part of the theme name in the report. ZF_LAYOUT_ACCESS=0 skips the variants (a quicker run).
void measure_menus(const std::string& gen_dir, const char* theme, int w, int h, const std::string& tmp, std::vector<remake::Box>& boxes, bool report) {
  ui::set_access(ui::TextSize::Normal, false);
  measure_menus_once(gen_dir, theme, w, h, tmp, boxes, report);
  const bool wanted = (w == 480 && h == 320) || (w == 640 && h == 480) || (w == 720 && h == 720);
  const char* off = std::getenv("ZF_LAYOUT_ACCESS");
  if (!wanted || (off && std::string(off) == "0")) return;
  const std::string large = std::string(theme) + "+LARGE", xl = std::string(theme) + "+XL+HC";
  ui::set_access(ui::TextSize::Large, false);
  measure_menus_once(gen_dir, large.c_str(), w, h, tmp, boxes, report);
  ui::set_access(ui::TextSize::XLarge, true);
  measure_menus_once(gen_dir, xl.c_str(), w, h, tmp, boxes, report);
  ui::set_access(ui::TextSize::Normal, false);
}

/// On the device a picture is never decoded by the frame that needs it: that stops the screen (up to 400 ms the first time a list opened).
void no_stop_while_drawing(const Theme& th, const char* theme, const std::vector<SystemItem>& items, const std::vector<std::string>& names, const Status& st, float w, float h) {
  std::string err;
  Assets b;
  CHECK(b.load_font(th.path(th.str("font")), err));
  Common common;
  CHECK(load_common(th, b, common, err));
  remake::state().inline_decode = false;
  for (float cam : {0.0f, 0.5f, 1.0f}) {
    std::vector<Op> ops;
    InfoFade fade;
    fade.item = 1;
    RecentRow recent;
    system_view(th, b, common, w, h, items, 1, cam, st, &recent, fade, ops);
    remake::state().pending.clear();
  }
  DetailedData d;
  d.name = names[0];
  std::vector<Op> ops;
  list_view(th, b, common, w, h, "Super Nintendo", names, 2, st, &d, nullptr, ops);
  remake::state().pending.clear();
  {  // a game list whose game and rows have pictures: asked of the job thread, none decoded by this frame (the stall of the first time each cover appears)
    const std::string cover = std::string(ZF_TEST_DATA) + "/cover-box.png";
    remake::state().row_image = [cover](size_t) { return cover; };
    DetailedData with;
    with.name = names[0];
    with.image = cover;
    with.decode_inline = false;
    std::vector<Op> ops2;
    list_view(th, b, common, w, h, "Super Nintendo", names, 2, st, &with, nullptr, ops2);
    if (remake::state().pending.empty() && !with.cover_missing) {
      ++g_bad;
      std::printf("MAL %s: la lista no pide la caratula al hilo de trabajo\n", theme);
    }
    remake::state().pending.clear();
    remake::state().row_image = nullptr;
  }
  remake::state().inline_decode = true;
  if (b.inline_fits()) {
    ++g_bad;
    std::printf("MAL %s: %zu imagenes decodificadas mientras se dibuja\n", theme, b.inline_fits());
  }
}

/// The Zenith folders by the same rules; `strict` false: counted apart, they do not fail the test.
void measure_zenith(const std::string& dir, const std::vector<const char*>& folders, const std::vector<std::string>& names, const std::string& root, bool report, bool strict) {
  g_report_only = !strict;
  Assets shot_maker;
  const std::string shot = root + "/shot.png";
  std::string err;
  CHECK(save_png(cpu_render({}, shot_maker, 320, 240), shot, err));
  const int sizes[][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 540}, {720, 720}, {800, 480}, {854, 480}, {1024, 768}, {1280, 720}};
  for (const char* folder : folders) {
    const std::string theme_dir = dir + "/" + folder;
    for (const auto& sz : sizes) {
      Theme th;
      CHECK(Theme::load(theme_dir, ratio_name(unsigned(sz[0]), unsigned(sz[1])), th, err));
      Assets a;
      CHECK(a.load_font(th.path(th.str("font")), err));
      Common common;
      CHECK(load_common(th, a, common, err));
      const float w = float(sz[0]), h = float(sz[1]);
      const remake::Grid g(w, h);
      Status st = Status::demo("12:04");
      std::vector<SystemItem> items(5);
      const char* sys[][2] = {{"snes", "Super Nintendo"}, {"megadrive", "Sega Mega Drive"}, {"psp", "PlayStation Portable"}, {"gba", "Game Boy Advance"}, {"nes", "Nintendo Entertainment System"}};
      for (size_t i = 0; i < items.size(); ++i) {
        items[i].theme_name = sys[i][0];
        items[i].full_name = sys[i][1];
        items[i].games = 19;
      }
      if (strict) no_stop_while_drawing(th, folder, items, names, st, w, h);
      std::vector<remake::Box> boxes;
      remake::audit_sink() = &boxes;
      if (std::getenv("ZF_LAYOUT_ROOM_STRIP") && strict) {  // the home view while the row opens: pictures of eleven steps of `row_room`
        for (int step = 0; step <= 10; ++step) {
          std::vector<Op> ops;
          InfoFade fade;
          RecentRow recent;
          recent.decode_inline = true;
          for (int i = 0; i < 3; ++i) recent.items.push_back(RecentItem{shot, "A game " + std::to_string(i), "11 h"});
          remake::state().row_room = float(step) / 10.0f;
          system_view(th, a, common, w, h, items, 0, 0.0f, st, &recent, fade, ops);
          char nm[32];
          std::snprintf(nm, sizeof nm, "room%02d", step);
          save_shot(std::move(ops), a, folder, sz[0], sz[1], nm, g);
        }
        remake::state().row_room = 1.0f;
        for (int sys = 0; sys < 3; ++sys)
          for (int fixed = 0; fixed < 2; ++fixed) {
            const bool has = sys != 1;  // the middle system has nothing to resume
            std::vector<Op> ops;
            InfoFade fade;
            RecentRow recent;
            recent.decode_inline = true;
            if (fixed) {
              const char* from[] = {"SNES", "PSP", "GBA"};
              for (int i = 0; i < 3; ++i) recent.items.push_back(RecentItem{shot, std::string("A game ") + std::to_string(i) + " - " + from[i], "11 h"});
            } else if (has) {
              for (int i = 0; i < 3; ++i) recent.items.push_back(RecentItem{shot, "A game " + std::to_string(i), "11 h"});
            }
            const bool row = fixed || has;
            remake::state().row_room = row ? 1.0f : 0.0f;
            system_view(th, a, common, w, h, items, sys, float(sys), st, row ? &recent : nullptr, fade, ops);
            char nm[32];
            std::snprintf(nm, sizeof nm, "nav-%s-%d", fixed ? "fija" : "hoy", sys);
            save_shot(std::move(ops), a, folder, sz[0], sz[1], nm, g);
          }
        remake::state().row_room = 1.0f;
      }
      if (strict) {  // no games to resume: the card alone, in the middle
        boxes.clear();
        std::vector<Op> ops;
        InfoFade fade;
        RecentRow none;
        remake::state().row_room = 0.0f;
        system_view(th, a, common, w, h, items, 0, 0.0f, st, &none, fade, ops);
        remake::state().row_room = 1.0f;
        check_screen(folder, sz[0], sz[1], "inicio-solo", g, boxes, report);
      }
      for (int k : {0, 4}) {
        boxes.clear();
        std::vector<Op> ops;
        InfoFade fade;
        fade.item = k;
        RecentRow recent;
        recent.decode_inline = true;
        for (int i = 0; i < 3; ++i) recent.items.push_back(RecentItem{shot, "A game with a long title " + std::to_string(i), strict ? "11 h ago - NINTENDO ENTERTAINMENT SYSTEM" : "11 h"});  // Next adds the console to it
        system_view(th, a, common, w, h, items, k, float(k), st, &recent, fade, ops);
        if (strict) {  // Next: the caption of every tile says when and on which console, and stays inside the tile
          int shown = 0;
          for (const remake::Box& q : boxes)
            if (q.tag.rfind("\"11 h ago", 0) == 0) ++shown;
          if (shown != 3) bad(folder, sz[0], sz[1], "inicio", "falta el 'hace ... - consola' de las miniaturas: " + std::to_string(shown) + " de 3");
        }
        check_screen(folder, sz[0], sz[1], k ? "inicio2" : "inicio", g, boxes, report);
        save_shot(std::move(ops), a, folder, sz[0], sz[1], k ? "inicio2" : "inicio", g);
      }
      DetailedData d;
      d.name = names[2];
      d.year = 1994;
      d.genre = "A role playing game with a long genre";
      d.players = "1-2";
      d.rating = 0.7f;
      d.desc.assign(4000, 'x');
      for (size_t i = 8; i < d.desc.size(); i += 9) d.desc[i] = ' ';
      d.desc_time = -1;
      const DetailedData* const details[] = {&d, nullptr};
      for (const DetailedData* detail : details)
        for (size_t cur : {size_t(0), size_t(2), size_t(19)}) {
          boxes.clear();
          std::vector<Op> ops;
          list_view(th, a, common, w, h, "Super Nintendo", names, cur, st, detail, nullptr, ops);
          char screen[24];
          std::snprintf(screen, sizeof screen, "lista%s%zu", detail ? "D" : "", cur);
          check_screen(folder, sz[0], sz[1], screen, g, boxes, report);
        }
      measure_menus(theme_dir, folder, sz[0], sz[1], root, boxes, report);
      remake::audit_sink() = nullptr;
    }
  }
  g_report_only = false;
}

}  // namespace

int main() {
  const std::string light = find_base("theme-light"), dark = find_base("theme-dark");
  if (light.empty() || dark.empty()) {
    std::printf("sin carpetas del tema: se omite\n");
    return zt::done();
  }
  const bool report = std::getenv("ZF_LAYOUT_REPORT") != nullptr;
  const char* real = std::getenv("ZF_ES_THEMES");
  char tmpl[] = "/tmp/zf-test-layout-XXXXXX";
  const std::string root = mkdtemp(tmpl);
  fake_battery(root);
  const char* lang = std::getenv("ZF_LAYOUT_LANG");  // the catalogs of ZF_LOCALE_DIR: other languages are longer
  i18n::set_language(lang ? lang : "", ui_language_options());
  std::vector<std::string> names = {"A game", "Another game", "The Legend of a very long name that does not fit in any row of any screen at all", "Short"};
  for (int i = 0; i < 20; ++i) names.push_back("Game number " + std::to_string(i));
  const int sizes[][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 540}, {720, 720}, {800, 480}, {854, 480}, {1024, 768}, {1280, 720}};
  // Iconic twice: its COLOR SCHEME subset (light by default, dark when chosen) changes every colour of its pages, so both are measured
  const struct {
    const char* folder;
    const char* scheme;
  } cases[] = {{"es-theme-xmb-fcamod", ""}, {"es-theme-art-book-next", ""}, {"es-theme-iconic", ""}, {"es-theme-iconic", "dark"}, {"es-theme-techdweeb", ""}};
  for (const auto& cs : cases) {
    const std::string label = std::string(cs.folder) + (*cs.scheme ? "-" : "") + cs.scheme;
    const char* folder = label.c_str();
    std::string es = root + "/" + cs.folder;
    if (real && fs::exists(std::string(real) + "/" + cs.folder + "/theme.xml")) {
      es = std::string(real) + "/" + cs.folder;
    } else {
      fs::create_directories(es);
      std::ofstream(es + "/theme.xml") << "<theme><view name=\"system\"><carousel name=\"systemcarousel\"><type>horizontal</type></carousel></view>"
                                          "<subset name=\"color-scheme\"><include name=\"light\"/><include name=\"dark\"/></subset></theme>";
    }
    const std::string choice = cs.scheme;
    estheme::Options opt;
    if (!choice.empty())
      opt.setting = [&choice](const std::string& k) -> const std::string* { return k == "subset.color-scheme" ? &choice : nullptr; };
    estheme::Generated gen;
    std::string err;
    CHECK(estheme::generate(es, estheme::Bases{light, dark}, root + "/gen", opt, gen, err));
    for (const auto& sz : sizes) {
      Theme th;
      CHECK(Theme::load(gen.dir, ratio_name(unsigned(sz[0]), unsigned(sz[1])), th, err));
      Assets a;
      CHECK(a.load_font(th.path(th.str("font")), err));
      const float w = float(sz[0]), h = float(sz[1]);
      const remake::Grid g(w, h);
      Status st = Status::demo("12:04");
      st.bluetooth = true;
      std::vector<SystemItem> items(5);
      const char* sys[][2] = {{"snes", "Super Nintendo"}, {"megadrive", "Sega Mega Drive"}, {"psp", "PlayStation Portable"}, {"gba", "Game Boy Advance"}, {"nes", "Nintendo Entertainment System"}};
      for (size_t i = 0; i < items.size(); ++i) {
        items[i].theme_name = sys[i][0];
        items[i].full_name = sys[i][1];
        items[i].games = 19;
      }
      remake::state().inline_decode = true;
      remake::state().system_theme = "snes";
      no_stop_while_drawing(th, folder, items, names, st, w, h);
      std::vector<remake::Box> boxes;
      remake::audit_sink() = &boxes;
      for (int k : {0, 4}) {
        boxes.clear();
        std::vector<Op> ops;
        InfoFade fade;
        fade.item = k;
        RecentRow recent;
        recent.decode_inline = true;
        for (int i = 0; i < 3; ++i) recent.items.push_back(RecentItem{"/nonexistent/" + std::to_string(i) + ".png", "A game with a long title " + std::to_string(i), "11 h ago", 0});
        remake::system_view(th, a, w, h, items, k, float(k), st, &recent, fade, ops);
        check_screen(folder, sz[0], sz[1], k ? "inicio2" : "inicio", g, boxes, report);
        save_shot(std::move(ops), a, folder, sz[0], sz[1], k ? "inicio2" : "inicio", g);
      }
      DetailedData d;
      d.name = names[2];
      d.year = 1994;
      d.genre = "A role playing game with a long genre";
      d.players = "1-2";
      d.rating = 0.7f;
      d.desc.assign(4000, 'x');
      for (size_t i = 8; i < d.desc.size(); i += 9) d.desc[i] = ' ';
      d.desc_time = -1;
      const DetailedData* const details[] = {&d, nullptr};
      for (const DetailedData* detail : details)
        for (size_t cur : {size_t(0), size_t(2), size_t(19)}) {
          boxes.clear();
          std::vector<Op> ops;
          remake::list_view(th, a, w, h, "Super Nintendo", names, cur, st, detail, ops);
          char screen[24];
          std::snprintf(screen, sizeof screen, "lista%s%zu", detail ? "D" : "", cur);
          check_screen(folder, sz[0], sz[1], screen, g, boxes, report);
        }
      {  // a search with no match: the list has no names, the header and the help still draw
        const std::vector<std::string> none;
        boxes.clear();
        std::vector<Op> ops;
        remake::list_view(th, a, w, h, "Super Nintendo", none, 0, st, nullptr, ops);
        check_screen(folder, sz[0], sz[1], "listaVacia", g, boxes, report);
      }
      measure_menus(gen.dir, folder, sz[0], sz[1], root, boxes, report);
      remake::audit_sink() = nullptr;
    }
  }
  if (real) {  // with the real themes: everything the rules are about went through them
    for (const char* kind : {"titulo", "texto", "barra", "ayuda", "columna", "pista", "desplazamiento", "tarjeta", "caratula"}) {
      if (report) std::printf("    medidas %-16s %d\n", kind, g_seen[kind]);
      CHECK(g_seen[kind] > 0);
    }
  }
  {  // Zenith Next is on the grid: its folders, as the package makes them, count like the rest
    const std::string next = root + "/next";
    for (const auto& [from, to] : {std::pair<std::string, std::string>{light, "es-theme-zenith-next"}, std::pair<std::string, std::string>{dark, "es-theme-zenith-next-dark"}}) {
      fs::create_directories(next);
      fs::copy(fs::canonical(from), next + "/" + to, fs::copy_options::recursive);
      std::ifstream in(next + "/" + to + "/theme.xml");
      std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      const size_t at = xml.find("<variables>");
      CHECK(at != std::string::npos);
      xml.insert(at + 11, "<zenStyle>next</zenStyle>");
      if (const char* v = std::getenv("ZF_LAYOUT_NEXT_VARS")) xml.insert(at + 11, v);  // to try a variable on Next (<menuCards>on</menuCards>)
      std::ofstream(next + "/" + to + "/theme.xml") << xml;
    }
    measure_zenith(next, {"es-theme-zenith-next", "es-theme-zenith-next-dark"}, names, root, report, true);
  }
  if (const char* z = std::getenv("ZF_LAYOUT_ZENITH")) {
    measure_zenith(z, {"es-theme-zenith", "es-theme-zenith-dark"}, names, root, report, false);
    std::printf("zenith, fallos sin contar para la prueba: %d\n", g_zenith);
  }
  if (report)
    for (const auto& [kind, n] : g_seen) std::printf("    %-40s %d\n", kind.c_str(), n);
  std::error_code ec;
  fs::remove_all(root, ec);
  std::printf("fallos de maquetacion: %d\n", g_bad);
  CHECK_EQ(g_bad, 0);
  return zt::done();
}
