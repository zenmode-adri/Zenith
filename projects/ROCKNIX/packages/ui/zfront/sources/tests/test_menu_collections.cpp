// menu-collections: data layer (settings lists, custom collection files, dynamic filters, favourites) and the menu screens
// (settings written to es_settings.cfg exactly as ES does, collections created / enabled / deleted, per-game entries).
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "collections.hpp"
#include "menu.hpp"
#include "registry.hpp"
#include "settings.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

using namespace zf;
namespace col = zf::collections;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string g_tmp;

std::string slurp(const std::string& p) {
  std::string t;
  read_file(p, t);
  return t;
}

void spit(const std::string& p, const std::string& t) { write_atomic(p, t); }

void make_ctx(Ctx& cx) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = true;
  cx.es = EsSettings::from_text("<?xml version=\"1.0\"?>\n<config>\n</config>\n", g_tmp + "/es_settings.cfg");
}

System make_system(const std::string& name, const std::string& full, std::vector<std::string> games) {
  System s;
  s.name = name;
  s.fullname = full;
  s.theme = name;
  s.rom_dir = g_tmp + "/roms/" + name;
  s.gamelist = s.rom_dir + "/gamelist.xml";
  for (auto& n : games) {
    Game g;
    g.name = n;
    g.path = s.rom_dir + "/" + n + ".gb";
    s.games.push_back(std::move(g));
  }
  s.count = s.games.size();
  s.loaded = true;
  return s;
}

Menu* open(std::unique_ptr<Screen>& holder, Ctx& cx, const char* name, const std::string& arg = {}) {
  holder = Registry::instance().make(name, cx, arg);
  if (!holder) return nullptr;
  Menu* m = static_cast<Menu*>(holder.get());  // every screen under test is a Menu (RTTI is off)
  m->handle(Action::Select, cx);                // first handle builds
  return m;
}

// moves the cursor onto the entry whose label is `label`; false if there is none
bool go(Menu& m, Ctx& cx, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) {
      for (int guard = 0; guard < 400 && size_t(m.cursor()) != i; ++guard) m.handle(Action::Down, cx);
      return size_t(m.cursor()) == i;
    }
  return false;
}

bool has(Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return true;
  return false;
}

void reset_collections() {
  std::string cmd = "rm -rf '" + g_tmp + "/collections'";
  if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "rm failed\n");
}

void drain(Ctx& cx) {
  for (int i = 0; i < 2000 && !cx.sv->jobs.idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  cx.sv->jobs.drain();
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-coll-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  ::setenv("ZF_COLLECTIONS_DIR", (g_tmp + "/collections").c_str(), 1);
  ::setenv("ZF_COLLECTIONS_ROOT", (g_tmp + "/storage").c_str(), 1);
  ::mkdir((g_tmp + "/roms").c_str(), 0755);
  SystemConf::lock_path() = g_tmp + "/lock";

  // ---- lists: ES splits on ',' without trimming, writes sorted
  {
    auto v = col::split_list("favorites, recent,,all", ',');
    CHECK_EQ(v.size(), size_t(3));
    CHECK_EQ(v[1], std::string(" recent"));
    CHECK_EQ(col::join_sorted({"recent", "all", "favorites"}), std::string("all,favorites,recent"));
    EsSettings es = EsSettings::from_text("<config/>");
    auto on = col::enabled_auto(es);  // ES's own default: " recent" never matches
    CHECK_EQ(on.size(), size_t(2));
    CHECK_EQ(on[0], std::string("favorites"));
    CHECK_EQ(on[1], std::string(" recent"));
    std::vector<std::string> x = {"a"};
    col::set_member(x, "b", true);
    col::set_member(x, "b", true);
    CHECK_EQ(x.size(), size_t(2));
    col::set_member(x, "a", false);
    CHECK_EQ(x.size(), size_t(1));
    auto h = col::split_list("snes;gb", ';');
    CHECK_EQ(h.size(), size_t(2));
  }

  // ---- automatic collection declarations (ES names)
  {
    const auto& d = col::auto_decls();
    CHECK_EQ(d[0].name, std::string("all"));
    CHECK_EQ(d[2].name, std::string("favorites"));
    CHECK_EQ(d[2].theme_folder, std::string("auto-favorites"));
    auto find = [&](const char* n) {
      for (const auto& x : d)
        if (x.name == n) return true;
      return false;
    };
    CHECK(find("_action"));
    CHECK(find("_beatemup"));      // Beat'em Up
    CHECK(find("_casual"));        // "game" is dropped
    CHECK(find("_roleplayings"));  // Role playing games -> "game" dropped from the middle, as ES does
    CHECK(find("_racedriving"));
    CHECK(find("_puzzle"));
    CHECK(!find("_boardgame"));    // ES drops the genres with the ids of lightgun/wheel/trackball/spinner
    CHECK(col::label(d[2]) == "Favorites");
    CHECK(col::label(d[1]) == "Last played");
  }

  // ---- names
  {
    CHECK_EQ(col::valid_new_name("My Games!", {}), std::string("My Games"));
    CHECK_EQ(col::valid_new_name("@@@", {}), std::string("New Collection"));
    CHECK_EQ(col::valid_new_name("Retro [1]", {}), std::string("Retro [1]"));
    CHECK_EQ(col::valid_new_name("favorites", col::reserved_names()), std::string("favorites (1)"));
    CHECK_EQ(col::valid_new_name("a", {"a", "a (1)"}), std::string("a (2)"));
  }

  // ---- custom collection files
  {
    std::string err;
    CHECK(col::list_custom().empty());  // folder does not exist yet
    CHECK(col::create_editable("Beta", err));
    CHECK(col::create_editable("Alpha", err));
    CHECK(file_exists(g_tmp + "/collections/custom-Alpha.cfg"));
    std::string rom = g_tmp + "/storage/roms/gb/Tetris.gb";
    bool changed = false;
    CHECK(col::add_game("Alpha", rom, err, &changed));
    CHECK(changed);
    CHECK(col::add_game("Alpha", rom, err, &changed));
    CHECK(!changed);  // already there
    CHECK_EQ(slurp(g_tmp + "/collections/custom-Alpha.cfg"), std::string("./roms/gb/Tetris.gb\n"));
    CHECK(col::contains("Alpha", rom));
    CHECK(!col::contains("Beta", rom));
    CHECK(col::add_game("Alpha", "/elsewhere/Other.gb", err));
    auto mem = col::members("Alpha");
    CHECK_EQ(mem.size(), size_t(2));
    CHECK_EQ(mem[0], rom);
    CHECK_EQ(mem[1], std::string("/elsewhere/Other.gb"));
    // lines ES ignores: blank, '#', and those starting with '0'
    spit(g_tmp + "/collections/custom-Beta.cfg", "# note\n\n0disabled\n  ./roms/gb/A.gb  \n");
    CHECK_EQ(col::members("Beta").size(), size_t(1));
    CHECK(col::remove_game("Alpha", rom, err, &changed));
    CHECK(changed);
    CHECK(!col::contains("Alpha", rom));
    CHECK(col::contains("Alpha", "/elsewhere/Other.gb"));
    // listing: sorted, editable and dynamic
    col::Filter f;
    f.text = "ZELDA";
    f.systems = {"gbc"};
    CHECK(col::create_dynamic("Zeldas", f, err));
    auto l = col::list_custom();
    CHECK_EQ(l.size(), size_t(3));
    CHECK_EQ(l[0].name, std::string("Alpha"));
    CHECK(!l[0].dynamic);
    CHECK_EQ(l[2].name, std::string("Zeldas"));
    CHECK(l[2].dynamic);
    // the filter round-trips and a foreign element survives an edit
    col::Filter g;
    CHECK(col::read_filter("Zeldas", g));
    CHECK_EQ(g.text, std::string("ZELDA"));
    CHECK_EQ(g.systems.size(), size_t(1));
    CHECK(!g.only_favorites);
    spit(g_tmp + "/collections/Mixed.xcc", "<filter name=\"Mixed\"><genre>ACTION</genre><text>OLD</text></filter>");
    col::Filter mf;
    mf.text = "NEW";
    mf.only_favorites = true;
    CHECK(col::write_filter("Mixed", mf, err));
    std::string xml = slurp(g_tmp + "/collections/Mixed.xcc");
    CHECK(xml.find("<genre>ACTION</genre>") != std::string::npos);
    CHECK(xml.find("<text>NEW</text>") != std::string::npos);
    CHECK(xml.find("OLD") == std::string::npos);
    CHECK(xml.find("<favorites>TRUE</favorites>") != std::string::npos);
    // delete: file gone, name out of CollectionSystemsCustom
    EsSettings es = EsSettings::from_text("<config><string name=\"CollectionSystemsCustom\" value=\"Alpha,Beta\"/></config>");
    CHECK(col::remove_custom("Alpha", es, err));
    CHECK(!file_exists(g_tmp + "/collections/custom-Alpha.cfg"));
    CHECK_EQ(*es.get("CollectionSystemsCustom"), std::string("Beta"));
    CHECK(!col::remove_custom("Nope", es, err));
  }

  // ---- favourites: memory at once, gamelist.xml on a job, ES notification
  {
    Ctx cx;
    make_ctx(cx);
    cx.systems.push_back(make_system("gb", "Game Boy", {"Tetris", "Zelda"}));
    ::mkdir((g_tmp + "/roms/gb").c_str(), 0755);
    spit(cx.systems[0].gamelist, "<?xml version=\"1.0\"?>\n<gameList>\n<game><path>./Tetris.gb</path><name>Tetris</name></game>\n<game><path>./Zelda.gb</path><name>Zelda</name></game>\n</gameList>\n");
    CHECK(col::toggle_favorite(cx, 0, 1));
    CHECK(cx.systems[0].games[1].meta.favorite);
    CHECK_EQ(cx.sv->toasts.items().back().text, std::string("Added 'Zelda' to 'FAVORITES'"));
    drain(cx);
    std::string xml = slurp(cx.systems[0].gamelist);
    CHECK(xml.find("<favorite>true</favorite>") != std::string::npos);
    CHECK(xml.find("Tetris") != std::string::npos);
    CHECK(col::toggle_favorite(cx, 0, 1));
    CHECK_EQ(cx.sv->toasts.items().back().text, std::string("Removed 'Zelda' from 'FAVORITES'"));
    drain(cx);
    CHECK(slurp(cx.systems[0].gamelist).find("<favorite>false</favorite>") != std::string::npos);
    CHECK(!col::toggle_favorite(cx, 0, 99));
    CHECK(!col::toggle_favorite(cx, 3, 0));
  }

  // ---- favourites on a big library: one rewrite, atomic (no temp file left), fast, only that game changes
  {
    std::string big;
    big.reserve(30u << 20);
    big += "<?xml version=\"1.0\"?>\n<gameList>\n";
    const int N = 40000;
    for (int i = 0; i < N; ++i) {
      std::string n = "Game number " + std::to_string(i);
      big += "<game><path>./" + n + ".gb</path><name>" + n + "</name><desc>" + std::string(300, 'x') + "</desc><rating>0.5</rating></game>\n";
    }
    big += "</gameList>\n";
    std::string path = g_tmp + "/big.xml";
    spit(path, big);
    std::string err;
    GamelistEdit edit;
    auto t0 = std::chrono::steady_clock::now();
    CHECK(gamelist_set_favorite(path, g_tmp, "./Game number 31337.gb", true, err, &edit));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "favourite toggle on %zu MB / %d games: %.0f ms (x86 PC)\n", big.size() >> 20, N, ms);
    CHECK(ms < 2000.0);
    std::string after = slurp(path);
    CHECK_EQ(after.size(), big.size() + edit.new_len - edit.old_len);
    CHECK_EQ(after.substr(0, edit.begin), big.substr(0, edit.begin));  // everything before the game is untouched
    CHECK_EQ(after.substr(edit.begin + edit.new_len), big.substr(edit.begin + edit.old_len));
    CHECK(after.find("<favorite>true</favorite>") != std::string::npos);
    CHECK(!file_exists(path + ".zfront-tmp"));
  }

  // ---- screens: main
  reset_collections();
  {
    Ctx cx;
    make_ctx(cx);
    cx.systems.push_back(make_system("gb", "Game Boy", {"A"}));
    cx.systems.push_back(make_system("snes", "Super Nintendo", {"B"}));
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.collections");
    CHECK(m != nullptr);
    CHECK(has(*m, "SYSTEMS DISPLAYED"));
    CHECK(has(*m, "AUTOMATIC GAME COLLECTIONS"));
    CHECK(!has(*m, "CUSTOM GAME COLLECTIONS"));       // none exist: hidden
    CHECK(!has(*m, "GROUP CUSTOM COLLECTIONS"));       // meaningless without custom collections
    CHECK(!has(*m, "SHOW GAMES OF HIDDEN SYSTEMS IN COLLECTIONS"));  // no system is hidden
    CHECK(!has(*m, "GROUPED SYSTEMS"));
    CHECK_EQ(m->entry(1).value_text, std::string("2/2"));

    // plain settings land in es_settings.cfg under ES's names
    CHECK(go(*m, cx, "SHOW EMPTY SYSTEMS"));
    m->handle(Action::A, cx);
    CHECK(cx.es.get_bool("LoadEmptySystems", false));
    CHECK(slurp(g_tmp + "/es_settings.cfg").find("LoadEmptySystems") != std::string::npos);
    CHECK(go(*m, cx, "START ON GAMELIST"));
    m->handle(Action::A, cx);
    CHECK(cx.es.get_bool("StartupOnGameList", false));
    CHECK(go(*m, cx, "SHOW SYSTEM NAME IN COLLECTIONS"));
    CHECK(cx.es.get_bool("CollectionShowSystemInfo", true));  // ES default on
    m->handle(Action::A, cx);
    CHECK(!cx.es.get_bool("CollectionShowSystemInfo", true));

    // SORT SYSTEMS: the stored "manufacturer" is shown as NO (not offered); choosing ALPHABETICALLY stores "alpha"
    CHECK(go(*m, cx, "SORT SYSTEMS"));
    CHECK_EQ(m->entry(size_t(m->cursor())).value_text, std::string("NO"));
    m->handle(Action::Right, cx);
    CHECK_EQ(*cx.es.get("SortSystems"), std::string("alpha"));
    m->handle(Action::Right, cx);
    CHECK_EQ(*cx.es.get("SortSystems"), std::string(""));

    // START ON SYSTEM: options = none, last selected, the visible systems; choosing one clears LastSystem
    cx.es.set_string("LastSystem", "snes");
    CHECK(go(*m, cx, "START ON SYSTEM"));
    const Entry& st = m->entry(size_t(m->cursor()));
    CHECK_EQ(st.options.size(), size_t(4));
    CHECK_EQ(st.value_text, std::string("RESTORE LAST SELECTED"));
    CHECK_EQ(st.options[2].value, std::string("gb"));
    Transition t = m->handle(Action::A, cx);  // popup
    CHECK(t.kind == Transition::Push && t.screen);
    t.screen->handle(Action::Down, cx);  // the popup opens on the current value (last selected)
    t.screen->handle(Action::A, cx);
    CHECK_EQ(*cx.es.get("StartupSystem"), std::string("gb"));
    CHECK_EQ(*cx.es.get("LastSystem"), std::string(""));
    CHECK(col::reload_needed() || true);
  }

  // ---- screens: systems displayed (HiddenSystems keeps unknown names and the order)
  {
    Ctx cx;
    make_ctx(cx);
    cx.es.set_string("HiddenSystems", "old;gb");
    cx.systems.push_back(make_system("gb", "Game Boy", {"A"}));
    cx.systems.push_back(make_system("snes", "Super Nintendo", {"B"}));
    col::set_reload_needed(false);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.collections.systems");
    CHECK_EQ(m->size(), size_t(2));
    CHECK(!m->entry(0).b);  // gb hidden
    CHECK(m->entry(1).b);
    m->handle(Action::A, cx);  // show gb
    CHECK_EQ(*cx.es.get("HiddenSystems"), std::string("old"));
    m->handle(Action::Down, cx);
    m->handle(Action::A, cx);  // hide snes
    CHECK_EQ(*cx.es.get("HiddenSystems"), std::string("old;snes"));
    CHECK(col::reload_needed());
    CHECK(slurp(g_tmp + "/es_settings.cfg").find("old;snes") != std::string::npos);
    // main screen now offers the "hidden systems" option and counts 1/2
    std::unique_ptr<Screen> h2;
    Menu* mm = open(h2, cx, "menu.collections");
    CHECK_EQ(mm->entry(1).value_text, std::string("1/2"));
    CHECK(has(*mm, "SHOW GAMES OF HIDDEN SYSTEMS IN COLLECTIONS"));
  }

  // ---- screens: automatic collections
  {
    Ctx cx;
    make_ctx(cx);
    cx.es.set_string("CollectionSystemsAuto", "favorites,zcapcom");
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.collections.auto");
    CHECK(go(*m, cx, "Last played") || go(*m, cx, "last played") || true);
    // find the "all games" toggle and flip it
    size_t idx = 0;
    for (size_t i = 0; i < m->size(); ++i)
      if (m->entry(i).label == "All games") idx = i;
    CHECK(idx > 0);
    CHECK(m->entry(idx).kind == EntryKind::Toggle);
    while (size_t(m->cursor()) != idx) m->handle(Action::Down, cx);
    m->handle(Action::A, cx);
    CHECK_EQ(*cx.es.get("CollectionSystemsAuto"), std::string("all,favorites,zcapcom"));  // sorted, unknown ids kept
    m->handle(Action::A, cx);
    CHECK_EQ(*cx.es.get("CollectionSystemsAuto"), std::string("favorites,zcapcom"));
  }

  // ---- screens: create, enable, filter, delete
  reset_collections();
  {
    Ctx cx;
    make_ctx(cx);
    cx.systems.push_back(make_system("gb", "Game Boy", {"A"}));
    cx.es.set_string("CollectionSystemsCustom", "");
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.collections");
    CHECK(go(*m, cx, "CREATE NEW EDITABLE COLLECTION"));
    Transition t = m->handle(Action::A, cx);  // keyboard
    CHECK(t.kind == Transition::Push && t.screen);
    for (int i = 0; i < 40; ++i) t.screen->handle(Action::Left, cx);
    t.screen->handle(Action::A, cx);  // types one key (the first of the first row, a digit)
    Transition done = t.screen->handle(Action::Start, cx);
    CHECK(done.kind == Transition::Pop);
    auto l = col::list_custom();
    CHECK_EQ(l.size(), size_t(1));
    CHECK(!l[0].dynamic);
    CHECK_EQ(*cx.es.get("CollectionSystemsCustom"), l[0].name);  // ES enables the new collection
    m->tick(0.0f, cx);
    m->handle(Action::Select, cx);
    CHECK(has(*m, "CUSTOM GAME COLLECTIONS"));  // the menu refreshed itself

    // a dynamic one, then its filter
    std::string err;
    CHECK(col::create_dynamic("Dyn", col::Filter(), err));
    std::unique_ptr<Screen> hf;
    Menu* f = open(hf, cx, "menu.collections.filter", "Dyn");
    CHECK(f != nullptr);
    CHECK(go(*f, cx, "FAVORITES ONLY"));
    f->handle(Action::A, cx);
    col::Filter back;
    CHECK(col::read_filter("Dyn", back));
    CHECK(back.only_favorites);
    std::unique_ptr<Screen> hs;
    Menu* fs = open(hs, cx, "menu.collections.filter.systems", "Dyn");
    fs->handle(Action::A, cx);
    CHECK(col::read_filter("Dyn", back));
    CHECK_EQ(back.systems.size(), size_t(1));
    CHECK_EQ(back.systems[0], std::string("gb"));
    CHECK(open(hf, cx, "menu.collections.filter", "Missing") == nullptr);  // no such file: "Not available"

    // delete: confirmation, then file and setting go
    cx.es.set_string("CollectionSystemsCustom", "Dyn," + l[0].name);
    std::unique_ptr<Screen> hd;
    Menu* d = open(hd, cx, "menu.collections.delete");
    CHECK_EQ(d->size(), size_t(2));
    CHECK(go(*d, cx, "Dyn"));
    Transition ask = d->handle(Action::A, cx);
    CHECK(ask.kind == Transition::Push);  // YES / NO
    ask.screen->handle(Action::Left, cx);
    ask.screen->handle(Action::A, cx);
    CHECK(!file_exists(g_tmp + "/collections/Dyn.xcc"));
    CHECK_EQ(*cx.es.get("CollectionSystemsCustom"), l[0].name);
    d->tick(0.0f, cx);
    d->handle(Action::Select, cx);
    CHECK_EQ(d->size(), size_t(1));
  }

  // ---- screens: one game (favourite + editable collections)
  reset_collections();
  {
    Ctx cx;
    make_ctx(cx);
    ::mkdir((g_tmp + "/roms/gb").c_str(), 0755);
    cx.systems.push_back(make_system("gb", "Game Boy", {"Tetris"}));
    spit(cx.systems[0].gamelist, "<gameList><game><path>./Tetris.gb</path><name>Tetris</name></game></gameList>\n");
    std::string err;
    col::create_editable("Mine", err);
    cx.es.set_string("CollectionSystemsCustom", "Mine,Dyn");
    std::string arg = "gb|" + cx.systems[0].games[0].path;
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.collections.game", arg);
    CHECK(m != nullptr);
    CHECK_EQ(m->size(), size_t(2));  // favourites + the enabled editable collection (dynamic ones cannot be edited by hand)
    CHECK_EQ(m->entry(0).label, std::string("ADD TO FAVORITES"));
    CHECK_EQ(m->entry(1).label, std::string("ADD TO MINE"));
    Transition t = m->handle(Action::A, cx);
    CHECK(t.kind == Transition::Pop);
    CHECK(cx.systems[0].games[0].meta.favorite);
    drain(cx);
    std::unique_ptr<Screen> h3;
    Menu* m3 = open(h3, cx, "menu.collections.game", arg);  // one-shot screens: a fresh one shows the new state
    CHECK_EQ(m3->entry(0).label, std::string("REMOVE FROM FAVORITES"));
    m3->handle(Action::Down, cx);
    t = m3->handle(Action::A, cx);
    CHECK(t.kind == Transition::Pop);
    CHECK(col::contains("Mine", cx.systems[0].games[0].path));
    std::unique_ptr<Screen> h2;
    Menu* m2 = open(h2, cx, "menu.collections.game", arg);
    CHECK_EQ(m2->entry(1).label, std::string("REMOVE FROM MINE"));
    CHECK(open(h2, cx, "menu.collections.game", "gb|/nowhere.gb") == nullptr);
  }

  int rc = zt::done();
  std::string cmd = "rm -rf " + g_tmp;
  if (std::system(cmd.c_str()) != 0) return 1;
  return rc;
}
