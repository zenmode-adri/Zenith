// The "Add games" card (addgames.hpp): it exists only on a console with no game at all, every key on it is safe, paging from a list
// skips it, and it goes away by itself when the list is read again with a game in it.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "addgames.hpp"
#include "app.hpp"
#include "check.hpp"
#include "collections.hpp"
#include "registry.hpp"
#include "scan.hpp"
#include "screens.hpp"
#include "syslist.hpp"
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

void touch(const std::string& p) {
  FILE* f = std::fopen(p.c_str(), "w");
  if (f) std::fclose(f);
}

struct FakeMenu : Screen {
  Transition handle(Action a, Ctx&) override { return a == Action::B ? Transition::pop() : Transition::none(); }
  void draw(Ctx&, std::vector<Op>&) override {}
  bool is_menu() const override { return true; }
};

// es_systems.cfg: Tools (holds no games) and SNES
std::string systems_xml() {
  return "<systemList>"
         "<system><name>tools</name><fullname>Tools</fullname><path>/storage/roms/tools</path><extension>.sh</extension>"
         "<command>run %ROM%</command><platform>tools</platform><theme>tools</theme></system>"
         "<system><name>snes</name><fullname>Super Nintendo</fullname><path>/storage/roms/snes</path><extension>.sfc</extension>"
         "<command>run %ROM%</command><platform>snes</platform><theme>snes</theme></system>"
         "</systemList>";
}

void setup(Ctx& cx) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = true;
  cx.es = EsSettings::from_text("<?xml version=\"1.0\"?>\n<config>\n</config>\n", g_tmp + "/es_settings.cfg");
  const std::string xml = systems_xml(), roms = g_tmp + "/roms";
  CHECK(load_systems_text_lazy(xml, roms, cx.systems, err));
  cx.items = make_items(cx.systems, cx.es);
  cx.system_loader = [xml, roms](bool load_empty, std::vector<System>& out, std::string& e) { return load_systems_text_lazy(xml, roms, out, e, load_empty); };
}

std::vector<std::string> cards(const Ctx& cx) { return item_names(cx.systems, cx.items); }

}  // namespace

int main() {
  Registry::instance().add("menu.main", [](Ctx&, const std::string&) { return std::make_unique<FakeMenu>(); });
  g_tmp = "/tmp/zf_addgames_" + std::to_string(getpid());
  mkdir(g_tmp.c_str(), 0755);
  mkdir((g_tmp + "/roms").c_str(), 0755);
  for (const char* n : {"tools", "snes"}) mkdir((g_tmp + "/roms/" + n).c_str(), 0755);
  touch(g_tmp + "/roms/tools/Start R36 Tuner.sh");
  setenv("ZF_COLLECTIONS_DIR", (g_tmp + "/collections").c_str(), 1);

  // ---- no game at all: the card goes first, with no count; Tools follows
  {
    Ctx cx;
    setup(cx);
    CHECK_EQ(cx.systems.size(), size_t(1));  // the empty SNES is not loaded
    CHECK_EQ(cx.items.size(), size_t(2));
    CHECK_EQ(cx.items[0].sys, kItemAddGames);
    CHECK(cx.items[0].games == kCountPending);  // no theme writes "0 games" or reads a row for it
    CHECK_EQ(cx.items[0].full_name, std::string("Add games"));
    CHECK_EQ(cx.items[1].sys, size_t(0));
    CHECK_EQ(cards(cx).size(), size_t(2));
    CHECK_EQ(cards(cx)[1], std::string("tools"));
    CHECK(cards(cx)[0] != cards(cx)[1]);

    // every key on the card (offline: wifi off)
    CarouselScreen c(0);
    c.tick(0.0f, cx);
    Transition t = c.handle(Action::A, cx);
    CHECK(t.kind == Transition::Push && t.screen != nullptr);  // the message on how to copy games, not a game list
    t = c.handle(Action::X, cx);
    CHECK(t.kind == Transition::None);
    c.tick(5.0f, cx);
    CHECK_EQ(c.system_cursor(cx), 0l);  // no random jump
    for (Action a : {Action::B, Action::Up, Action::Down, Action::L, Action::R}) {
      t = c.handle(a, cx);
      CHECK(t.kind == Transition::None);
    }
    CHECK(c.handle(Action::Select, cx).kind == Transition::Open);
    CHECK(c.handle(Action::Start, cx).kind == Transition::Open);
    for (int i = 0; i < 4; ++i) c.tick(1.0f, cx);  // the card is drawn by test_layout in every theme ("sin-juegos"): this rig has no fonts
  }

  // ---- in Tools, paging with left / right skips the card (one list only: it stays)
  {
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx);
    App app(std::move(cx), std::make_unique<CarouselScreen>(1), make_status_source("/nonexistent"));  // on Tools
    app.tick(0.0f);
    app.handle(Action::A);  // the list of Tools
    CHECK(app.cx.systems[0].loaded);
    CHECK(app.handle(Action::Right).kind == Outcome::None);
    CHECK(app.handle(Action::Left).kind == Outcome::None);
    CHECK(app.launch_target().find("/roms/tools/") != std::string::npos);  // still the list of Tools
  }

  // ---- a game arrives (the web transfer reloads the list): the card is gone, the cursor is valid
  {
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx);
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));  // on the card
    app.tick(0.0f);
    touch(g_tmp + "/roms/snes/Game.sfc");
    app.handle(Action::Start);
    col::set_reload_needed(true);
    app.handle(Action::B);
    for (int i = 0; i < 200 && !app.cx.sv->jobs.idle(); ++i) {
      app.drain_jobs(Clock::now());
      usleep(5000);
    }
    CHECK_EQ(app.cx.systems.size(), size_t(2));
    CHECK_EQ(app.cx.items.size(), size_t(2));
    for (const SystemItem& it : app.cx.items) CHECK(it.sys != kItemAddGames);
    app.tick(0.0f);
    std::vector<Op> ops;
    (void)ops;
    CHECK(app.handle(Action::X).kind == Outcome::None);  // random again possible, no crash
  }

  // ---- with one game anywhere the card does not exist (the carousel is what it was)
  {
    Ctx cx;
    setup(cx);
    CHECK_EQ(cx.items.size(), size_t(2));
    for (const SystemItem& it : cx.items) CHECK(it.sys != kItemAddGames);
  }

  std::string cmd = "rm -rf '" + g_tmp + "'";
  if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
  return zt::done();
}
