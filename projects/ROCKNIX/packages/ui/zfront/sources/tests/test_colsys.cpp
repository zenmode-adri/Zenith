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
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
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

int main() {
  ::setenv("TZ", "UTC", 1);
  ::tzset();
  Registry::instance().add("menu.main", [](Ctx&, const std::string&) { return std::make_unique<FakeMenu>(); });
  g_tmp = "/tmp/zf_colsys_" + std::to_string(getpid());
  mkdir(g_tmp.c_str(), 0755);
  for (const char* d : {"/roms", "/roms/gb", "/roms/snes", "/collections"}) mkdir((g_tmp + d).c_str(), 0755);
  for (const char* f : {"gb/Tetris.gb", "gb/Zelda.gb", "gb/Metroid.gb", "snes/Mario.sfc", "snes/Kirby.sfc", "snes/Hidden.sfc"}) touch(g_tmp + "/roms/" + f);
  write_gamelists();
  ::setenv("ZF_COLLECTIONS_DIR", (g_tmp + "/collections").c_str(), 1);
  ::setenv("ZF_COLLECTIONS_ROOT", g_tmp.c_str(), 1);
  ::setenv("ZF_PLAYLOG", (g_tmp + "/playlog.tsv").c_str(), 1);
  collections::paths() = {g_tmp + "/collections", g_tmp};

  // ---- nothing enabled: no collection; ES's default "favorites, recent" enables only "favorites" (the space is not trimmed)
  {
    Ctx cx;
    setup(cx, "-");  // a list with nothing we know
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    app.refresh_collections(true);
    CHECK_EQ(cards(app.cx), v({"gb", "snes"}));
    CHECK(app.cx.cols.empty());
  }
  {
    Ctx cx;
    setup(cx, "");  // no setting: ES's own default "favorites, recent"
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    app.refresh_collections(true);
    CHECK_EQ(cards(app.cx), v({"gb", "snes", "favorites"}));
  }

  // ---- members, order and names; real systems stay alone in `systems`
  {
    Ctx cx;
    setup(cx, "all,recent,favorites,neverplayed");
    App app(std::move(cx), std::make_unique<CarouselScreen>(1), make_status_source("/nonexistent"));
    app.refresh_collections(true);  // gb and snes were never opened: the build reads their gamelists itself
    Ctx& c = app.cx;
    CHECK_EQ(c.systems.size(), size_t(2));
    CHECK(!c.systems[0].loaded);  // reading them for the collections does not open them in the carousel
    CHECK_EQ(cards(c), v({"gb", "snes", "all", "favorites", "neverplayed", "recent"}));  // ES's order: all, favorites, neverplayed, recent
    const System* all = col(c, "all");
    const System* fav = col(c, "favorites");
    const System* nev = col(c, "neverplayed");
    const System* rec = col(c, "recent");
    CHECK(all && fav && nev && rec);
    if (all && fav && nev && rec) {
      CHECK(all->collection);
      CHECK_EQ(all->fullname, std::string("ALL GAMES"));
      CHECK_EQ(fav->theme, std::string("auto-favorites"));
      CHECK_EQ(rec->theme, std::string("auto-lastplayed"));
      CHECK_EQ(names_of(*all), v({"Kirby", "Mario", "Metroid", "Tetris", "Zelda"}));  // hidden Hidden is out; by name
      CHECK_EQ(names_of(*fav), v({"Tetris", "Zelda"}));
      CHECK_EQ(names_of(*nev), v({"Mario", "Metroid"}));
      CHECK_EQ(names_of(*rec), v({"Tetris", "Zelda", "Kirby"}));  // newest first (Apr, Mar, Feb)
      CHECK_EQ(all->count, size_t(5));
      CHECK_EQ(std::string(fav->games[0].src_system), std::string("gb"));
      CHECK_EQ(std::string(all->games[0].src_system), std::string("snes"));  // Kirby
    }
    // the carousel items point to the collections after the systems
    CHECK_EQ(c.items.size(), size_t(6));
    CHECK_EQ(c.items[2].sys, c.systems.size() + 0);
    CHECK_EQ(c.items[2].full_name, std::string("ALL GAMES"));
    CHECK_EQ(c.system_of(5), index_of(c, "recent"));
    // the cursor stayed on snes
    CHECK_EQ(app.cx.item_of(1), 1l);
  }

  // ---- disappear with the toggles, and empty ones are hidden
  {
    Ctx cx;
    setup(cx, "favorites,neverplayed");
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    app.refresh_collections(true);
    CHECK_EQ(cards(app.cx), v({"gb", "snes", "favorites", "neverplayed"}));
    app.cx.es.set_string("CollectionSystemsAuto", "neverplayed");
    app.refresh_collections(true);
    CHECK_EQ(cards(app.cx), v({"gb", "snes", "neverplayed"}));
    app.cx.es.set_string("CollectionSystemsAuto", "");
    app.refresh_collections(true);
    CHECK_EQ(cards(app.cx), v({"gb", "snes"}));
    CHECK(app.cx.cols.empty());
    // HiddenSystems hides a collection card by name too
    app.cx.es.set_string("CollectionSystemsAuto", "favorites");
    app.cx.es.set_string("HiddenSystems", "favorites");
    app.refresh_collections(true);
    CHECK_EQ(cards(app.cx), v({"gb", "snes"}));
    CHECK_EQ(app.cx.cols.size(), size_t(1));
  }
  {
    // no favourites at all: the collection is empty, so it is not shown
    Ctx cx;
    setup(cx, "favorites");
    std::vector<System> snap = cx.systems;
    colsys::fill(snap);
    for (System& s : snap)
      for (Game& g : s.games) g.meta.favorite = false;
    smartcol::Options o;
    o.now = 1800000000;
    CHECK(colsys::build(snap, cx.es, {}, o).empty());
  }

  // ---- kid mode and hidden games (pure build)
  {
    Ctx cx;
    setup(cx, "all,favorites,neverplayed,recent");
    std::vector<System> snap = cx.systems;
    colsys::fill(snap);
    smartcol::Options o;
    o.now = 1800000000;
    o.mode = uimode::Mode::Kid;
    std::vector<System> cols = colsys::build(snap, cx.es, {}, o);
    CHECK_EQ(cols.size(), size_t(2));  // only Kirby is a kid game: all and recent
    if (cols.size() == 2) {
      CHECK_EQ(cols[0].name, std::string("all"));
      CHECK_EQ(names_of(cols[0]), v({"Kirby"}));
      CHECK_EQ(cols[1].name, std::string("recent"));
    }
    o.mode = uimode::Mode::Full;
    o.show_hidden = true;  // hidden games are in the list only if the lists were read with them (load_games drops them): all stays 5
    cols = colsys::build(snap, cx.es, {}, o);
    CHECK_EQ(names_of(cols[0]).size(), size_t(5));
  }

  // ---- a custom collection (editable file and dynamic filter), before the automatic ones
  {
    spit(g_tmp + "/collections/custom-Mine.cfg", "./roms/snes/Kirby.sfc\n./roms/gb/Zelda.gb\n# comment\n./roms/gb/Gone.gb\n");
    collections::Filter f;
    f.systems = {"gb"};
    f.text = "ET";
    std::string err;
    CHECK(collections::create_dynamic("Filtered", f, err));
    Ctx cx;
    setup(cx, "favorites", "", "Mine,Filtered,Missing");
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    app.refresh_collections(true);
    CHECK_EQ(cards(app.cx), v({"gb", "snes", "Filtered", "Mine", "favorites"}));
    const System* mine = col(app.cx, "Mine");
    const System* fil = col(app.cx, "Filtered");
    CHECK(mine && fil);
    if (mine && fil) {
      CHECK_EQ(names_of(*mine), v({"Kirby", "Zelda"}));  // the missing ROM is skipped
      CHECK_EQ(names_of(*fil), v({"Metroid", "Tetris"}));  // gb games whose name has ET
      CHECK_EQ(mine->theme, std::string("custom-collections"));
    }
  }
  unlink((g_tmp + "/collections/custom-Mine.cfg").c_str());
  unlink((g_tmp + "/collections/Filtered.xcc").c_str());

  // ---- launching from a collection = launching from the source system; favourites go to the source gamelist
  {
    Ctx cx;
    setup(cx, "all,favorites");
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    app.refresh_collections(true);
    Ctx& c = app.cx;
    const size_t fav_idx = index_of(c, "favorites");
    CHECK(fav_idx != size_t(-1));
    // the same game, Zelda, from the favourites list and from gb's own list
    ListScreen from_col(fav_idx, c);
    from_col.handle(Action::Down, c);  // Tetris, Zelda
    Transition tc = from_col.handle(Action::A, c);
    ListScreen from_sys(0, c);
    // gb's list (name order): Metroid, Tetris, Zelda
    from_sys.handle(Action::Down, c);
    from_sys.handle(Action::Down, c);
    Transition ts = from_sys.handle(Action::A, c);
    CHECK(tc.kind == Transition::Launch);
    CHECK(ts.kind == Transition::Launch);
    CHECK_EQ(tc.cmd, ts.cmd);
    CHECK(!tc.cmd.empty());
    CHECK_EQ(tc.sys, 0);  // the source system and its game, whatever list it was started from
    CHECK_EQ(tc.sys, ts.sys);
    CHECK_EQ(tc.game, ts.game);
    CHECK(c.systems[0].loaded);  // starting it read the source's gamelist
    CHECK_EQ(c.systems[size_t(tc.sys)].games[size_t(tc.game)].name, std::string("Zelda"));
    CHECK_EQ(tc.cmd.find("-Pgb") != std::string::npos, true);

    // A on a snes game from ALL GAMES uses snes's command
    ListScreen from_all(index_of(c, "all"), c);  // Kirby first
    Transition ta = from_all.handle(Action::A, c);
    CHECK(ta.kind == Transition::Launch);
    CHECK(ta.cmd.find("-Psnes") != std::string::npos);
    CHECK_EQ(ta.sys, 1);
    CHECK(ta.cmd.find("Kirby.sfc") != std::string::npos);

    // R3 (favourite) on Zelda in FAVORITES: the flag goes to gb's gamelist, the row leaves the list, the source game changes
    colsys::clear_request();
    ListScreen favs(fav_idx, c);
    favs.handle(Action::Down, c);
    favs.handle(Action::R3, c);
    CHECK(!c.systems[0].games[2].meta.favorite);  // Zelda in gb's own memory (Metroid, Tetris, Zelda)
    CHECK(colsys::requested());                   // the carousel will rebuild when it is back
    favs.tick(0.0f, c);                           // the open list follows the change at once: the row leaves
    CHECK_EQ(names_of(c.cols[fav_idx - c.systems.size()]), v({"Tetris"}));
    settle(app);
    const std::string gl = slurp(g_tmp + "/roms/gb/gamelist.xml");
    const size_t z = gl.find("./Zelda.gb");
    CHECK(z != std::string::npos);
    CHECK(gl.find("<favorite>true</favorite>", z) == std::string::npos || gl.find("<favorite>true</favorite>", z) > gl.find("</game>", z));
    CHECK(!colsys::requested());  // the rebuild ran during settle (the carousel is the only screen)
  }

  // ---- after a game: play count goes to the source; LAST PLAYED and NEVER PLAYED follow after a rebuild
  {
    write_gamelists();
    Ctx cx;
    setup(cx, "neverplayed,recent");
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    app.refresh_collections(true);
    Ctx& c = app.cx;
    CHECK_EQ(names_of(*col(c, "neverplayed")), v({"Mario", "Metroid"}));
    ListScreen nev(index_of(c, "neverplayed"), c);  // Mario
    Transition t = nev.handle(Action::A, c);
    CHECK(t.kind == Transition::Launch);
    colsys::clear_request();
    game_played(c, size_t(t.sys), size_t(t.game), 120, 1780000000);
    CHECK_EQ(c.systems[1].games[size_t(t.game)].meta.playcount, 1u);
    CHECK(colsys::requested());  // a play asks for a rebuild
    settle(app);                 // the gamelist write, and the rebuild the request started
    CHECK(slurp(g_tmp + "/roms/snes/gamelist.xml").find("<playcount>1</playcount>") != std::string::npos);
    app.refresh_collections(true);
    CHECK_EQ(names_of(*col(c, "neverplayed")), v({"Metroid"}));
    CHECK_EQ(names_of(*col(c, "recent")).front(), std::string("Mario"));  // 2026-05, the newest play
    colsys::clear_request();
  }

  // ---- the result waits for the carousel; the cursor stays on its card by name; a collections-only reload (no system changed) rebuilds
  {
    write_gamelists();
    Ctx cx;
    setup(cx, "-");
    cx.system_loader = [](bool, std::vector<System>& out, std::string& e) {
      return load_systems_text_lazy(systems_xml(), g_tmp + "/roms", out, e);
    };
    App app(std::move(cx), std::make_unique<CarouselScreen>(1), make_status_source("/nonexistent"));  // on snes
    app.tick(0.0f);
    app.handle(Action::Start);  // a window is open
    app.cx.es.set_string("CollectionSystemsAuto", "all,favorites");
    app.refresh_collections(true);  // built now, but not shown while a window is on top
    CHECK(app.collections_pending());
    CHECK(app.cx.cols.empty());
    CHECK_EQ(cards(app.cx), v({"gb", "snes"}));
    app.handle(Action::B);  // the window closes: it is applied
    CHECK(!app.collections_pending());
    CHECK_EQ(cards(app.cx), v({"gb", "snes", "all", "favorites"}));
    // the cursor is still on snes (not on the first new card)
    CHECK_EQ(app.cx.system_of(size_t(app.cx.items.size() - 1)), index_of(app.cx, "favorites"));

    // the menus change the toggle and ask for a reload (nothing about the systems changes): the job rebuilds
    app.handle(Action::Start);
    app.cx.es.set_string("CollectionSystemsAuto", "favorites");
    collections::set_reload_needed(true);
    app.handle(Action::B);
    settle(app);
    CHECK_EQ(cards(app.cx), v({"gb", "snes", "favorites"}));
    CHECK(!collections::reload_needed());
    // and off: the cards go
    app.handle(Action::Start);
    app.cx.es.set_string("CollectionSystemsAuto", "");
    collections::set_reload_needed(true);
    app.handle(Action::B);
    settle(app);
    CHECK_EQ(cards(app.cx), v({"gb", "snes"}));
  }

  // ---- the library doctor, play stats and the web remote look at `systems`: collections are not in it
  {
    Ctx cx;
    setup(cx, "all,favorites");
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    app.refresh_collections(true);
    CHECK_EQ(app.cx.systems.size(), size_t(2));
    for (const System& s : app.cx.systems) CHECK(!s.collection);
    for (const System& s : app.cx.cols) CHECK(s.collection);
    CHECK(!app.cx.is_collection(0));
    CHECK(app.cx.is_collection(2));
  }
  // ---- systems that hold no games (screenshots viewer, tools, music) stay out of every collection, and the names of the automatic ones
  // are written like the systems' (on the console ALL GAMES counted 100 screenshots and 33 tools, and LAST PLAYED listed screenshots)
  {
    std::vector<System> snap;
    System gb;
    gb.name = "gb";
    gb.fullname = "Game Boy";
    gb.loaded = true;
    Game g;
    g.name = "Tetris";
    g.path = "/roms/gb/Tetris.gb";
    gb.games.push_back(g);
    System shots;
    shots.name = "imageviewer";
    shots.theme = "imageviewer";
    shots.loaded = true;
    Game p;
    p.name = "shot";
    p.path = "/roms/screenshots/shot.png";
    shots.games.push_back(p);
    System tools;
    tools.name = "tools";
    tools.platform = "tools";
    tools.loaded = true;
    Game t;
    t.name = "Backup";
    t.path = "/roms/tools/Backup.sh";
    tools.games.push_back(t);
    snap.push_back(gb);
    snap.push_back(shots);
    snap.push_back(tools);
    colsys::keep_game_systems(snap);
    CHECK_EQ(snap.size(), size_t(1));
    CHECK(!snap.empty() && snap[0].name == "gb");
  }
  return zt::done();
}
