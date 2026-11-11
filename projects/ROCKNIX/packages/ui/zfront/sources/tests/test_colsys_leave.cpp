// Collections as carousel systems (colsys.hpp): they appear and disappear with the settings, hold the right games in the right order,
// launch through the system the game belongs to, write favourites to that system's gamelist, follow kid / hidden rules, and stay out of
// `Ctx::systems` (play statistics, library doctor, web remote only see real systems).
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "collections.hpp"
#include "colsys.hpp"
#include "gamemeta.hpp"
#include "menu.hpp"
#include "registry.hpp"
#include "screens.hpp"
#include "syslist.hpp"
#include "uimode.hpp"
#include "util.hpp"

using namespace zf;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string g_tmp;

void touch(const std::string& p) {
  FILE* f = std::fopen(p.c_str(), "w");
  if (f) std::fclose(f);
}
void spit(const std::string& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }
std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A window the root opens with Start; B closes it (the menus' way to ask for a rebuild is the flags set meanwhile).
struct FakeMenu : Screen {
  Transition handle(Action a, Ctx&) override {
    if (a == Action::B) return Transition::pop();
    return Transition::none();
  }
  void draw(Ctx&, std::vector<Op>&) override {}
  bool is_menu() const override { return true; }
};

// gb: Tetris (favourite, 1 play, newest), Zelda (favourite, 3 plays), Metroid (never played)
// snes: Mario (never played), Kirby (2 plays, kid game), Hidden (hidden, never played)
void write_gamelists() {
  spit(g_tmp + "/roms/gb/gamelist.xml",
       "<?xml version=\"1.0\"?>\n<gameList>\n"
       "\t<game>\n\t\t<path>./Tetris.gb</path>\n\t\t<name>Tetris</name>\n\t\t<favorite>true</favorite>\n\t\t<playcount>1</playcount>\n\t\t<lastplayed>20260401T120000</lastplayed>\n\t</game>\n"
       "\t<game>\n\t\t<path>./Zelda.gb</path>\n\t\t<name>Zelda</name>\n\t\t<favorite>true</favorite>\n\t\t<playcount>3</playcount>\n\t\t<lastplayed>20260301T120000</lastplayed>\n\t</game>\n"
       "\t<game>\n\t\t<path>./Metroid.gb</path>\n\t\t<name>Metroid</name>\n\t</game>\n"
       "</gameList>\n");
  spit(g_tmp + "/roms/snes/gamelist.xml",
       "<?xml version=\"1.0\"?>\n<gameList>\n"
       "\t<game>\n\t\t<path>./Mario.sfc</path>\n\t\t<name>Mario</name>\n\t</game>\n"
       "\t<game>\n\t\t<path>./Kirby.sfc</path>\n\t\t<name>Kirby</name>\n\t\t<kidgame>true</kidgame>\n\t\t<playcount>2</playcount>\n\t\t<lastplayed>20260215T000000</lastplayed>\n\t</game>\n"
       "\t<game>\n\t\t<path>./Hidden.sfc</path>\n\t\t<name>Hidden</name>\n\t\t<hidden>true</hidden>\n\t</game>\n"
       "</gameList>\n");
}

std::string systems_xml() {
  auto sys = [](const char* name, const char* full, const char* ext) {
    return std::string("<system><name>") + name + "</name><fullname>" + full + "</fullname><path>/storage/roms/" + name + "</path><extension>" + ext +
           "</extension><command>run %ROM% -P%SYSTEM%</command></system>";
  };
  return "<systemList>" + sys("gb", "Game Boy", ".gb") + sys("snes", "Super Nintendo", ".sfc") + "</systemList>";
}

void setup(Ctx& cx, const char* auto_list, const char* smart_list = "", const char* custom_list = "") {
  std::string err;
  const char* e = std::getenv("ZF_THEME_DIR");
  Theme th;
  const std::string dir = e ? e : "/usr/share/zfront/theme-dark";  // the theme folder, or the one that holds theme-dark
  if ((!Theme::load(dir, ratio_name(640, 480), th, err) && !Theme::load(dir + "/theme-dark", ratio_name(640, 480), th, err)) || !cx.assets.load_font(th.path(th.str("font")), err)) {
    std::printf("test_colsys_leave: no theme (%s), skipped\n", err.c_str());
    std::exit(0);
  }
  cx.th = th;
  cx.w = 640, cx.h = 480;
  CHECK(load_common(cx.th, cx.assets, cx.common, err));
  cx.sv->cmd.dry_run = true;
  std::string xml = "<config>";
  if (*auto_list) xml += std::string("<string name=\"CollectionSystemsAuto\" value=\"") + auto_list + "\"/>";
  if (*smart_list) xml += std::string("<string name=\"ZfCollectionSystemsAuto\" value=\"") + smart_list + "\"/>";
  if (*custom_list) xml += std::string("<string name=\"CollectionSystemsCustom\" value=\"") + custom_list + "\"/>";
  xml += "</config>";
  cx.es = EsSettings::from_text(xml, g_tmp + "/es_settings.cfg");
  cx.conf = SystemConf::from_text("", "/nonexistent/system.cfg");
  CHECK(load_systems_text_lazy(systems_xml(), g_tmp + "/roms", cx.systems, err));
  cx.items = make_items(cx.systems, cx.es);
}

std::vector<std::string> cards(const Ctx& cx) { return item_names(cx.systems, cx.items, cx.cols); }
std::vector<std::string> v(std::initializer_list<const char*> l) { return std::vector<std::string>(l.begin(), l.end()); }

std::vector<std::string> names_of(const System& s) {
  std::vector<std::string> o;
  for (const Game& g : s.games) o.push_back(g.name);
  return o;
}
const System* col(const Ctx& cx, const char* name) {
  for (const System& c : cx.cols)
    if (c.name == name) return &c;
  return nullptr;
}
size_t index_of(const Ctx& cx, const char* name) {
  for (size_t j = 0; j < cx.cols.size(); ++j)
    if (cx.cols[j].name == name) return cx.systems.size() + j;
  return size_t(-1);
}

// jobs done (their callbacks run), the way the loop drains them
void settle(App& app) {
  for (int i = 0; i < 1000; ++i) {
    app.drain_jobs(Clock::now());
    if (app.cx.sv->jobs.idle()) {
      app.drain_jobs(Clock::now());
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(false);
}

}  // namespace

// A collection that empties while its list is still leaving the screen (B right after removing the last
// favourite, animations on): the rebuild must wait for the leave to end, the leaving list still draws it.
int main() {
  ::setenv("TZ", "UTC", 1);
  ::tzset();
  Registry::instance().add("menu.main", [](Ctx&, const std::string&) { return std::make_unique<FakeMenu>(); });
  g_tmp = "/tmp/zf_colleave_" + std::to_string(getpid());
  mkdir(g_tmp.c_str(), 0755);
  for (const char* d : {"/roms", "/roms/gb", "/roms/snes", "/collections"}) mkdir((g_tmp + d).c_str(), 0755);
  for (const char* f : {"gb/Tetris.gb", "gb/Zelda.gb", "gb/Metroid.gb", "snes/Mario.sfc", "snes/Kirby.sfc", "snes/Hidden.sfc"}) touch(g_tmp + "/roms/" + f);
  write_gamelists();
  spit(g_tmp + "/roms/gb/gamelist.xml",  // one favourite only
       "<?xml version=\"1.0\"?>\n<gameList>\n"
       "\t<game>\n\t\t<path>./Tetris.gb</path>\n\t\t<name>Tetris</name>\n\t\t<favorite>true</favorite>\n\t</game>\n"
       "\t<game>\n\t\t<path>./Zelda.gb</path>\n\t\t<name>Zelda</name>\n\t</game>\n"
       "\t<game>\n\t\t<path>./Metroid.gb</path>\n\t\t<name>Metroid</name>\n\t</game>\n"
       "</gameList>\n");
  ::setenv("ZF_COLLECTIONS_DIR", (g_tmp + "/collections").c_str(), 1);
  ::setenv("ZF_COLLECTIONS_ROOT", g_tmp.c_str(), 1);
  ::setenv("ZF_PLAYLOG", (g_tmp + "/playlog.tsv").c_str(), 1);
  collections::paths() = {g_tmp + "/collections", g_tmp};

  Ctx cx;
  setup(cx, "favorites");
  cx.ui_motion = true;
  App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
  app.refresh_collections(true);
  auto run = [&](float secs) {
    for (float t = 0; t < secs; t += 0.016f) {
      app.drain_jobs(Clock::now());
      app.tick(0.016f);
      app.render();
    }
  };
  CHECK_EQ(app.cx.cols.size(), size_t(1));
  app.handle(Action::Right);
  run(0.6f);
  app.handle(Action::Right);
  run(0.6f);  // on FAVORITES
  app.handle(Action::A);  // open its list
  run(0.6f);
  app.handle(Action::R3);  // remove the only favourite
  app.handle(Action::B);  // back to the carousel: the list stays drawn while it leaves
  settle(app);  // the rebuild is ready, the list is still leaving
  CHECK(slurp(g_tmp + "/roms/gb/gamelist.xml").find("<favorite>true") == std::string::npos);
  CHECK(app.collections_pending());
  CHECK_EQ(app.cx.cols.size(), size_t(1));
  app.tick(0.001f);
  app.render();  // used to read past the end of cx.cols
  run(0.5f);  // the leave ends: now the empty collection goes
  CHECK_EQ(app.cx.cols.size(), size_t(0));
  for (const std::string& n : cards(app.cx)) CHECK(n != "favorites");
  return zt::done();
}
