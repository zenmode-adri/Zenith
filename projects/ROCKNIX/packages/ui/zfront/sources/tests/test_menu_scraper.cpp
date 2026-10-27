// menu.scraper and its sub-screens, driven like the user would (keys) on the fake-curl rig of test_scraper_run: what each entry
// writes to es_settings.cfg, the systems left out, the account test, and the progress screen (cancel with B, back with B).
#include <cstdio>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "menu_main.hpp"
#include "menu_scraper.hpp"
#include "registry.hpp"
#include "scraper_fake.hpp"
#include "scraper_run.hpp"

using namespace zf;
using namespace zf::scraper;
using namespace scrfake;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

void make_systems(Rig& t) {
  put(t.roms() + "/a.sfc", "ROMa");
  t.known("a");
  t.systems3();
  std::string err;
  CHECK_EQ(t.cx.systems.size(), 3u);
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", t.cx.th, err));
  t.cx.w = 640, t.cx.h = 480;
}

std::unique_ptr<Screen> open(Ctx& cx, const char* name) {
  CHECK(Registry::instance().has(name));
  std::unique_ptr<Screen> s = Registry::instance().make(name, cx, "");
  CHECK(s != nullptr);
  if (s) s->tick(0.0f, cx);
  return s;
}
Menu* menu_of(std::unique_ptr<Screen>& s) { return static_cast<Menu*>(s.get()); }
int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}
bool focus(Menu& m, Ctx& cx, const std::string& label) {
  int idx = find(m, label);
  if (idx < 0) return false;
  for (size_t k = 0; k <= m.size() && m.cursor() != idx; ++k) m.handle(Action::Down, cx);
  return m.cursor() == idx;
}
void pump(Ctx& cx, Menu* m = nullptr) {
  for (int i = 0; i < 600; ++i) {
    cx.sv->jobs.drain();
    if (cx.sv->jobs.idle()) break;
    ::usleep(5000);
  }
  cx.sv->jobs.drain();
  if (m) m->tick(0.0f, cx);
}
std::string es_val(Ctx& cx, const char* k) {
  const std::string* v = cx.es.get(k);
  return v ? *v : std::string("<unset>");
}

}  // namespace

int main() {
  // ---- registered, and the main menu shows it
  {
    Rig t;
    make_systems(t);
    for (const char* n : {"menu.scraper", "menu.scraper.systems", "menu.scraper.options", "menu.scraper.account", "menu.scraper.run"}) CHECK(Registry::instance().has(n));
    bool row = false;
    for (const MainRow& r : main_rows())
      if (r.screen == "menu.g.library") row = true;  // the scraper is reached through LIBRARY
    CHECK(row);
  }

  // ---- main menu: START needs a developer login and at least one system
  {
    Rig t;
    make_systems(t);
    ::unsetenv("ZF_SS_DEVLOGIN");
    auto s = open(t.cx, "menu.scraper");
    Menu& m = *menu_of(s);
    int st = find(m, tr("START"));
    CHECK(st >= 0);
    CHECK(!m.entry(size_t(st)).enabled);
    CHECK(find(m, tr("THIS BUILD CANNOT REACH SCREENSCRAPER")) >= 0);
    ::setenv("ZF_SS_DEVLOGIN", "devid=DEVIDSECRET&devpassword=DEVPWSECRET", 1);
    auto s2 = open(t.cx, "menu.scraper");
    Menu& m2 = *menu_of(s2);
    st = find(m2, tr("START"));
    CHECK(m2.entry(size_t(st)).enabled);
    CHECK(find(m2, tr("THIS BUILD CANNOT REACH SCREENSCRAPER")) < 0);
    // the systems entry counts only the ones ScreenScraper knows (ports does not)
    int sy = find(m2, tr("SYSTEMS"));
    CHECK_EQ(m2.entry(size_t(sy)).value_text, std::string("2 / 2"));
    // the filter and the "recently scraped" choice persist
    CHECK(focus(m2, t.cx, tr("GAMES TO SCRAPE")));
    m2.handle(Action::Right, t.cx);
    CHECK(es_val(t.cx, "ZfScraperFilter") != "<unset>");
    CHECK(focus(m2, t.cx, tr("IGNORE RECENTLY SCRAPED GAMES")));
    m2.handle(Action::Right, t.cx);
    CHECK_EQ(es_val(t.cx, "RecentlyScrappedFilter"), std::string("4"));
    // START opens the run screen
    CHECK(focus(m2, t.cx, tr("START")));
    Transition tr_ = m2.handle(Action::A, t.cx);
    CHECK(tr_.kind == Transition::Open);
    CHECK_EQ(tr_.name, std::string("menu.scraper.run"));
  }

  // ---- systems: toggles, select none / all, the run uses the selection
  {
    Rig t;
    make_systems(t);
    auto s = open(t.cx, "menu.scraper.systems");
    Menu& m = *menu_of(s);
    CHECK(find(m, "Super Nintendo") >= 0 && find(m, "Game Boy Advance") >= 0);
    CHECK(find(m, "Ports") < 0);
    CHECK_EQ(scraper_run_options(t.cx).systems.size(), 2u);
    CHECK(focus(m, t.cx, "Game Boy Advance"));
    m.handle(Action::A, t.cx);
    CHECK_EQ(es_val(t.cx, "ZfScraperSkip"), std::string("gba"));
    Options o = scraper_run_options(t.cx);
    CHECK_EQ(o.systems.size(), 1u);
    CHECK_EQ(t.cx.systems[o.systems[0]].name, std::string("snes"));
    CHECK(focus(m, t.cx, tr("SELECT NONE")));
    m.handle(Action::A, t.cx);
    m.tick(0.0f, t.cx);
    CHECK_EQ(scraper_run_options(t.cx).systems.size(), 0u);
    CHECK(focus(m, t.cx, tr("SELECT ALL")));
    m.handle(Action::A, t.cx);
    m.tick(0.0f, t.cx);
    CHECK_EQ(scraper_run_options(t.cx).systems.size(), 2u);
    CHECK_EQ(es_val(t.cx, "ZfScraperSkip"), std::string(""));
  }

  // ---- settings write ES's keys
  {
    Rig t;
    make_systems(t);
    auto s = open(t.cx, "menu.scraper.options");
    Menu& m = *menu_of(s);
    CHECK(focus(m, t.cx, tr("IMAGE")));
    m.handle(Action::Right, t.cx);
    CHECK_EQ(es_val(t.cx, "ScrapperImageSrc"), std::string("sstitle"));
    CHECK(focus(m, t.cx, tr("VIDEOS")));
    m.handle(Action::A, t.cx);
    CHECK(!t.cx.es.get_bool("ScrapeVideos", true));
    CHECK(focus(m, t.cx, tr("FANART")));
    m.handle(Action::A, t.cx);
    CHECK(t.cx.es.get_bool("ScrapeFanart", false));
    CHECK(focus(m, t.cx, tr("IMAGE SIZE")));
    m.handle(Action::Right, t.cx);  // 640 -> 1024
    CHECK_EQ(es_val(t.cx, "ScraperResizeWidth"), std::string("1024"));
    Options o = scraper_run_options(t.cx);
    CHECK(!o.wish.video && o.wish.fanart);
    CHECK_EQ(o.wish.image_src, std::string("sstitle"));
    CHECK_EQ(o.resize_px, 1024);
  }

  // ---- account: fields, test with the fake server, nothing secret on screen
  {
    Rig t;
    make_systems(t);
    auto s = open(t.cx, "menu.scraper.account");
    Menu& m = *menu_of(s);
    int u = find(m, tr("USERNAME")), p = find(m, tr("PASSWORD"));
    CHECK(u >= 0 && p >= 0);
    m.entry(size_t(u)).set_s("USERSECRET");
    m.entry(size_t(p)).set_s("PASSSECRET");
    CHECK_EQ(es_val(t.cx, "ScreenScraperUser"), std::string("USERSECRET"));
    CHECK_EQ(es_val(t.cx, "ScreenScraperPass"), std::string("PASSSECRET"));
    CHECK(focus(m, t.cx, tr("TEST ACCOUNT")));
    m.handle(Action::A, t.cx);
    pump(t.cx, &m);
    m.tick(0.0f, t.cx);
    CHECK(find(m, tr("REQUESTS TODAY")) >= 0);
    CHECK_EQ(m.entry(size_t(find(m, tr("REQUESTS TODAY")))).value_text, std::string("12 / 20000"));
    for (size_t i = 0; i < m.size(); ++i) {
      CHECK(!has_secret(m.entry(i).label));
      if (i != size_t(p)) CHECK(!has_secret(m.entry(i).value_text) || i == size_t(u));
    }
    CHECK(m.entry(size_t(p)).password);
    for (const std::string& c : t.cx.sv->cmd.log) CHECK(!has_secret(c));
    // wrong login
    put(t.fake + "/user.http", "403");
    put(t.fake + "/user.xml", "Erreur de login");
    CHECK(focus(m, t.cx, tr("TEST ACCOUNT")));
    m.handle(Action::A, t.cx);
    pump(t.cx, &m);
    m.tick(0.0f, t.cx);
    CHECK(find(m, tr("REQUESTS TODAY")) < 0);
    CHECK(find(m, describe(Abort::BadLogin)) >= 0);
  }

  // ---- the progress screen: B stops, what was done is saved, B again goes back
  {
    Rig t;
    make_systems(t);
    for (int i = 0; i < 8; ++i) {
      std::string n = "g" + std::to_string(i);
      t.rom(n + ".sfc");
      t.known(n);
    }
    Options o = t.opts();
    o.systems = {0};
    o.flush_every = 100;
    o.min_interval_ms = 30;
    std::unique_ptr<Screen> s = make_scraper_run_screen(t.cx, o);
    auto step = [&] {
      t.cx.sv->jobs.drain();
      s->tick(0.01f, t.cx);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    // run until three games are done
    for (int i = 0; i < 20000; ++i) {
      step();
      if (t.requests().find("g2.sfc") != std::string::npos) break;
    }
    Transition tr1 = s->handle(Action::B, t.cx);  // stop
    CHECK(tr1.kind == Transition::None);
    bool back = false;
    for (int i = 0; i < 20000 && !back; ++i) {
      step();
      s->handle(Action::Down, t.cx);  // scrolling is harmless
      back = s->handle(Action::B, t.cx).kind == Transition::Pop;
    }
    CHECK(back);
    CHECK(count_of(t.gamelist(), "<scrap name=\"ScreenScraper\"") >= 1);
    CHECK(count_of(t.gamelist(), "<scrap name=\"ScreenScraper\"") < 8);
  }

  // ---- leaving by Start (close_menus) cancels the run and keeps the work
  {
    Rig t;
    make_systems(t);
    for (int i = 0; i < 6; ++i) {
      std::string n = "h" + std::to_string(i);
      t.rom(n + ".sfc");
      t.known(n);
    }
    Options o = t.opts();
    o.systems = {0};
    o.min_interval_ms = 30;
    std::unique_ptr<Screen> s = make_scraper_run_screen(t.cx, o);
    for (int i = 0; i < 20000 && t.requests().find("h1.sfc") == std::string::npos; ++i) {
      t.cx.sv->jobs.drain();
      s->tick(0.01f, t.cx);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    s->on_dismiss(t.cx);
    s.reset();  // the engine finishes on its own: drain until the worker is idle
    for (int i = 0; i < 2000; ++i) {
      t.cx.sv->jobs.drain();
      if (t.cx.sv->jobs.idle()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    t.cx.sv->jobs.drain();
    CHECK(count_of(t.gamelist(), "<scrap name=\"ScreenScraper\"") >= 1);
  }
  return zt::done();
}
