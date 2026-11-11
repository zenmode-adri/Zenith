// LIBRARY > CHECK LIBRARY: the read-only scan (empty files, fake archives, duplicates by stem, games without a picture), the report menu,
// and that nothing in the ROM tree is created, removed or touched.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "app.hpp"
#include "check.hpp"
#include "library_doctor.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "registry.hpp"
#include "systems.hpp"
#include "util.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

void spit(const fs::path& p, const std::string& t) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << t;
}

// path -> "size:mtime" of every entry under `root` (directories included)
std::map<std::string, std::string> snapshot(const fs::path& root) {
  std::map<std::string, std::string> m;
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    struct stat st;
    if (::stat(e.path().c_str(), &st) != 0) continue;
    m[e.path().string()] = std::to_string(st.st_size) + ":" + std::to_string(st.st_mtim.tv_sec) + "." + std::to_string(st.st_mtim.tv_nsec);
  }
  return m;
}

int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}

void theme(Ctx& cx) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640;
  cx.h = 480;
  cx.sv->cmd.dry_run = true;
}

// Opens the screen the way the app does: the first build is the loading state, the first key runs on_open (synchronous in dry-run).
std::unique_ptr<Screen> open_report(Ctx& cx) {
  std::unique_ptr<Screen> s = Registry::instance().make("menu.library_doctor", cx, "");
  CHECK(s != nullptr);
  if (!s) return s;
  Menu* m = static_cast<Menu*>(s.get());
  m->rebuild(cx);
  CHECK(find(*m, "PLEASE WAIT") >= 0);
  m->handle(Action::Down, cx);
  m->rebuild(cx);
  CHECK(find(*m, "PLEASE WAIT") < 0);
  return s;
}

}  // namespace

int main() {
  // ---- the comparison name
  CHECK_EQ(doctor::dup_stem("Super Mario (USA).gb"), std::string("super mario"));
  CHECK_EQ(doctor::dup_stem("Super  Mario (Europe) [!].GB"), std::string("super mario"));
  CHECK_EQ(doctor::dup_stem("Zelda (Disc 1).cue"), std::string("zelda (disc 1)"));
  CHECK(doctor::dup_stem("Zelda (Disc 1).cue") != doctor::dup_stem("Zelda (Disc 2).cue"));
  CHECK(!doctor::dup_stem("(USA).gb").empty());              // all tags: still something to compare

  char tmpl[] = "/tmp/zf-doctor-XXXXXX";
  const fs::path dir = ::mkdtemp(tmpl);
  const fs::path roms = dir / "roms/gb";
  const std::string zip_empty_archive = std::string("PK\x05\x06") + std::string(18, '\0');
  const std::string sevenz = std::string("7z\xBC\xAF\x27\x1C\0\x04", 8);
  spit(roms / "empty.gb", "");
  spit(roms / "fake.zip", "this is not a zip file at all");
  spit(roms / "real.zip", zip_empty_archive);
  spit(roms / "good.7z", sevenz);
  spit(roms / "bad.7z", "PK\x03\x04 a zip with a 7z name");
  spit(roms / "Super Mario (USA).gb", "rom-a");
  spit(roms / "Super Mario (Europe) [!].gb", "rom-b");
  spit(roms / "Zelda (Disc 1).gb", "rom-c");
  spit(roms / "Zelda (Disc 2).gb", "rom-d");
  spit(roms / "Disc Game.cue", "FILE \"Disc Game.bin\" BINARY");
  spit(roms / "Disc Game.bin", "bin");
  spit(roms / "media/images/mario.png", "png");
  spit(roms / "gamelist.xml",
       "<?xml version=\"1.0\"?>\n<gameList>\n"
       "<game><path>./Super Mario (USA).gb</path><name>Super Mario</name><image>./media/images/mario.png</image></game>\n"
       "<game><path>./Zelda (Disc 1).gb</path><name>Zelda 1</name><image>./media/images/gone.png</image></game>\n"
       "</gameList>\n");
  spit(dir / "root/tmp/rocknix-systems.out",
       "> Sega Saturn\nMISSING 85ec9ca47d8f6807718151cbcca8b964 bios/saturn_bios.bin\nINVALID - bios/sega_101.bin\n> PlayStation\nMISSING - bios/scph1001.bin\n"
       "> gb\nMISSING - bios/gb_bios.bin\n");  // the console names systems by their short name; Saturn and PlayStation have no games here

  System gb;
  gb.name = "gb";
  gb.fullname = "Game Boy";
  gb.rom_dir = roms.string();
  gb.gamelist = (roms / "gamelist.xml").string();
  gb.exts = {".gb", ".zip", ".7z", ".cue", ".bin"};
  System empty_sys;  // an empty system folder is not a problem
  empty_sys.name = "nes";
  empty_sys.fullname = "NES";
  empty_sys.rom_dir = (dir / "roms/nes").string();
  fs::create_directories(dir / "roms/nes");
  empty_sys.exts = {".nes"};

  const std::map<std::string, std::string> before = snapshot(dir);

  // ---- the scan
  doctor::Report rep = doctor::scan({gb, empty_sys});
  CHECK_EQ(rep.games, size_t(11));
  CHECK_EQ(rep.broken.size(), size_t(3));
  bool has_empty = false, has_fake_zip = false, has_bad_7z = false, flagged_good = false;
  for (const doctor::Problem& p : rep.broken) {
    if (p.file == "empty.gb" && p.detail == "EMPTY FILE") has_empty = true;
    if (p.file == "fake.zip" && p.detail == "NOT A VALID ARCHIVE") has_fake_zip = true;
    if (p.file == "bad.7z" && p.detail == "NOT A VALID ARCHIVE") has_bad_7z = true;
    if (p.file == "real.zip" || p.file == "good.7z") flagged_good = true;
    CHECK_EQ(p.system, std::string("Game Boy"));
  }
  CHECK(has_empty && has_fake_zip && has_bad_7z && !flagged_good);
  CHECK_EQ(rep.duplicates.size(), size_t(1));  // the two Mario regions; the discs and cue+bin are not duplicates
  if (rep.duplicates.size() == 1) {
    CHECK_EQ(rep.duplicates[0].file, std::string("super mario"));
    CHECK_EQ(rep.duplicates[0].detail, std::string("2"));
  }
  CHECK_EQ(rep.no_image.size(), size_t(1));  // only Game Boy; NES has no games
  if (rep.no_image.size() == 1) CHECK_EQ(rep.no_image[0].n, size_t(10));  // 11 games, one has a picture on disk (Zelda 1 lists one that is gone)

  // ---- the report menu (dry-run: synchronous, the BIOS sample stands in for the script)
  Ctx cx;
  theme(cx);
  cx.systems = {gb, empty_sys};
  mg::Env saved = mg::env();
  mg::env().root = (dir / "root").string();
  {
    std::unique_ptr<Screen> s = open_report(cx);
    Menu* m = static_cast<Menu*>(s.get());
    const int tot = find(*m, "PROBLEMS FOUND");
    CHECK(tot >= 0);
    // one unit all through, the same numbers as the rows below: missing BIOS files + 3 broken + 1 duplicate + 10 games without a picture
    const int brow = find(*m, "MISSING BIOS CHECK");
    CHECK(brow >= 0);
    if (tot >= 0 && brow >= 0) {
      const int nb = std::atoi(m->entry(size_t(brow)).value_text.c_str());
      CHECK(nb >= 1);
      CHECK_EQ(m->entry(size_t(tot)).value_text, std::to_string(nb + 3 + 1 + 10));
    }
    CHECK(find(*m, "MISSING BIOS") >= 0 && find(*m, "POSSIBLY BROKEN FILES") >= 0 && find(*m, "DUPLICATES") >= 0 && find(*m, "GAMES WITHOUT A PICTURE") >= 0);
    CHECK(find(*m, "Sega Saturn") < 0 && find(*m, "PlayStation") < 0);  // no games there: their BIOS is not a problem of this library
    CHECK(find(*m, "gb") >= 0);
    CHECK(find(*m, "Game Boy: empty.gb") >= 0);
    CHECK(find(*m, "Game Boy: fake.zip") >= 0);
    CHECK(find(*m, "Game Boy: real.zip") < 0);
    CHECK(find(*m, "Game Boy: super mario") >= 0);
    const int pic = find(*m, "Game Boy");
    CHECK(pic >= 0);
    if (pic >= 0) CHECK_EQ(m->entry(size_t(pic)).value_text, std::string("10"));
    CHECK(find(*m, "NES") < 0);  // nothing to say about an empty folder
    const int bios = find(*m, "MISSING BIOS CHECK"), scr = find(*m, "SCRAPER");
    CHECK(bios >= 0 && scr >= 0);
    if (bios >= 0) CHECK(m->entry(size_t(bios)).show_arrow);
    if (scr >= 0) CHECK(m->entry(size_t(scr)).show_arrow);
  }

  // ---- a long group is cut at 50 with "AND MORE"; a clean library says so
  {
    const fs::path dir2 = fs::path(dir.string() + "-extra");  // outside the snapshot: no BIOS sample here either
    mg::env().root = dir2.string();
    fs::path big = dir2 / "roms/big";
    for (int i = 0; i < 60; ++i) spit(big / ("e" + std::to_string(i) + ".gb"), "");
    System b;
    b.name = "big";
    b.fullname = "Big";
    b.rom_dir = big.string();
    b.exts = {".gb"};
    Ctx c2;
    theme(c2);
    c2.systems = {b};
    std::unique_ptr<Screen> s2 = open_report(c2);
    Menu* m2 = static_cast<Menu*>(s2.get());
    const int more = find(*m2, "AND MORE");
    CHECK(more >= 0);
    if (more >= 0) CHECK_EQ(m2->entry(size_t(more)).value_text, std::string("10"));
    CHECK(find(*m2, "Big: e0.gb") >= 0);
    CHECK(find(*m2, "Big: e59.gb") < 0);

    System fine;
    fine.name = "ok";
    fine.fullname = "Fine";
    fs::path okdir = dir2 / "roms/ok";
    spit(okdir / "a.gb", "x");
    spit(okdir / "media/a.png", "x");
    spit(okdir / "gamelist.xml", "<gameList><game><path>./a.gb</path><name>A</name><image>./media/a.png</image></game></gameList>");
    fine.rom_dir = okdir.string();
    fine.gamelist = (okdir / "gamelist.xml").string();
    fine.exts = {".gb"};
    c2.systems = {fine};
    std::unique_ptr<Screen> s3 = open_report(c2);
    Menu* m3 = static_cast<Menu*>(s3.get());
    CHECK(find(*m3, "NO PROBLEMS FOUND") >= 0);
    CHECK(find(*m3, "PROBLEMS FOUND") < 0);
    fs::remove_all(dir2);
  }
  mg::env() = saved;

  // ---- nothing changed: every entry of the snapshot has the same size and mtime (the folders of the last block were added after it)
  std::map<std::string, std::string> after = snapshot(dir);
  for (const auto& kv : before) {
    auto it = after.find(kv.first);
    CHECK(it != after.end());
    if (it != after.end()) CHECK_EQ(it->second, kv.second);
  }
  fs::remove_all(dir);
  // ---- systems that hold no games are left out; the BIOS list keeps to the systems that have games (on the console it listed 101
  // files of systems with no game and 100 "screenshots without a picture", 9-nov)
  {
    System shots;
    shots.name = "screenshots";
    shots.theme = "imageviewer";
    System tools;
    tools.name = "tools";
    tools.platform = "tools";
    System music;
    music.name = "music";
    music.platform = "music";
    System ign;
    ign.name = "x";
    ign.platform = "ignore";
    System gb;
    gb.name = "gb";
    gb.platform = "gb";
    CHECK(!doctor::is_game_system(shots) && !doctor::is_game_system(tools) && !doctor::is_game_system(music) && !doctor::is_game_system(ign));
    CHECK(doctor::is_game_system(gb));
    std::vector<std::pair<std::string, size_t>> all = {{"gb", 1}, {"scv", 1}, {"segacd", 3}};
    std::vector<std::pair<std::string, size_t>> kept = doctor::bios_for(all, {"gb", "psp"});
    CHECK_EQ(kept.size(), size_t(1));
    CHECK(!kept.empty() && kept[0].first == "gb");
  }
  return zt::done();
}
