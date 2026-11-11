// EDIT THIS GAME'S METADATA, SCRAPE (one game) and DELETE GAME: the gamelist layer (set fields, read fields, remove an element), the
// edit and delete flows over temp folders (memory at once, files and gamelist on the job thread, refusals), and the rows and screens of the
// game options. Temp dirs only.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "collections.hpp"
#include "gamelist_game.hpp"
#include "gamemeta.hpp"
#include "i18n.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "menu_game_edit.hpp"
#include "registry.hpp"
#include "scraper_run.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;

namespace {

std::string tmpdir(const char* tag) {
  std::string d = "/tmp/zf_test_game_edit_" + std::string(tag) + "_" + std::to_string(getpid());
  std::string cmd = "rm -rf " + d + " && mkdir -p " + d;
  CHECK(std::system(cmd.c_str()) == 0);
  return d;
}
void spit(const std::string& p, const std::string& s) {
  FILE* f = std::fopen(p.c_str(), "wb");
  CHECK(f != nullptr);
  if (f) {
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
  }
}
std::string slurp(const std::string& p) {
  std::string s;
  read_file(p, s);
  return s;
}
bool exists(const std::string& p) {
  struct stat st;
  return ::lstat(p.c_str(), &st) == 0;
}
bool has(const std::string& hay, const std::string& n) { return hay.find(n) != std::string::npos; }
void settle(Ctx& cx) {
  while (!cx.sv->jobs.idle()) {
    cx.sv->jobs.drain();
    usleep(1000);
  }
  cx.sv->jobs.drain();
}

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

// three games g0..g2.gb in <dir>; g1 has a developer, an image and a video
struct Fixture {
  std::string dir, xml;
  Ctx cx;
  explicit Fixture(const char* tag) {
    dir = tmpdir(tag);
    xml = dir + "/gamelist.xml";
    std::string x = "<?xml version=\"1.0\"?>\n<gameList>\n";
    for (int i = 0; i < 3; ++i) {
      std::string n = std::to_string(i);
      spit(dir + "/g" + n + ".gb", "rom" + n);
      x += "\t<game>\n\t\t<path>./g" + n + ".gb</path>\n\t\t<name>G" + n + "</name>\n\t\t<desc>d" + n + "</desc>\n";
      if (i == 1) x += "\t\t<developer>Dev &amp; Co</developer>\n\t\t<image>./images/g1.png</image>\n\t\t<video>./videos/g1.mp4</video>\n";
      x += "\t</game>\n";
    }
    x += "</gameList>\n";
    spit(xml, x);
    CHECK(std::system(("mkdir -p " + dir + "/images " + dir + "/videos").c_str()) == 0);
    spit(dir + "/images/g1.png", "png");
    spit(dir + "/videos/g1.mp4", "mp4");
    System s;
    s.name = "gb";
    s.fullname = "Game Boy";
    s.rom_dir = dir;
    s.gamelist = xml;
    s.exts = {".gb"};
    cx.systems.push_back(std::move(s));
    cx.open_system(0);
    std::string err;
    CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
    cx.w = 640;
    cx.h = 480;
  }
  ~Fixture() {
    std::string cmd = "rm -rf " + dir;
    if (std::system(cmd.c_str())) {
    }
  }
  std::string rom(int i) const { return dir + "/g" + std::to_string(i) + ".gb"; }
  System& sys() { return cx.systems[0]; }
  void check_spans() {
    std::string t = slurp(xml);
    for (Game& g : sys().games) CHECK_EQ(t.compare(g.meta.span_begin, 5, "<game"), 0);
  }
};

Menu& as_menu(std::unique_ptr<Screen>& s) { return *static_cast<Menu*>(s.get()); }
int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}

}  // namespace

int main() {
  // ---- gamelist layer
  {
    Fixture f("gl");
    GameMeta none;
    auto r = gamelist_read_fields(f.xml, f.dir, f.rom(1), f.sys().games[1].meta, {"developer", "desc", "publisher", "image"});
    CHECK_EQ(r["developer"], std::string("Dev & Co"));  // entities decoded
    CHECK_EQ(r["desc"], std::string("d1"));
    CHECK(r.find("publisher") == r.end());
    CHECK_EQ(r["image"], std::string("./images/g1.png"));
    CHECK(gamelist_read_fields(f.xml, f.dir, f.dir + "/nope.gb", none, {"desc"}).empty());

    std::string err;
    GamelistEdit ed;
    CHECK(gamelist_set_fields(f.xml, f.dir, f.rom(0), {{"publisher", "Pub <1>"}, {"name", "Zero"}}, err, &ed));
    f.sys().apply_edit(ed);
    f.check_spans();
    std::string t = slurp(f.xml);
    CHECK(has(t, "<name>Zero</name>"));
    CHECK(has(t, "<publisher>Pub &lt;1&gt;</publisher>"));
    CHECK(has(t, "<developer>Dev &amp; Co</developer>"));  // the rest stays

    // remove: the element, its indentation and its line break, nothing else; the spans of the others keep working
    CHECK(gamelist_remove_game(f.xml, f.dir, f.rom(1), err, &ed));
    f.sys().games.erase(f.sys().games.begin() + 1);
    f.sys().apply_edit(ed);
    t = slurp(f.xml);
    CHECK(!has(t, "g1.gb"));
    CHECK(!has(t, "Dev"));
    CHECK(has(t, "\t</game>\n\t<game>\n\t\t<path>./g2.gb</path>"));  // no blank line left
    f.check_spans();
    GamelistEdit e2;
    CHECK(gamelist_remove_game(f.xml, f.dir, f.rom(1), err, &e2));  // already gone: not an error, nothing changes
    CHECK_EQ(e2.old_len, uint64_t(0));
    CHECK_EQ(slurp(f.xml), t);
    // a malformed file is never touched
    spit(f.dir + "/bad.xml", "<gameList><game><path>./g0.gb</path>");
    CHECK(!gamelist_remove_game(f.dir + "/bad.xml", f.dir, f.rom(0), err, nullptr));
    CHECK_EQ(slurp(f.dir + "/bad.xml"), std::string("<gameList><game><path>./g0.gb</path>"));
  }

  // ---- edit: form read, only the changed fields written, memory at once, hidden games leave the lists
  {
    Fixture f("edit");
    GameForm a;
    CHECK(game_form_load(f.cx, 0, f.rom(1), a));
    CHECK_EQ(a.name, std::string("G1"));
    CHECK_EQ(a.developer, std::string("Dev & Co"));
    CHECK(a.releasedate.empty());
    CHECK(!game_form_load(f.cx, 0, f.dir + "/nope.gb", a));
    CHECK(valid_release_date("1991-08-23") && valid_release_date("") && !valid_release_date("1991-13-01") && !valid_release_date("91-8-23") && !valid_release_date("1991/08/23"));

    GameForm before;
    CHECK(game_form_load(f.cx, 0, f.rom(2), before));
    GameForm after = before;
    after.name = "Two";
    after.rating = 7;
    after.releasedate = "1991-08-23";
    after.genre = "Puzzle";
    after.players = "2";
    after.hidden = true;
    unsigned gen = games_generation();
    game_edit(f.cx, 0, f.rom(2), before, after);
    CHECK(games_generation() != gen);
    Game& g = f.sys().games[2];
    CHECK_EQ(g.name, std::string("Two"));  // memory before the job ran
    CHECK_EQ(int(g.meta.rating_pct), 70);
    CHECK_EQ(int(g.meta.release_year), 1991);
    CHECK(g.meta.hidden);
    settle(f.cx);
    std::string t = slurp(f.xml);
    CHECK(has(t, "<name>Two</name>"));
    CHECK(has(t, "<rating>0.700000</rating>"));
    CHECK(has(t, "<releasedate>19910823T000000</releasedate>"));
    CHECK(has(t, "<genre>Puzzle</genre>") && has(t, "<players>2</players>") && has(t, "<hidden>true</hidden>"));
    CHECK(!has(t, "<favorite>"));  // not changed: not written
    CHECK(!has(t, "<developer></developer>"));
    f.check_spans();
    GameForm back;
    CHECK(game_form_load(f.cx, 0, f.rom(2), back));
    CHECK(back == after);
    // the other games and the same game with no change are left alone
    std::string before_text = slurp(f.xml);
    game_edit(f.cx, 0, f.rom(2), back, back);
    settle(f.cx);
    CHECK_EQ(slurp(f.xml), before_text);
  }

  // ---- fix4 z4 #2: the batch edit (splice_multi) writes nothing in preview / dry-run
  {
    Fixture f("blocked");
    const std::string before_text = slurp(f.xml);
    GameFieldEdit e;
    e.rom = f.rom(0);
    e.fields = {{"name", "Changed"}};
    std::string err;
    GamelistBatchResult r;
    set_writes_blocked(true);
    bool ok = gamelist_edit_fields(f.xml, f.dir, {e}, err, &r);
    set_writes_blocked(false);
    CHECK(ok);
    CHECK_EQ(slurp(f.xml), before_text);
    for (const auto& n : std::vector<std::string>{"/gamelist.xml.zfront-tmp"}) CHECK(!exists(f.dir + n));
    // and unblocked it does write
    CHECK(gamelist_edit_fields(f.xml, f.dir, {e}, err, &r));
    CHECK(has(slurp(f.xml), "<name>Changed</name>"));
  }

  // ---- fix4 z1 #4: SORT NAME and KIDGAME are in the form, written and read back
  {
    Fixture f("kid");
    GameForm before;
    CHECK(game_form_load(f.cx, 0, f.rom(0), before));
    CHECK(before.sortname.empty() && !before.kidgame);
    GameForm after = before;
    after.sortname = "Zed";
    after.kidgame = true;
    game_edit(f.cx, 0, f.rom(0), before, after);
    CHECK(f.sys().games[0].meta.kid);
    settle(f.cx);
    std::string t = slurp(f.xml);
    CHECK(has(t, "<sortname>Zed</sortname>") && has(t, "<kidgame>true</kidgame>"));
    GameForm back;
    CHECK(game_form_load(f.cx, 0, f.rom(0), back));
    CHECK_EQ(back.sortname, std::string("Zed"));
    CHECK(back.kidgame);
    CHECK(back == after);
    GameForm same = after;
    same.kidgame = false;  // the comparison sees each of the two fields
    CHECK(same != after);
    same = after;
    same.sortname = "Y";
    CHECK(same != after);
  }

  // ---- fix4 z4 #6: a game with no <game> element gets its span at once (its description is readable without a reload)
  {
    Fixture f("newspan");
    spit(f.dir + "/g3.gb", "rom3");
    f.sys().games = load_games(f.dir, {".gb"}, f.xml);
    std::string rom = f.rom(3);
    size_t gi = 99;
    for (size_t i = 0; i < f.sys().games.size(); ++i)
      if (f.sys().games[i].path == rom) gi = i;
    CHECK(gi != size_t(99));
    CHECK_EQ(f.sys().games[gi].meta.span_end, 0u);
    GameForm before, after;
    CHECK(game_form_load(f.cx, 0, rom, before));
    after = before;
    after.desc = "Fresh description";
    game_edit(f.cx, 0, rom, before, after);
    settle(f.cx);
    Game& g = f.sys().games[gi];
    CHECK(g.meta.span_end > g.meta.span_begin);
    CHECK_EQ(slurp(f.xml).compare(g.meta.span_begin, 5, "<game"), 0);
    CHECK_EQ(gamelist_details(f.xml, f.dir, rom, g.meta).desc, std::string("Fresh description"));
  }

  // ---- fix4 z4 #5: when the write fails the list goes back to what is on disk
  {
    Fixture f("failw");
    f.sys().gamelist = f.dir + "/nodir/gamelist.xml";  // the folder is not there: the write cannot succeed
    GameForm before, after;
    CHECK(game_form_load(f.cx, 0, f.rom(0), before));
    after = before;
    after.name = "Renamed";
    after.rating = 8;
    after.favorite = true;
    after.kidgame = true;
    game_edit(f.cx, 0, f.rom(0), before, after);
    CHECK_EQ(f.sys().games[0].name, std::string("Renamed"));  // at once
    settle(f.cx);
    const Game& g = f.sys().games[0];
    CHECK_EQ(g.name, before.name);  // and back
    CHECK_EQ(int(g.meta.rating_pct), 0);
    CHECK(!g.meta.favorite && !g.meta.kid);
  }

  // ---- delete: ROM and media inside the folder go, the entry goes, the others stay; files outside stay
  {
    Fixture f("del");
    std::string outside = tmpdir("outside");
    spit(outside + "/precious.png", "x");
    // g1's image points outside the ROM folder: it must survive
    std::string x = slurp(f.xml);
    size_t at = x.find("./images/g1.png");
    x.replace(at, std::string("./images/g1.png").size(), outside + "/precious.png");
    spit(f.xml, x);
    f.sys().games.clear();
    f.cx.systems[0].loaded = false;
    f.cx.open_system(0);
    CHECK_EQ(f.sys().games.size(), size_t(3));
    unsigned gen = games_generation();
    std::string why;
    CHECK(game_delete(f.cx, 0, f.rom(1), why) == DeleteResult::Ok);
    CHECK(games_generation() != gen);
    CHECK_EQ(f.sys().games.size(), size_t(2));  // out of the list at once
    settle(f.cx);
    CHECK(!exists(f.rom(1)));
    CHECK(!exists(f.dir + "/videos/g1.mp4"));
    CHECK(exists(outside + "/precious.png"));
    CHECK(exists(f.rom(0)) && exists(f.rom(2)));
    CHECK(!has(slurp(f.xml), "g1.gb"));
    CHECK(has(slurp(f.xml), "g0.gb") && has(slurp(f.xml), "g2.gb"));
    f.check_spans();
    // a ROM that is already gone: the entry is cleaned up
    CHECK(::unlink(f.rom(2).c_str()) == 0);
    CHECK(game_delete(f.cx, 0, f.rom(2), why) == DeleteResult::Ok);
    settle(f.cx);
    CHECK(!has(slurp(f.xml), "g2.gb"));
    std::string cmd = "rm -rf " + outside;
    if (std::system(cmd.c_str())) {
    }
  }

  // ---- delete: a picture another game also uses stays; a list that cannot be changed keeps the game's files
  {
    Fixture f("del_shared");
    std::string x = slurp(f.xml);
    size_t at = x.find("<name>G2</name>");
    x.insert(at, "<image>./images/g1.png</image>\n\t\t");
    spit(f.xml, x);
    f.sys().games.clear();
    f.cx.systems[0].loaded = false;
    f.cx.open_system(0);
    std::string why;
    CHECK(game_delete(f.cx, 0, f.rom(1), why) == DeleteResult::Ok);
    settle(f.cx);
    CHECK(!exists(f.rom(1)));
    CHECK(!exists(f.dir + "/videos/g1.mp4"));
    CHECK(exists(f.dir + "/images/g1.png"));  // g2 still shows it
    CHECK(!has(slurp(f.xml), "g1.gb") && has(slurp(f.xml), "g2.gb"));
  }
  {
    Fixture f("del_badlist");
    std::string x = slurp(f.xml);
    x = x.substr(0, x.rfind("</gameList>")) + "<game><path>./zz.gb</path>";  // cut in the middle of an entry: malformed, not modified
    spit(f.xml, x);
    const std::string before = slurp(f.xml);
    std::string why;
    CHECK(game_delete(f.cx, 0, f.rom(1), why) == DeleteResult::Ok);
    settle(f.cx);
    CHECK(exists(f.rom(1)));
    CHECK(exists(f.dir + "/images/g1.png") && exists(f.dir + "/videos/g1.mp4"));
    CHECK(slurp(f.xml) == before);
    CHECK(collections::rescan_needed());
    collections::set_rescan_needed(false);
  }

  // ---- delete: refusals change nothing
  {
    Fixture f("refuse");
    std::string outside = tmpdir("refuse_out");
    spit(outside + "/o.gb", "x");
    CHECK(::symlink(outside.c_str(), (f.dir + "/link").c_str()) == 0);
    CHECK(::mkdir((f.dir + "/folder.gb").c_str(), 0755) == 0);
    spit(f.dir + "/folder.gb/inner.bin", "x");
    // entries that exist in memory but whose paths are wrong
    auto add = [&](const std::string& path) {
      Game g;
      g.name = "x";
      g.path = path;
      f.sys().games.push_back(g);
    };
    add(outside + "/o.gb");                 // not inside the ROM folder
    add(f.dir + "/../" + "zz.gb");          // ..
    add(f.dir + "/link/o.gb");              // inside by name, outside through a symlinked folder
    add(f.dir + "/folder.gb");              // a directory
    add(f.dir);                             // the ROM folder itself
    const std::string before = slurp(f.xml);
    const size_t n = f.sys().games.size();
    std::string why;
    for (size_t i = 3; i < n; ++i) {
      std::string p = f.sys().games[i].path;
      CHECK(game_delete(f.cx, 0, p, why) == DeleteResult::Refused);
      CHECK(!why.empty());
    }
    CHECK(game_delete(f.cx, 0, f.dir + "/g0.gb.missing", why) == DeleteResult::Refused);  // not a game of the system
    CHECK(game_delete(f.cx, 7, f.rom(0), why) == DeleteResult::Refused);                   // no such system
    settle(f.cx);
    CHECK_EQ(f.sys().games.size(), n);  // nobody left the list
    CHECK(exists(outside + "/o.gb"));
    CHECK(exists(f.dir + "/folder.gb/inner.bin"));
    CHECK_EQ(slurp(f.xml), before);
    // a system with no ROM folder, and "/"
    f.sys().rom_dir = "";
    CHECK(game_delete(f.cx, 0, f.rom(0), why) == DeleteResult::Refused);
    f.sys().rom_dir = "/";
    CHECK(game_delete(f.cx, 0, f.rom(0), why) == DeleteResult::Refused);
    CHECK(exists(f.rom(0)));
    std::string cmd = "rm -rf " + outside;
    if (std::system(cmd.c_str())) {
    }
  }

  // ---- screens: the rows of the options and of the editor, the draft, SAVE CHANGES?
  {
    Fixture f("menu");
    const std::string arg = mg::game_arg("gb", f.rom(0));
    {
      std::unique_ptr<Screen> s = Registry::instance().make("menu.game_options", f.cx, arg);
      CHECK(s != nullptr);
      as_menu(s).rebuild(f.cx);
      CHECK(find(as_menu(s), tr("EDIT THIS GAME'S METADATA")) >= 0);
      int del = find(as_menu(s), tr("DELETE GAME"));
      CHECK(del >= 0);
      CHECK(as_menu(s).entry(size_t(del)).is_danger);
      CHECK_EQ(as_menu(s).entry(size_t(del)).confirm_msg, std::string(tr("THIS WILL DELETE THE ACTUAL GAME FILE(S)!\nARE YOU SURE?")));
    }
    {
      std::unique_ptr<Screen> s = Registry::instance().make("menu.game.edit", f.cx, arg);
      CHECK(s != nullptr);
      Menu& m = as_menu(s);
      m.rebuild(f.cx);
      const int name = find(m, std::string(tr("Name")));
      CHECK(name >= 0);
      CHECK_EQ(m.entry(size_t(name)).value_text, std::string("G0"));
      for (const char* k : {"Description", "Genre", "Developer", "Publisher", "Players", "Release date", "Rating", "Favorite", "Hidden", "Sort name", "Kidgame"}) CHECK(find(m, std::string(tr(k))) >= 0);
      CHECK(find(m, tr("SAVE")) >= 0);
      CHECK(find(m, tr("DELETE")) >= 0);
      // B on a clean draft closes; on a changed one it asks first
      CHECK(m.handle(Action::B, f.cx).kind == Transition::Pop);
      int fav = find(m, std::string(tr("Favorite")));
      m.entry(size_t(fav)).set_b(true);
      Transition t = m.handle(Action::B, f.cx);
      CHECK(t.kind == Transition::Push);  // SAVE CHANGES?
      // Start on a changed draft asks the same question, and whatever the answer, every menu closes (B only closes this one)
      Transition ts = m.handle(Action::Start, f.cx);
      CHECK(ts.kind == Transition::Push);
      CHECK(ts.screen != nullptr);
      if (ts.screen) {
        ts.screen->handle(Action::Down, f.cx);  // YES -> NO
        ts.screen->handle(Action::A, f.cx);
      }
      CHECK(m.take_transition(f.cx).kind == Transition::CloseMenus);
      CHECK(m.take_transition(f.cx).kind == Transition::None);
      if (t.screen) {  // B: only this menu
        t.screen->handle(Action::Down, f.cx);
        t.screen->handle(Action::A, f.cx);
        CHECK(m.take_transition(f.cx).kind == Transition::Pop);
      }
      // SAVE writes only the favourite flag
      int save = find(m, tr("SAVE"));
      for (size_t i = 0; i < m.size() + 1 && m.cursor() != save; ++i) m.handle(Action::Down, f.cx);
      Transition st = m.handle(Action::A, f.cx);
      CHECK(st.kind == Transition::Pop);
      settle(f.cx);
      CHECK(has(slurp(f.xml), "<favorite>true</favorite>"));
      CHECK(f.sys().games[0].meta.favorite);
    }
  }
  return zt::done();
}
