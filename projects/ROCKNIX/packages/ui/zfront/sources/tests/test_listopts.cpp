// Gamelist options (ES GuiGamelistOptions + the GAMELIST OPTIONS group of the UI settings): SORT GAMES BY (`<system>.sort`),
// FAVORITES ON TOP, SHOW HIDDEN FILES, SHOW FILENAMES INSTEAD, IGNORE LEADING ARTICLES WHEN SORTING, and the favourites /
// genre filters. Everything is read through the ES keys of es_settings.cfg, as ES does, and shown by the game list.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "menu.hpp"
#include "registry.hpp"
#include "screens.hpp"
#include "settings.hpp"
#include "systems.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string g_tmp;

struct G {
  std::string name;
  int rating = 0;  // percent
  unsigned plays = 0;
  uint64_t last = 0;
  int year = -1;
  unsigned secs = 0;
  bool fav = false;
  std::string genre, players;
};

// One system `sysname` whose games are given in name order (the order the loader gives them with the default options).
void make_ctx(Ctx& cx, const std::string& sysname, const std::vector<G>& games, const std::string& es_body = std::string()) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = true;
  cx.es = EsSettings::from_text("<config>" + es_body + "</config>", "/nonexistent/es_settings.cfg");
  cx.conf = SystemConf::from_text("", "/nonexistent/system.cfg");
  System s;
  s.name = sysname;
  s.fullname = sysname;
  s.theme = sysname;
  s.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR%";
  s.emulator = "retroarch";
  s.core = "gambatte";
  s.emus = {{"retroarch", {"gambatte"}}};
  s.rom_dir = "/roms/" + sysname;
  s.gamelist = s.rom_dir + "/gamelist.xml";
  for (const G& x : games) {
    Game g;
    g.name = x.name;
    g.path = s.rom_dir + "/" + x.name + ".gb";
    g.meta.rating_pct = uint8_t(x.rating);
    g.meta.playcount = x.plays;
    g.meta.lastplayed = x.last;
    g.meta.release_year = x.year;
    g.meta.gametime = x.secs;
    g.meta.favorite = x.fav;
    g.meta.genre = gamelist_intern(x.genre);
    g.meta.players = gamelist_intern(x.players);
    s.games.push_back(std::move(g));
  }
  s.count = s.games.size();
  s.loaded = true;
  cx.systems.push_back(std::move(s));
}

// The names the list shows from the top, in order: walk it launching nothing, reading the game under the cursor each time.
std::vector<std::string> order(ListScreen& s, Ctx& cx) {
  std::vector<std::string> out;
  const System& sys = cx.systems[0];
  // go to the first row (L2 stops at the first letter: use Up with wrap until the cursor repeats)
  std::vector<long> seen;
  for (int i = 0; i < 64; ++i) {
    Transition t = s.handle(Action::A, cx);
    if (t.kind != Transition::Launch) break;
    if (!seen.empty() && t.game == seen[0]) break;  // wrapped: the whole list was seen once
    seen.push_back(t.game);
    s.handle(Action::Down, cx);
  }
  // after n Downs the cursor is back on the first row (wrap), so `seen` starts at the cursor's row and is the list once around
  for (long g : seen) out.push_back(sys.games[size_t(g)].name);
  return out;
}

std::vector<std::string> v(std::initializer_list<const char*> l) { return std::vector<std::string>(l.begin(), l.end()); }

// stored sort -> rows (ids as ES: 2/3 rating, 4/5 times played, 6/7 last played, 8/9 players, 10/11 release, 12/13 genre, 22/23 game time)
void test_sort_types() {
  const std::vector<G> games = {{"Alpha", 20, 5, 20260101000000, 1999, 600, false, "Rpg", "2"},
                                {"Bravo", 90, 1, 20260301000000, 1994, 60, false, "action", "1-2"},
                                {"Cobra", 0, 0, 0, -1, 0, false, "", ""},
                                {"Delta", 60, 9, 20260201000000, 2005, 7200, false, "Puzzle", "4"}};
  struct Case {
    int id;
    std::vector<std::string> want;
  };
  const Case cases[] = {
      {0, v({"Alpha", "Bravo", "Cobra", "Delta"})},   {1, v({"Delta", "Cobra", "Bravo", "Alpha"})},
      {2, v({"Cobra", "Alpha", "Delta", "Bravo"})},   {3, v({"Bravo", "Delta", "Alpha", "Cobra"})},
      {4, v({"Cobra", "Bravo", "Alpha", "Delta"})},   {5, v({"Delta", "Alpha", "Bravo", "Cobra"})},
      {6, v({"Cobra", "Alpha", "Delta", "Bravo"})},   {7, v({"Bravo", "Delta", "Alpha", "Cobra"})},
      {8, v({"Cobra", "Bravo", "Alpha", "Delta"})},   {9, v({"Delta", "Alpha", "Bravo", "Cobra"})},
      {10, v({"Cobra", "Bravo", "Alpha", "Delta"})},  {11, v({"Delta", "Alpha", "Bravo", "Cobra"})},
      {12, v({"Cobra", "Bravo", "Delta", "Alpha"})},  {13, v({"Alpha", "Delta", "Bravo", "Cobra"})},
      {22, v({"Cobra", "Bravo", "Alpha", "Delta"})},  {23, v({"Delta", "Alpha", "Bravo", "Cobra"})},
      // developer / publisher / system / file creation date: zfront has no data for them, the list keeps the name order (ES: out of its list -> 0)
      {14, v({"Alpha", "Bravo", "Cobra", "Delta"})},  {20, v({"Alpha", "Bravo", "Cobra", "Delta"})},
      {99, v({"Alpha", "Bravo", "Cobra", "Delta"})},
  };
  for (const Case& c : cases) {
    Ctx cx;
    make_ctx(cx, "gb", games, "<int name=\"gb.sort\" value=\"" + std::to_string(c.id) + "\"/>");
    ListScreen s(0, cx);
    if (order(s, cx) != c.want) {
      std::fprintf(stderr, "sort id %d gave:", c.id);
      for (const std::string& n : order(s, cx)) std::fprintf(stderr, " %s", n.c_str());
      std::fprintf(stderr, "\n");
      CHECK(false);
    }
  }
  // the sort belongs to its system: another system's key does nothing
  Ctx cx;
  make_ctx(cx, "gb", games, "<int name=\"snes.sort\" value=\"1\"/>");
  ListScreen s(0, cx);
  CHECK(order(s, cx) == v({"Alpha", "Bravo", "Cobra", "Delta"}));
}

// equal values keep the name order, in both directions
void test_sort_ties() {
  const std::vector<G> games = {{"Alpha", 50}, {"Bravo", 50}, {"Cobra", 80}, {"Delta", 50}};
  for (int id : {2, 3}) {
    Ctx cx;
    make_ctx(cx, "gb", games, "<int name=\"gb.sort\" value=\"" + std::to_string(id) + "\"/>");
    ListScreen s(0, cx);
    CHECK(order(s, cx) == (id == 2 ? v({"Alpha", "Bravo", "Delta", "Cobra"}) : v({"Cobra", "Alpha", "Bravo", "Delta"})));
  }
}

// FavoritesFirst (default off), per system override "<system>.FavoritesFirst" 1 / 0, never in the favourites collection
void test_favorites_first() {
  const std::vector<G> games = {{"Alpha"}, {"Bravo", 0, 0, 0, -1, 0, true}, {"Cobra"}, {"Delta", 0, 0, 0, -1, 0, true}};
  Ctx off;
  make_ctx(off, "gb", games);
  ListScreen so(0, off);
  CHECK(order(so, off) == v({"Alpha", "Bravo", "Cobra", "Delta"}));
  Ctx on;
  make_ctx(on, "gb", games, "<bool name=\"FavoritesFirst\" value=\"true\"/>");
  ListScreen sn(0, on);
  CHECK(order(sn, on) == v({"Bravo", "Delta", "Alpha", "Cobra"}));
  Ctx per;  // global on, this system off ("0")
  make_ctx(per, "gb", games, "<bool name=\"FavoritesFirst\" value=\"true\"/><string name=\"gb.FavoritesFirst\" value=\"0\"/>");
  ListScreen sp(0, per);
  CHECK(order(sp, per) == v({"Alpha", "Bravo", "Cobra", "Delta"}));
  Ctx per2;  // global off, this system on ("1")
  make_ctx(per2, "gb", games, "<string name=\"gb.FavoritesFirst\" value=\"1\"/>");
  ListScreen sp2(0, per2);
  CHECK(order(sp2, per2) == v({"Bravo", "Delta", "Alpha", "Cobra"}));
  Ctx fav;  // ES: the favourites and recent collections never put favourites first
  make_ctx(fav, "favorites", games, "<bool name=\"FavoritesFirst\" value=\"true\"/>");
  ListScreen sf(0, fav);
  CHECK(order(sf, fav) == v({"Alpha", "Bravo", "Cobra", "Delta"}));
  // it comes before the sort: favourites by rating, then the others by rating
  Ctx srt;
  make_ctx(srt, "gb", {{"Alpha", 90}, {"Bravo", 10, 0, 0, -1, 0, true}, {"Cobra", 50}, {"Delta", 70, 0, 0, -1, 0, true}},
           "<bool name=\"FavoritesFirst\" value=\"true\"/><int name=\"gb.sort\" value=\"3\"/>");
  ListScreen ss(0, srt);
  CHECK(order(ss, srt) == v({"Delta", "Bravo", "Alpha", "Cobra"}));
}

// ShowFilenames: the list shows (and sorts by) the file name without extension instead of the scraped name
void test_show_filenames() {
  Ctx cx;
  make_ctx(cx, "gb", {{"Super Alpha"}, {"Zed Game"}}, "<bool name=\"ShowFilenames\" value=\"true\"/>");
  // the ROM files are called differently from the names
  cx.systems[0].games[0].path = "/roms/gb/zzz-first.gb";
  cx.systems[0].games[1].path = "/roms/gb/aaa-second.gb";
  ListScreen s(0, cx);
  CHECK(order(s, cx) == v({"Zed Game", "Super Alpha"}));  // aaa-second (Zed Game) sorts before zzz-first
  Ctx plain;
  make_ctx(plain, "gb", {{"Super Alpha"}, {"Zed Game"}});
  plain.systems[0].games[0].path = "/roms/gb/zzz-first.gb";
  plain.systems[0].games[1].path = "/roms/gb/aaa-second.gb";
  ListScreen sp(0, plain);
  CHECK(order(sp, plain) == v({"Super Alpha", "Zed Game"}));
}

// the real files: hidden games, leading articles
void write(const std::string& p, const std::string& t) { CHECK(write_atomic(p, t)); }

void test_hidden_and_articles() {
  const std::string dir = g_tmp + "/roms/gb";
  ::mkdir((g_tmp + "/roms").c_str(), 0755);
  ::mkdir(dir.c_str(), 0755);
  for (const char* f : {"The Zelda.gb", "Alpha.gb", "Secret.gb", "An Omega.gb", "Zulu.gb"}) write(dir + "/" + f, "x");
  write(dir + "/gamelist.xml",
        "<gameList><game><path>./Secret.gb</path><name>Secret</name><hidden>true</hidden></game>"
        "<game><path>./The Zelda.gb</path><name>The Zelda</name></game></gameList>");
  auto fresh = [&](const std::string& es_body) {
    auto cx = std::make_unique<Ctx>();
    make_ctx(*cx, "gb", {}, es_body);
    System& s = cx->systems[0];
    s.rom_dir = dir;
    s.exts = {".gb"};
    s.gamelist = dir + "/gamelist.xml";
    set_load_options(LoadOptions{});  // as the loader read it at start, with the defaults
    s.games = load_games(dir, {".gb"}, s.gamelist);
    s.count = s.games.size();
    return cx;
  };
  {  // defaults: hidden left out, articles ignored: Alpha, An Omega (O), The Zelda (Z), Zulu
    std::unique_ptr<Ctx> cx = fresh("");
    ListScreen s(0, *cx);
    CHECK(order(s, *cx) == v({"Alpha", "An Omega", "The Zelda", "Zulu"}));
  }
  {  // SHOW HIDDEN FILES on: the hidden game is in the list (and the system is read again)
    std::unique_ptr<Ctx> cx = fresh("<bool name=\"ShowHiddenFiles\" value=\"true\"/>");
    ListScreen s(0, *cx);
    CHECK(order(s, *cx) == v({"Alpha", "An Omega", "Secret", "The Zelda", "Zulu"}));
  }
  {  // ... and off again: the next list drops it again
    std::unique_ptr<Ctx> cx = fresh("<bool name=\"ShowHiddenFiles\" value=\"false\"/>");
    ListScreen s(0, *cx);
    CHECK(order(s, *cx) == v({"Alpha", "An Omega", "The Zelda", "Zulu"}));
  }
  {  // IGNORE LEADING ARTICLES off: plain name order, "An Omega" and "The Zelda" under their article
    std::unique_ptr<Ctx> cx = fresh("<bool name=\"IgnoreLeadingArticles\" value=\"false\"/>");
    ListScreen s(0, *cx);
    CHECK(order(s, *cx) == v({"Alpha", "An Omega", "The Zelda", "Zulu"}));  // A-l < A-n < T < Z: same here, so use a sharper case below
  }
  {
    write(dir + "/Zebra.gb", "x");
    write(dir + "/The Apple.gb", "x");
    std::unique_ptr<Ctx> on = fresh("");
    ListScreen so(0, *on);
    CHECK(order(so, *on) == v({"Alpha", "The Apple", "An Omega", "Zebra", "The Zelda", "Zulu"}));  // by APPLE, OMEGA, ZEBRA, ZELDA
    std::unique_ptr<Ctx> off = fresh("<bool name=\"IgnoreLeadingArticles\" value=\"false\"/>");
    ListScreen sf(0, *off);
    CHECK(order(sf, *off) == v({"Alpha", "An Omega", "The Apple", "The Zelda", "Zebra", "Zulu"}));
  }
}

// the menu entries: sort + filters in the options screen, the four switches in the UI settings group
Menu* open_menu(std::unique_ptr<Screen>& holder, Ctx& cx, const char* name, const std::string& arg) {
  holder = Registry::instance().make(name, cx, arg);
  if (!holder) return nullptr;
  Menu* m = static_cast<Menu*>(holder.get());
  m->rebuild(cx);
  return m;
}

int find(Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}

void test_options_screen_and_filters() {
  Ctx cx;
  make_ctx(cx, "gb", {{"Alpha", 0, 0, 0, -1, 0, true, "Rpg"}, {"Bravo", 0, 0, 0, -1, 0, false, "Action"}, {"Cobra", 0, 0, 0, -1, 0, true, "action"}});
  std::unique_ptr<Screen> hold;
  Menu* m = open_menu(hold, cx, "menu.list_options", "gb");
  CHECK(m != nullptr);
  if (!m) return;
  CHECK(find(*m, "SORT GAMES BY") >= 0);
  CHECK(find(*m, "FAVORITES ONLY") >= 0);
  CHECK(find(*m, "GENRE") >= 0);
  {  // one entry per genre, shown as it is written in the gamelist ("Action", not "action"); the value stays lower case
    const Entry& ge = m->entry(size_t(find(*m, "GENRE")));
    CHECK_EQ(ge.options.size(), size_t(3));
    if (ge.options.size() == 3) {
      CHECK_EQ(ge.options[1].value, std::string("action"));
      CHECK_EQ(ge.options[1].label, std::string("Action"));
      CHECK_EQ(ge.options[2].label, std::string("Rpg"));
    }
  }
  // the game options reach it
  std::unique_ptr<Screen> go = Registry::instance().make("menu.game_options", cx, "gb\t/roms/gb/Alpha.gb");
  Menu* gm = static_cast<Menu*>(go.get());
  gm->rebuild(cx);
  const int at = find(*gm, "GAMELIST OPTIONS");
  CHECK(at >= 0);
  if (at >= 0) {
    Transition t = gm->entry(size_t(at)).on_a(cx);
    CHECK(t.kind == Transition::Open && t.name == "menu.list_options" && t.arg == "gb");
  }
  // choosing a sort writes ES's key and the open list follows
  ListScreen list(0, cx);
  CHECK(order(list, cx) == v({"Alpha", "Bravo", "Cobra"}));
  const int sorti = find(*m, "SORT GAMES BY");
  CHECK(sorti >= 0);
  m->entry(size_t(sorti)).set_s("1");  // FILENAME, DESCENDING
  CHECK_EQ(cx.es.get_int("gb.sort", -1), 1L);
  CHECK(order(list, cx) == v({"Cobra", "Bravo", "Alpha"}));
  m->entry(size_t(sorti)).set_s("0");
  // favourites only
  m->rebuild(cx);
  m->entry(size_t(find(*m, "FAVORITES ONLY"))).set_b(true);
  CHECK(order(list, cx) == v({"Alpha", "Cobra"}));
  // genre (case-insensitive, one entry for "Action" and "action")
  m->rebuild(cx);
  m->entry(size_t(find(*m, "GENRE"))).set_s("action");
  CHECK(order(list, cx) == v({"Cobra"}));
  m->entry(size_t(find(*m, "FAVORITES ONLY"))).set_b(false);
  CHECK(order(list, cx) == v({"Bravo", "Cobra"}));
  m->entry(size_t(find(*m, "GENRE"))).set_s("");
  CHECK(order(list, cx) == v({"Alpha", "Bravo", "Cobra"}));
  // a filter that leaves nothing must not trap the user: Select on the empty list opens the list options
  m->entry(size_t(find(*m, "GENRE"))).set_s("puzzle");
  CHECK(order(list, cx).empty());
  Transition t = list.handle(Action::Select, cx);
  CHECK(t.kind == Transition::Open && t.name == "menu.list_options" && t.arg == "gb");
  // the options then say which filter is on (not ALL over an empty list)
  m->rebuild(cx);
  CHECK_EQ(m->entry(size_t(find(*m, "GENRE"))).value_text, std::string("puzzle"));
  m->entry(size_t(find(*m, "GENRE"))).set_s("");
  // a collection has genres too: the options of its list offer those of its games
  cx.cols.push_back(cx.systems[0]);
  cx.cols[0].name = "favorites";
  std::unique_ptr<Screen> hold2;
  Menu* cm = open_menu(hold2, cx, "menu.list_options", "favorites");
  CHECK(cm != nullptr);
  if (cm) {
    cm->entry(size_t(find(*cm, "GENRE"))).set_s("rpg");
    cm->rebuild(cx);
    CHECK_EQ(cm->entry(size_t(find(*cm, "GENRE"))).value_text, std::string("Rpg"));  // the gamelist's spelling (the value stays lower case)
    cm->entry(size_t(find(*cm, "GENRE"))).set_s("");
  }
}

void test_ui_group() {
  Ctx cx;
  make_ctx(cx, "gb", {{"Alpha"}});
  std::unique_ptr<Screen> hold;
  Menu* m = open_menu(hold, cx, "menu.ui", "");
  CHECK(m != nullptr);
  if (!m) return;
  for (const char* l : {"GAMELIST OPTIONS", "SHOW FAVORITES ON TOP", "SHOW HIDDEN FILES", "SHOW FILENAMES INSTEAD", "IGNORE LEADING ARTICLES WHEN SORTING", "SHOW VIDEO PREVIEWS"})
    CHECK(find(*m, l) >= 0);
  CHECK(m->entry(size_t(find(*m, "SHOW VIDEO PREVIEWS"))).get_b());  // ES default true
  CHECK_EQ(find(*m, "ENABLE VIDEO PREVIEW AUDIO"), find(*m, "SHOW VIDEO PREVIEWS") + 1);  // ES VideoAudio, next to the previews
  // ES defaults: favourites on top off, hidden off, file names off, articles ignored (on)
  CHECK(!m->entry(size_t(find(*m, "SHOW FAVORITES ON TOP"))).get_b());
  CHECK(!m->entry(size_t(find(*m, "SHOW HIDDEN FILES"))).get_b());
  CHECK(!m->entry(size_t(find(*m, "SHOW FILENAMES INSTEAD"))).get_b());
  CHECK(m->entry(size_t(find(*m, "IGNORE LEADING ARTICLES WHEN SORTING"))).get_b());
  m->entry(size_t(find(*m, "SHOW FAVORITES ON TOP"))).set_b(true);
  const std::string* fv = cx.es.get("FavoritesFirst");
  CHECK(fv && *fv == "true");
  m->entry(size_t(find(*m, "IGNORE LEADING ARTICLES WHEN SORTING"))).set_b(false);
  const std::string* ia = cx.es.get("IgnoreLeadingArticles");
  CHECK(ia && *ia == "false");
  // the library group links to the same rows
  std::unique_ptr<Screen> lib;
  Menu* lm = open_menu(lib, cx, "menu.g.library", "");
  CHECK(lm && find(*lm, "GAMELIST OPTIONS") >= 0);
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-listopts-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  SystemConf::lock_path() = g_tmp + "/lock";
  test_sort_types();
  test_sort_ties();
  test_favorites_first();
  test_show_filenames();
  test_hidden_and_articles();
  test_options_screen_and_filters();
  test_ui_group();
  std::string cmd = "rm -rf '" + g_tmp + "'";
  if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "rm failed\n");
  return zt::done();
}
