// VIEW GAME MANUAL / VIEW GAME MAGAZINE: the PDF viewer (manual.cpp). The pure parts (pdfinfo parsing, the pdftoppm command line, the cache names,
// ES's dpi rule), the rows of the game options (only when the file and the PDF programs exist), and the viewer over fake `pdfinfo` / `pdftoppm`
// scripts in a temp folder (lazy extraction order, page turning, zoom, the counter inside the margins, the cache emptied on close). Temp dirs only.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "i18n.hpp"
#include "manual.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "registry.hpp"
#include "theme.hpp"
#include "ui_style.hpp"
#include "util.hpp"

using namespace zf;

namespace {

std::string tmpdir(const char* tag) {
  std::string d = "/tmp/zf_test_manual_" + std::string(tag) + "_" + std::to_string(getpid());
  std::string cmd = "rm -rf " + d + " && mkdir -p " + d;
  CHECK(std::system(cmd.c_str()) == 0);
  return d;
}
void spit(const std::string& p, const std::string& s, bool exec = false) {
  FILE* f = std::fopen(p.c_str(), "wb");
  CHECK(f != nullptr);
  if (f) {
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
  }
  if (exec) ::chmod(p.c_str(), 0755);
}
std::string slurp(const std::string& p) {
  std::string s;
  read_file(p, s);
  return s;
}
bool exists(const std::string& p) { return file_exists(p); }
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

// three games: g0 has a manual (file there), g1 none, g2 a magazine whose file is missing
// the viewer draws text (page counter): the real theme's font, as the other drawing tests (ZF_THEME_DIR or its theme-dark)
bool load_theme_font(Ctx& cx) {
  const char* e = std::getenv("ZF_THEME_DIR");
  const std::string base = e ? e : "/usr/share/zfront/theme-dark";
  for (const std::string& d : {base, base + "/theme-dark"}) {
    Theme th;
    std::string err;
    if (Theme::load(d, "1/1", th, err) && cx.assets.load_font(th.path(th.str("font")), err)) return true;
  }
  return false;
}

struct Fixture {
  std::string dir;
  Ctx cx;
  bool font = false;
  explicit Fixture(const char* tag, float side = 640) {
    dir = tmpdir(tag);
    std::string x = "<?xml version=\"1.0\"?>\n<gameList>\n";
    for (int i = 0; i < 3; ++i) {
      std::string n = std::to_string(i);
      spit(dir + "/g" + n + ".gb", "rom" + n);
      x += "\t<game>\n\t\t<path>./g" + n + ".gb</path>\n\t\t<name>G" + n + "</name>\n";
      if (i == 0) x += "\t\t<manual>./manuals/g0.pdf</manual>\n";
      if (i == 2) x += "\t\t<magazine>./manuals/mag2.pdf</magazine>\n";
      x += "\t</game>\n";
    }
    x += "</gameList>\n";
    spit(dir + "/gamelist.xml", x);
    CHECK(std::system(("mkdir -p " + dir + "/manuals " + dir + "/root/usr/bin " + dir + "/root/storage/.config/emulationstation").c_str()) == 0);
    spit(dir + "/manuals/g0.pdf", "%PDF-1.4");
    System s;
    s.name = "gb";
    s.fullname = "Game Boy";
    s.rom_dir = dir;
    s.gamelist = dir + "/gamelist.xml";
    s.exts = {".gb"};
    cx.systems.push_back(std::move(s));
    cx.open_system(0);
    std::string err;
    CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
    cx.w = side;
    cx.h = side;
    font = load_theme_font(cx);
    mg::env().root = dir + "/root";
    mg::reset_caches();
  }
  ~Fixture() {
    mg::env().root.clear();
    mg::reset_caches();
    std::string cmd = "rm -rf " + dir;
    if (std::system(cmd.c_str())) {
    }
  }
  std::string rom(int i) const { return dir + "/g" + std::to_string(i) + ".gb"; }
  std::string bin() const { return dir + "/root/usr/bin"; }
  std::string cache() const { return dir + "/root/storage/.config/emulationstation/pdftmp"; }
  // fake pdfinfo (reads its page count from `pages`) and pdftoppm (logs its arguments, writes the picture where it was told)
  void fake_programs(int pages) {
    spit(bin() + "/pages", std::to_string(pages));
    spit(bin() + "/pdfinfo",
         "#!/bin/sh\nd=$(dirname \"$0\")\necho 'Title:          Sample'\necho 'CreationDate:   Mon Jan  1 10:00:00 2020 CET'\n"
         "echo 'Page size:      595.276 x 841.89 pts (A4)'\necho \"Pages:          $(cat \"$d/pages\")\"\n",
         true);
    spit(bin() + "/pdftoppm",
         "#!/bin/sh\nd=$(dirname \"$0\")\nfor a; do last=\"$a\"; done\necho \"$@\" >> \"$d/log\"\ncp \"$ZF_TEST_DATA_DIR/cover-box.png\" \"${last}-1.jpg\"\n", true);
  }
  std::vector<std::string> log() const {
    std::vector<std::string> v;
    std::string t = slurp(bin() + "/log");
    size_t i = 0;
    while (i < t.size()) {
      size_t e = t.find('\n', i);
      if (e == std::string::npos) e = t.size();
      if (e > i) v.push_back(t.substr(i, e - i));
      i = e + 1;
    }
    return v;
  }
};

Menu& as_menu(std::unique_ptr<Screen>& s) { return *static_cast<Menu*>(s.get()); }
int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}
bool has(const std::string& hay, const std::string& n) { return hay.find(n) != std::string::npos; }

}  // namespace

int main() {
  ::setenv("ZF_TEST_DATA_DIR", ZF_TEST_DATA, 1);

  // ---- pure parts
  {
    const std::string info =
        "Title:          Sample Manual\nCreator:        Writer\nProducer:       LibreOffice\nCreationDate:   Mon Jan  1 10:00:00 2020 CET\n"
        "Page size:      595.276 x 841.89 pts (A4)\nPages:          24\nEncrypted:      no\n";
    CHECK_EQ(manual::parse_page_count(info), 24);
    CHECK_EQ(manual::parse_page_count("Pages:7"), 7);
    CHECK_EQ(manual::parse_page_count("Page size:      612 x 792 pts\n"), 0);  // "Page size" is not "Pages"
    CHECK_EQ(manual::parse_page_count("Pages: none\n"), 0);
    CHECK_EQ(manual::parse_page_count(""), 0);
    CHECK_EQ(manual::parse_page_count("pdfinfo: no such file\n"), 0);
    CHECK_EQ(manual::pdfinfo_command("/r/a b.pdf"), std::string("pdfinfo ") + shell_quote("/r/a b.pdf"));

    std::string force = "true", no = "false", empty;
    CHECK(manual::small_screen(640, 480, nullptr));  // ES: a side of 480 or less
    CHECK(manual::small_screen(480, 320, nullptr));
    CHECK(!manual::small_screen(720, 720, nullptr));
    CHECK(!manual::small_screen(1280, 720, nullptr));
    CHECK(manual::small_screen(1280, 720, &force));  // ForceSmallScreen wins
    CHECK(!manual::small_screen(640, 480, &no));
    CHECK(manual::small_screen(640, 480, &empty));
    CHECK_EQ(manual::page_dpi(true), 96);
    CHECK_EQ(manual::page_dpi(false), 125);

    const std::string pdf = "/storage/roms/psp/manuals/Game (USA).pdf";
    const std::string prefix = manual::page_prefix(pdf, 125, 3);
    CHECK_EQ(prefix, std::string("Game (USA).pdf-125-00000003-pdf"));
    CHECK_EQ(manual::page_prefix(pdf, 96, 12), std::string("Game (USA).pdf-96-00000012-pdf"));
    // page 1 / 10 / 100 never share a prefix (8 digits)
    CHECK(!starts_with(manual::page_prefix(pdf, 125, 10), manual::page_prefix(pdf, 125, 1)));
    const std::string cmd = manual::extract_command(pdf, 125, 3, "/c", prefix);
    CHECK_EQ(cmd, "pdftoppm -jpeg -r 125 -cropbox -f 3 -l 3 " + shell_quote(pdf) + " " + shell_quote("/c/" + prefix));
    CHECK(has(manual::extract_command("/a'b.pdf", 96, 1, "/c", "p"), "-r 96 "));
    CHECK(has(manual::extract_command("/a'b.pdf", 96, 1, "/c", "p"), "'/a'\\''b.pdf'"));  // quoted

    // pdftoppm adds `-<n>.jpg` itself, padded by the page count of the document: the file is found by its prefix
    const std::string p3 = manual::page_prefix(pdf, 125, 3), p13 = manual::page_prefix(pdf, 125, 13);
    std::vector<std::string> names = {p13 + "-13.jpg", "other.jpg", p3 + "-03.jpg", p3 + ".txt", p3 + "-03.JPG.bak"};
    CHECK_EQ(manual::find_page_file(names, p3), p3 + "-03.jpg");
    CHECK_EQ(manual::find_page_file(names, p13), p13 + "-13.jpg");
    CHECK_EQ(manual::find_page_file(names, manual::page_prefix(pdf, 125, 4)), std::string());
    CHECK_EQ(manual::find_page_file({p3 + "-3.PPM"}, p3), p3 + "-3.PPM");
    CHECK_EQ(manual::find_page_file({p3 + "-3.png", p3 + "-2.jpg"}, p3), p3 + "-2.jpg");  // sorted: the first
    CHECK_EQ(manual::find_page_file({}, p3), std::string());

    mg::env().root = "/r";
    CHECK_EQ(manual::cache_dir(), std::string("/r/storage/.config/emulationstation/pdftmp"));  // ES: <user dir>/pdftmp
    mg::env().root.clear();
    CHECK_EQ(manual::cache_dir(), std::string("/storage/.config/emulationstation/pdftmp"));
  }

  // ---- the rows of the game options
  {
    Fixture f("rows");
    const std::string arg0 = mg::game_arg("gb", f.rom(0)), arg1 = mg::game_arg("gb", f.rom(1)), arg2 = mg::game_arg("gb", f.rom(2));
    CHECK(!manual::available());  // no programs yet
    CHECK_EQ(manual::media_file(f.cx, "gb", f.rom(0), manual::Kind::Manual), f.dir + "/manuals/g0.pdf");
    CHECK_EQ(manual::media_file(f.cx, "gb", f.rom(1), manual::Kind::Manual), std::string());
    CHECK_EQ(manual::media_file(f.cx, "gb", f.rom(2), manual::Kind::Magazine), std::string());  // named, but the file is not there
    CHECK_EQ(manual::media_file(f.cx, "gb", f.rom(0), manual::Kind::Magazine), std::string());
    CHECK_EQ(manual::media_file(f.cx, "nope", f.rom(0), manual::Kind::Manual), std::string());
    {
      auto s = Registry::instance().make("menu.game_options", f.cx, arg0);
      CHECK(s != nullptr);
      as_menu(s).rebuild(f.cx);
      CHECK(find(as_menu(s), tr("VIEW GAME MANUAL")) < 0);  // the file is there but pdftoppm / pdfinfo are not
      CHECK(find(as_menu(s), tr("GAME MEDIA")) < 0);
    }
    spit(f.bin() + "/pdftoppm", "#!/bin/sh\n", true);
    CHECK(!manual::available());  // both are needed
    spit(f.bin() + "/pdfinfo", "#!/bin/sh\n", true);
    CHECK(manual::available());
    {
      auto s = Registry::instance().make("menu.game_options", f.cx, arg0);
      Menu& m = as_menu(s);
      m.rebuild(f.cx);
      int h = find(m, tr("GAME MEDIA")), man = find(m, tr("VIEW GAME MANUAL"));
      CHECK(h >= 0);
      CHECK(man > h);
      CHECK(find(m, tr("VIEW GAME MAGAZINE")) < 0);  // g0 has no magazine
      for (size_t i = 0; i < m.size() + 1 && m.cursor() != man; ++i) m.handle(Action::Down, f.cx);
      Transition t = m.handle(Action::A, f.cx);
      CHECK(t.kind == Transition::Open);
      CHECK_EQ(t.name, std::string("menu.game.manual"));
      CHECK_EQ(t.arg, arg0);
    }
    for (const std::string& arg : {arg1, arg2}) {  // no manual, and a magazine whose file is missing: nothing
      auto s = Registry::instance().make("menu.game_options", f.cx, arg);
      Menu& m = as_menu(s);
      m.rebuild(f.cx);
      CHECK(find(m, tr("GAME MEDIA")) < 0);
      CHECK(find(m, tr("VIEW GAME MANUAL")) < 0);
      CHECK(find(m, tr("VIEW GAME MAGAZINE")) < 0);
    }
    spit(f.dir + "/manuals/mag2.pdf", "%PDF-1.4");  // now the magazine exists
    {
      auto s = Registry::instance().make("menu.game_options", f.cx, arg2);
      Menu& m = as_menu(s);
      m.rebuild(f.cx);
      CHECK(find(m, tr("GAME MEDIA")) >= 0);
      CHECK(find(m, tr("VIEW GAME MAGAZINE")) >= 0);
      CHECK(find(m, tr("VIEW GAME MANUAL")) < 0);
    }
    // the viewer screens refuse what they cannot show (test_uimode opens every screen with junk too)
    CHECK(Registry::instance().make("menu.game.manual", f.cx, "") == nullptr);
    CHECK(Registry::instance().make("menu.game.manual", f.cx, "x\ty") == nullptr);
    CHECK(Registry::instance().make("menu.game.manual", f.cx, arg1) == nullptr);  // no manual
    CHECK(Registry::instance().make("menu.game.magazine", f.cx, arg0) == nullptr);  // no magazine
    settle(f.cx);
  }

  // ---- the viewer over fake programs
  {
    Fixture f("view", 720);  // 720x720: not a small screen, 125 dpi
    f.fake_programs(3);
    const std::string old_path = std::getenv("PATH") ? std::getenv("PATH") : "";
    ::setenv("PATH", (f.bin() + ":" + old_path).c_str(), 1);
    const std::string pdf = f.dir + "/manuals/g0.pdf";
    {
      auto v = Registry::instance().make("menu.game.manual", f.cx, mg::game_arg("gb", f.rom(0)));
      CHECK(v != nullptr);
      settle(f.cx);
      manual::ViewerState st = manual::viewer_state(*v);
      CHECK_EQ(st.pages, 3);
      CHECK_EQ(st.page, 1);
      CHECK(!st.closed);
      // lazy: the page on screen first, then its neighbour; the last page is not asked for yet
      std::vector<std::string> log = f.log();
      CHECK_EQ(log.size(), size_t(2));
      if (log.size() == 2) {
        CHECK(has(log[0], "-jpeg -r 125 -cropbox -f 1 -l 1 "));
        CHECK(has(log[0], pdf));
        CHECK(has(log[0], f.cache() + "/" + manual::page_prefix(pdf, 125, 1)));
        CHECK(has(log[1], "-f 2 -l 2 "));
      }
      CHECK(exists(f.cache()));

      // the first draw: the page picture, the counter inside the margins, nothing else outside the screen
      std::vector<Op> out;
      CHECK(f.font);  // no font: a clear failure instead of a crash in Font::measure
      if (f.font) v->draw(f.cx, out);
      st = manual::viewer_state(*v);
      const ui::Metrics mt = ui::Metrics::from(f.cx.th, f.cx.w, f.cx.h);
      // the page lives between the title row (the counter) and the help bar, as the content of the menus: on the device the help
      // was drawn over the white page and could not be read (9-nov)
      const float help_top = 2.0f * mt.help_y - f.cx.h;
      CHECK(st.area.h > 0.5f * f.cx.h);
      CHECK(st.area.y + st.area.h <= help_top + 0.5f);
      size_t pages = 0;
      for (size_t i = 0; i + 1 < out.size(); ++i)  // the page: an image inside a clip of the page area
        if (out[i].kind == Op::Clip && out[i + 1].kind == Op::Image) {
          ++pages;
          const Op& im = out[i + 1];
          CHECK(out[i].y >= st.area.y - 0.5f && out[i].y + out[i].h <= st.area.y + st.area.h + 0.5f);
          CHECK(im.y >= st.area.y - 0.5f && im.y + im.h <= st.area.y + st.area.h + 0.5f);  // fitted to the area at zoom 1
          CHECK(im.w <= f.cx.w + 0.5f);
          CHECK(im.w >= f.cx.w - 1.5f || im.h >= st.area.h - 1.5f);
        }
      CHECK_EQ(pages, size_t(1));
      const float margin = mt.margin;
      CHECK(st.counter.y + st.counter.h <= st.area.y + 0.5f);  // the counter stays on the black of the title row
      CHECK(st.counter.w > 0);
      CHECK(st.counter.x >= margin - 0.5f);
      CHECK(st.counter.x + st.counter.w <= f.cx.w - margin + 0.5f);
      CHECK(st.counter.y >= 0 && st.counter.y + st.counter.h <= f.cx.h);

      // Right / Left turn the page while it fits, and the next one is extracted behind it
      CHECK(v->handle(Action::Right, f.cx).kind == Transition::None);
      settle(f.cx);
      CHECK_EQ(manual::viewer_state(*v).page, 2);
      CHECK_EQ(f.log().size(), size_t(3));
      CHECK(has(f.log().back(), "-f 3 -l 3 "));
      v->handle(Action::Right, f.cx);
      v->handle(Action::Right, f.cx);  // no wrap
      settle(f.cx);
      CHECK_EQ(manual::viewer_state(*v).page, 3);
      CHECK_EQ(f.log().size(), size_t(3));  // nothing more to extract
      v->handle(Action::Left, f.cx);
      CHECK_EQ(manual::viewer_state(*v).page, 2);

      // zoom: A zooms in / out, a zoomed page is moved (not turned) by the d-pad, L / R step the zoom
      v->handle(Action::A, f.cx);
      CHECK(manual::viewer_state(*v).zoom > 1.5f);
      v->handle(Action::Right, f.cx);
      CHECK_EQ(manual::viewer_state(*v).page, 2);
      out.clear();
      CHECK(f.font);  // no font: a clear failure instead of a crash in Font::measure
      if (f.font) v->draw(f.cx, out);
      v->handle(Action::A, f.cx);
      CHECK_EQ(manual::viewer_state(*v).zoom, 1.0f);
      v->handle(Action::R, f.cx);
      CHECK(manual::viewer_state(*v).zoom > 1.0f);
      v->handle(Action::L, f.cx);
      CHECK_EQ(manual::viewer_state(*v).zoom, 1.0f);
      v->handle(Action::Left, f.cx);  // fits again: turns
      CHECK_EQ(manual::viewer_state(*v).page, 1);

      // B closes and the cache is emptied (after any extraction still running)
      CHECK(exists(f.cache() + "/" + manual::page_prefix(pdf, 125, 1) + "-1.jpg"));
      CHECK(v->handle(Action::Start, f.cx).kind == Transition::Pop);  // Start leaves like B (the viewer is no menu: close_menus would skip it)
      Transition t = v->handle(Action::B, f.cx);
      CHECK(t.kind == Transition::Pop);
      settle(f.cx);
      CHECK(!exists(f.cache() + "/" + manual::page_prefix(pdf, 125, 1) + "-1.jpg"));
    }
    // pdftoppm that produces no picture: the page says so (it used to stay black), and the viewer still closes with B
    f.fake_programs(2);
    spit(f.bin() + "/pdftoppm", "#!/bin/sh\nexit 0\n", true);
    {
      auto v = Registry::instance().make("menu.game.manual", f.cx, mg::game_arg("gb", f.rom(0)));
      CHECK(v != nullptr);
      settle(f.cx);
      std::vector<Op> out;
      if (f.font) v->draw(f.cx, out);
      CHECK(manual::viewer_state(*v).failed);
      CHECK(v->handle(Action::B, f.cx).kind == Transition::Pop);
      settle(f.cx);
    }
    // a document with no pages: no viewer (ES: delete this), asked for once
    f.fake_programs(0);
    ::unlink((f.bin() + "/log").c_str());
    {
      auto v = Registry::instance().make("menu.game.manual", f.cx, mg::game_arg("gb", f.rom(0)));
      CHECK(v != nullptr);
      settle(f.cx);
      CHECK(manual::viewer_state(*v).closed);
      CHECK(v->take_transition(f.cx).kind == Transition::Pop);
      CHECK(v->take_transition(f.cx).kind == Transition::None);
      CHECK(f.log().empty());  // pdftoppm never ran
    }
    ::setenv("PATH", old_path.c_str(), 1);
    settle(f.cx);
  }

  // ---- dry-run: the command is only logged, nothing is extracted
  {
    Fixture f("dry");
    f.fake_programs(3);
    f.cx.sv->cmd.dry_run = true;
    auto v = manual::make_viewer(f.cx, f.dir + "/manuals/g0.pdf");
    settle(f.cx);
    CHECK(manual::viewer_state(*v).closed);
    CHECK_EQ(f.cx.sv->cmd.log.size(), size_t(1));
    if (!f.cx.sv->cmd.log.empty()) CHECK_EQ(f.cx.sv->cmd.log[0], manual::pdfinfo_command(f.dir + "/manuals/g0.pdf"));
    CHECK(f.log().empty());
  }
  return zt::done();
}
