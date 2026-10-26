// Review pictures of the scraper screens: runs only when $ZF_SCR_PNG names an output directory (it does nothing under ctest).
// Both themes at the four screen sizes, every relevant state, on the fake-curl rig of test_scraper_run.
//   ZF_SCR_PNG=/home/epsil/zf-out/scraper ZF_THEME_DIR=/home/epsil/zenith-frontend ./test_menu_scraper_png
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "cpu.hpp"
#include "menu.hpp"
#include "menu_scraper.hpp"
#include "registry.hpp"
#include "scraper_fake.hpp"
#include "scraper_run.hpp"
#include "ui_dialogs.hpp"

using namespace zf;
using namespace zf::scraper;
using namespace scrfake;

namespace {

Menu* menu_of(std::unique_ptr<Screen>& s) { return static_cast<Menu*>(s.get()); }

std::unique_ptr<Screen> open(Ctx& cx, const char* name) {
  std::unique_ptr<Screen> s = Registry::instance().make(name, cx, "");
  if (s) s->tick(0.0f, cx);
  return s;
}
void pump(Ctx& cx, Menu* m) {
  for (int i = 0; i < 600; ++i) {
    cx.sv->jobs.drain();
    if (cx.sv->jobs.idle()) break;
    ::usleep(5000);
  }
  cx.sv->jobs.drain();
  if (m) m->tick(0.0f, cx);
}
void settle(Menu& m, Ctx& cx) {
  for (int i = 0; i < 6; ++i) m.tick(1.0f, cx);
}
bool focus(Menu& m, Ctx& cx, const std::string& label) {
  int idx = -1;
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) idx = int(i);
  if (idx < 0) return false;
  for (size_t k = 0; k <= m.size() && m.cursor() != idx; ++k) m.handle(Action::Down, cx);
  return m.cursor() == idx;
}

struct Shooter {
  std::string dir, theme_name;
  unsigned w = 0, h = 0;
  int n = 0;
  void shot(Ctx& cx, const char* name, Screen* base) {
    std::vector<Op> ops;
    base->draw(cx, ops);
    Canvas cv = cpu_render(ops, cx.assets, w, h);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s/%02d-%s-%s-%ux%u.png", dir.c_str(), n++, name, theme_name.c_str(), w, h);
    std::string err;
    CHECK(save_png(cv, buf, err));
  }
};

template <class F>
bool until(Ctx& cx, Screen& s, F pred, int ms = 30000) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    cx.sv->jobs.drain();
    s.tick(0.01f, cx);
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

std::string long_name(int i) {
  static const char* kNames[] = {"Super Mario World",
                                 "The Legend of Zelda: A Link to the Past",
                                 "Final Fantasy VI",
                                 "Chrono Trigger",
                                 "Super Metroid",
                                 "Street Fighter II Turbo: Hyper Fighting",
                                 "Donkey Kong Country 2: Diddy's Kong Quest",
                                 "Mega Man X",
                                 "Secret of Mana",
                                 "Kirby Super Star",
                                 "EarthBound",
                                 "F-Zero"};
  return kNames[i % 12];
}

void run_all(const std::string& theme_dir, const std::string& theme_name, unsigned w, unsigned h, const std::string& outdir) {
  Rig& t = *new Rig;  // leaked on purpose: framework caches key on the Assets address
  Ctx& cx = t.cx;
  std::string err;
  cx.w = float(w);
  cx.h = float(h);
  CHECK(Theme::load(theme_dir, ratio_name(w, h), cx.th, err));
  CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
  CHECK(load_common(cx.th, cx.assets, cx.common, err));
  cx.status = Status::demo();
  for (int i = 0; i < 12; ++i) {
    std::string stem = "game" + std::to_string(i);
    t.rom(stem + ".sfc");
    if (i != 3 && i != 8) t.known(stem);
    std::string xml = slurp(t.fake + "/game/" + stem + ".sfc.xml");
    if (!xml.empty()) put(t.fake + "/game/" + stem + ".sfc.xml", replace_all(xml, "Game " + stem, long_name(i)));
  }
  t.systems3();
  Shooter sh;
  sh.dir = outdir;
  sh.theme_name = theme_name;
  sh.w = w;
  sh.h = h;

  {  // menus
    auto s = open(cx, "menu.scraper");
    settle(*menu_of(s), cx);
    sh.shot(cx, "main", s.get());
  }
  {
    auto s = open(cx, "menu.scraper.systems");
    focus(*menu_of(s), cx, "Game Boy Advance");
    menu_of(s)->handle(Action::A, cx);
    settle(*menu_of(s), cx);
    sh.shot(cx, "systems", s.get());
    cx.es.set_string("ZfScraperSkip", "");
  }
  {
    auto s = open(cx, "menu.scraper.options");
    settle(*menu_of(s), cx);
    sh.shot(cx, "options-top", s.get());
    focus(*menu_of(s), cx, "REPLACE EXISTING MEDIA");
    settle(*menu_of(s), cx);
    sh.shot(cx, "options-bottom", s.get());
  }
  {
    auto s = open(cx, "menu.scraper.account");
    Menu& m = *menu_of(s);
    settle(m, cx);
    sh.shot(cx, "account-empty", s.get());
    m.entry(0).set_s("adri_zenith");
    m.entry(1).set_s("hunter2");
    focus(m, cx, "TEST ACCOUNT");
    m.handle(Action::A, cx);
    pump(cx, &m);
    settle(m, cx);
    sh.shot(cx, "account-tested", s.get());
    put(t.fake + "/user.http", "403");
    put(t.fake + "/user.xml", "Erreur de login");
    focus(m, cx, "TEST ACCOUNT");
    m.handle(Action::A, cx);
    pump(cx, &m);
    settle(m, cx);
    sh.shot(cx, "account-bad-login", s.get());
    fs::remove(t.fake + "/user.http");
    put(t.fake + "/user.xml", kUserOk);
    cx.es.set_string("ScreenScraperUser", "");
    cx.es.set_string("ScreenScraperPass", "");
  }

  // ---- the run screen
  Options base = t.opts();
  base.systems = {0};
  base.flush_every = 100;
  base.skip_days = 0;
  {  // just started: reading the lists
    Options o = base;
    fs::remove(t.fake + "/requests.log");
    auto s = make_scraper_run_screen(cx, o);
    sh.shot(cx, "run-starting", s.get());
    until(cx, *s, [&] { return false; }, 300);
  }
  {  // in the middle: pacing between two requests (below the 2 s that shows a countdown)
    Options o = base;
    o.min_interval_ms = 1500;
    fs::remove(t.fake + "/requests.log");
    auto s = make_scraper_run_screen(cx, o);
    auto& rs = static_cast<Screen&>(*s);
    until(cx, rs, [&] { return t.requests().find("game6.sfc") != std::string::npos; });
    until(cx, rs, [&] { return false; }, 200);
    sh.shot(cx, "run-running", s.get());
    until(cx, rs, [&] { return false; }, 100);
    s->handle(Action::B, cx);
    sh.shot(cx, "run-stopping", s.get());
    until(cx, rs, [&] { return s->handle(Action::Down, cx).kind == Transition::Pop; }, 20000);
  }
  {  // waiting for the server
    Options o = base;
    o.min_interval_ms = 45000;
    fs::remove(t.fake + "/requests.log");
    auto s = make_scraper_run_screen(cx, o);
    until(cx, *s, [&] { return t.requests().find("game1.sfc") != std::string::npos; });
    until(cx, *s, [&] { return false; }, 300);
    sh.shot(cx, "run-waiting", s.get());
    s->handle(Action::B, cx);
    until(cx, *s, [&] { return false; }, 500);
  }
  {  // everything fine except two games that ScreenScraper does not know
    Options o = base;
    o.min_interval_ms = 0;
    fs::remove(t.fake + "/requests.log");
    auto s = make_scraper_run_screen(cx, o);
    for (int i = 0; i < 4000; ++i) {
      cx.sv->jobs.drain();
      s->tick(0.01f, cx);
      if (cx.sv->jobs.idle() && t.requests().find("game11.sfc") != std::string::npos) {
        until(cx, *s, [&] { return false; }, 200);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sh.shot(cx, "run-summary", s.get());
  }
  {  // problems: media that cannot be downloaded, games the server fails on
    put(t.fake + "/media.http", "503");
    put(t.fake + "/game/game2.sfc.http", "500");
    put(t.fake + "/game/game5.sfc.http", "500");
    Options o = base;
    o.min_interval_ms = 0;
    o.filter = Options::Filter::All;
    fs::remove(t.fake + "/requests.log");
    auto s = make_scraper_run_screen(cx, o);
    for (int i = 0; i < 4000; ++i) {
      cx.sv->jobs.drain();
      s->tick(0.01f, cx);
      if (cx.sv->jobs.idle() && t.requests().find("game11.sfc") != std::string::npos) {
        until(cx, *s, [&] { return false; }, 300);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sh.shot(cx, "run-problems", s.get());
    for (int k = 0; k < 5; ++k) s->handle(Action::Down, cx);
    sh.shot(cx, "run-problems-scrolled", s.get());
    fs::remove(t.fake + "/media.http");
    fs::remove(t.fake + "/game/game2.sfc.http");
    fs::remove(t.fake + "/game/game5.sfc.http");
  }
  {  // the daily quota runs out in the middle
    fs::remove(t.roms() + "/gamelist.xml");
    Options o = base;
    o.min_interval_ms = 0;
    o.filter = Options::Filter::All;
    fs::remove(t.fake + "/requests.log");
    auto s = make_scraper_run_screen(cx, o);
    until(cx, *s, [&] { return t.requests().find("game4.sfc") != std::string::npos; });
    put(t.fake + "/game.http", "430");
    for (int i = 0; i < 4000; ++i) {
      cx.sv->jobs.drain();
      s->tick(0.01f, cx);
      if (cx.sv->jobs.idle() && !s->wake_in() && t.requests().find("game6.sfc") != std::string::npos) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    until(cx, *s, [&] { return false; }, 400);
    sh.shot(cx, "run-quota", s.get());
    fs::remove(t.fake + "/game.http");
  }
  {  // stopped by the user
    fs::remove(t.roms() + "/gamelist.xml");
    Options o = base;
    o.min_interval_ms = 200;
    o.filter = Options::Filter::All;
    fs::remove(t.fake + "/requests.log");
    auto s = make_scraper_run_screen(cx, o);
    until(cx, *s, [&] { return t.requests().find("game3.sfc") != std::string::npos; });
    s->handle(Action::B, cx);
    until(cx, *s, [&] { return false; }, 800);
    sh.shot(cx, "run-cancelled", s.get());
  }
  {  // nothing to do
    fs::remove(t.roms() + "/gamelist.xml");
    Options o = base;
    o.min_interval_ms = 0;
    o.filter = Options::Filter::MissingAny;
    o.skip_days = 15;
    // every game was just scraped by the run above? make it explicit: a stamped gamelist
    std::string gl = "<gameList>\n";
    for (int i = 0; i < 12; ++i)
      gl += "<game><path>./game" + std::to_string(i) + ".sfc</path><scrap name=\"ScreenScraper\" date=\"20990101T000000\"/></game>\n";
    put(t.roms() + "/gamelist.xml", gl + "</gameList>\n");
    fs::remove(t.fake + "/requests.log");
    auto s = make_scraper_run_screen(cx, o);
    until(cx, *s, [&] { return false; }, 600);
    sh.shot(cx, "run-nothing", s.get());
  }
}

}  // namespace

int main() {
  const char* out = std::getenv("ZF_SCR_PNG");
  if (!out) return 0;
  const char* root = std::getenv("ZF_THEME_DIR");
  std::string base = root ? root : "/home/epsil/zenith-frontend";
  fs::create_directories(out);
  const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
  for (const char* theme : {"theme-dark", "theme-light"})
    for (const auto& wh : sizes) run_all(base + "/" + theme, theme + 6, wh[0], wh[1], out);
  return zt::done();
}
