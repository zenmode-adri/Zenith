// Hot reload of the system list: after the menus close, a change in the collection settings (HiddenSystems, SortSystems,
// LoadEmptySystems) shows in the carousel without restarting, the cursor stays on the same system, nothing is lost.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "collections.hpp"
#include "menu.hpp"
#include "registry.hpp"
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

// A window the root opens with Start: B closes it, Start closes every window, A opens the collections settings.
struct FakeMenu : Screen {
  Transition handle(Action a, Ctx&) override {
    if (a == Action::B) return Transition::pop();
    if (a == Action::Start) return Transition::close_menus();
    if (a == Action::A) return Transition::open("menu.collections");
    return Transition::none();
  }
  void draw(Ctx&, std::vector<Op>&) override {}
  bool is_menu() const override { return true; }
};

// es_systems.cfg: nes (Nintendo), snes (Super Nintendo), gb (Game Boy) with games, empty (Empty) without
std::string systems_xml() {
  auto sys = [](const char* name, const char* full, const char* ext) {
    return std::string("<system><name>") + name + "</name><fullname>" + full + "</fullname><path>/storage/roms/" + name + "</path><extension>" + ext +
           "</extension><command>run %ROM%</command></system>";
  };
  return "<systemList>" + sys("nes", "Nintendo", ".nes") + sys("snes", "Super Nintendo", ".sfc") + sys("gb", "Game Boy", ".gb") + sys("empty", "Empty", ".x") +
         "</systemList>";
}

// A loader that counts how many times it ran (like main's: lazy, only paths and the mode captured)
struct Loader {
  std::string xml, roms;
  int calls = 0;
  bool ok = true;
  bool lazy = true;
};

void setup(Ctx& cx, const std::shared_ptr<Loader>& ld, bool with_loader = true) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = true;
  cx.es = EsSettings::from_text("<?xml version=\"1.0\"?>\n<config>\n</config>\n", g_tmp + "/es_settings.cfg");
  CHECK(load_systems_text_lazy(ld->xml, ld->roms, cx.systems, err));
  cx.items = make_items(cx.systems, cx.es);
  if (with_loader)
    cx.system_loader = [ld](bool load_empty, std::vector<System>& out, std::string& e) {
      ++ld->calls;
      if (!ld->ok) {
        e = "boom";
        return false;
      }
      return ld->lazy ? load_systems_text_lazy(ld->xml, ld->roms, out, e, load_empty) : load_systems_text(ld->xml, ld->roms, false, out, e, load_empty);
    };
}

// names of the cards in order
std::vector<std::string> cards(const Ctx& cx) { return item_names(cx.systems, cx.items); }

std::vector<std::string> v(std::initializer_list<const char*> l) { return std::vector<std::string>(l.begin(), l.end()); }

// Opens the fake window, sets what the settings menus would have set and closes the window with B.
void change_and_close(App& app, const char* key, const char* value) {
  app.handle(Action::Start);  // opens the fake "menu.main"
  app.cx.es.set_string(key, value);
  col::set_reload_needed(true);
  app.handle(Action::B);
}

}  // namespace

int main() {
  Registry::instance().add("menu.main", [](Ctx&, const std::string&) { return std::make_unique<FakeMenu>(); });
  g_tmp = "/tmp/zf_reload_" + std::to_string(getpid());
  mkdir(g_tmp.c_str(), 0755);
  mkdir((g_tmp + "/roms").c_str(), 0755);
  for (const char* n : {"nes", "snes", "gb", "empty"}) mkdir((g_tmp + "/roms/" + n).c_str(), 0755);
  touch(g_tmp + "/roms/nes/a.nes");
  touch(g_tmp + "/roms/nes/b.nes");
  touch(g_tmp + "/roms/snes/c.sfc");
  touch(g_tmp + "/roms/gb/d.gb");
  touch(g_tmp + "/roms/gb/e.gb");
  touch(g_tmp + "/roms/gb/f.gb");
  setenv("ZF_COLLECTIONS_DIR", (g_tmp + "/collections").c_str(), 1);
  auto ld = std::make_shared<Loader>();
  ld->xml = systems_xml();
  ld->roms = g_tmp + "/roms";

  // ---- the pure parts
  {
    Ctx cx;
    setup(cx, ld);
    CHECK_EQ(cards(cx), v({"nes", "snes", "gb"}));  // empty systems are not loaded by default
    CHECK_EQ(cx.systems.size(), size_t(3));
    cx.es.set_string("HiddenSystems", "snes;nope");
    CHECK_EQ(item_names(cx.systems, make_items(cx.systems, cx.es)), v({"nes", "gb"}));
    cx.es.set_string("SortSystems", "alpha");  // by full name: Game Boy, Nintendo
    CHECK_EQ(item_names(cx.systems, make_items(cx.systems, cx.es)), v({"gb", "nes"}));
    std::vector<SystemItem> it = make_items(cx.systems, cx.es);
    CHECK_EQ(it[0].sys, size_t(2));  // gb is the third system of es_systems.cfg
    cx.es.set_string("SortSystems", "manufacturer");  // not offered: es_systems.cfg order
    CHECK_EQ(item_names(cx.systems, make_items(cx.systems, cx.es)), v({"nes", "gb"}));

    // nearest card when the one under the cursor disappears: the next one, then the previous one, wrapping
    CHECK_EQ(nearest_item(v({"a", "b", "c", "d"}), 1, v({"a", "b", "c", "d"})), 1l);
    CHECK_EQ(nearest_item(v({"a", "b", "c", "d"}), 1, v({"a", "c", "d"})), 1l);   // b gone: c
    CHECK_EQ(nearest_item(v({"a", "b", "c", "d"}), 3, v({"c", "a"})), 1l);          // d gone, wraps to a
    CHECK_EQ(nearest_item(v({"a", "b", "c", "d"}), 1, v({"c", "d"})), 0l);          // b gone, c is next
    CHECK_EQ(nearest_item(v({"a", "b"}), 0, v({"x"})), 0l);                         // nothing in common
    CHECK_EQ(nearest_item(v({"a", "b"}), 0, std::vector<std::string>()), 0l);
    CHECK_EQ(nearest_item(v({"a", "b", "c"}), 2, v({"a", "b"})), 0l);              // c gone: next (wraps) is a
  }

  // ---- loader keeps empty systems on request
  {
    std::vector<System> out;
    std::string err;
    CHECK(load_systems_text_lazy(ld->xml, ld->roms, out, err, true));
    CHECK_EQ(out.size(), size_t(4));
    CHECK_EQ(out[3].name, std::string("empty"));
    CHECK_EQ(out[3].count, size_t(0));
    CHECK(out[0].count == kCountPending);
    CHECK(load_systems_text(ld->xml, ld->roms, false, out, err, true));
    CHECK_EQ(out.size(), size_t(4));
    CHECK_EQ(out[0].count, size_t(2));
  }

  // ---- hiding a system: the carousel changes when the window closes, the cursor keeps the system, no notice
  {
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx, ld);
    App app(std::move(cx), std::make_unique<CarouselScreen>(2), make_status_source("/nonexistent"));  // on gb
    int before = ld->calls;
    app.handle(Action::Start);
    app.cx.es.set_string("HiddenSystems", "nes");
    col::set_reload_needed(true);
    CHECK_EQ(ld->calls, before);  // nothing yet: the window is still open
    CHECK_EQ(app.cx.items.size(), size_t(3));
    app.handle(Action::B);
    CHECK_EQ(ld->calls, before + 1);
    CHECK_EQ(cards(app.cx), v({"snes", "gb"}));
    CHECK_EQ(app.cx.systems.size(), size_t(3));  // the menus still list every system
    CHECK(!col::reload_needed());
    CHECK(app.cx.sv->toasts.items().empty());
    // the cursor is on gb (card 1 now) and the first tick announces it, no glide
    app.tick(0.0f);
    app.handle(Action::A);  // enters the system under the cursor: the list of gb (3 games)
    CHECK(app.cx.systems[2].loaded);
    CHECK_EQ(app.cx.systems[2].games.size(), size_t(3));
    CHECK_EQ(app.cx.systems[0].games.size(), size_t(0));
  }

  // ---- the cursor stays on the same system by name when cards before it go away
  {
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx, ld);
    App app(std::move(cx), std::make_unique<CarouselScreen>(1), make_status_source("/nonexistent"));  // on snes
    change_and_close(app, "SortSystems", "alpha");  // Game Boy, Nintendo, Super Nintendo: snes moves to card 2
    CHECK_EQ(cards(app.cx), v({"gb", "nes", "snes"}));
    CHECK_EQ(app.cx.system_of(0), size_t(2));
    // the screen under the cursor is snes: A opens its list, which needs the games of the right system (1 game)
    app.tick(0.0f);
    app.handle(Action::A);
    CHECK(app.cx.systems[1].loaded);
    CHECK_EQ(app.cx.systems[1].games.size(), size_t(1));
    CHECK(!app.cx.systems[2].loaded);
    app.handle(Action::B);
    // the system under the cursor is hidden: the nearest one takes over (the next card, wrapping)
    change_and_close(app, "HiddenSystems", "snes");
    CHECK_EQ(cards(app.cx), v({"gb", "nes"}));
    app.tick(0.0f);
    app.handle(Action::A);  // after snes (last card) comes gb (first card)
    CHECK(app.cx.systems[2].loaded);
    CHECK(!app.cx.systems[0].loaded);
    app.handle(Action::B);
    // the games already read are kept (no second read of snes after it comes back)
    app.cx.systems[1].games[0].name = "marker";
    change_and_close(app, "HiddenSystems", "");
    CHECK_EQ(cards(app.cx), v({"gb", "nes", "snes"}));
    CHECK_EQ(app.cx.systems[1].games.size(), size_t(1));
    CHECK_EQ(app.cx.systems[1].games[0].name, std::string("marker"));
  }

  // ---- show empty systems
  {
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx, ld);
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    app.handle(Action::Start);
    app.cx.es.set_bool("LoadEmptySystems", true);
    col::set_reload_needed(true);
    app.handle(Action::Start);  // closes the whole stack
    CHECK_EQ(cards(app.cx), v({"nes", "snes", "gb", "empty"}));
    CHECK_EQ(app.cx.items[3].games, 0u);
    CHECK_EQ(app.cx.systems.size(), size_t(4));
    // and back
    change_and_close(app, "LoadEmptySystems", "false");
    CHECK_EQ(cards(app.cx), v({"nes", "snes", "gb"}));
  }

  // ---- a change that does not alter the list (a collection edited) reloads nothing visible
  {
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx, ld);
    App app(std::move(cx), std::make_unique<CarouselScreen>(1), make_status_source("/nonexistent"));
    app.cx.systems[1].games.push_back(Game());  // marks the vector: a swap would lose it
    app.cx.systems[1].loaded = true;
    app.handle(Action::Start);
    col::set_reload_needed(true);
    int before = ld->calls;
    app.handle(Action::B);
    CHECK_EQ(ld->calls, before + 1);  // it did look...
    CHECK_EQ(app.cx.systems[1].games.size(), size_t(1));  // ...and kept everything
    CHECK(!col::reload_needed());
  }

  // ---- no loader: the flag stays and the old notice appears; with a loader: no notice
  for (int with = 0; with < 2; ++with) {
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx, ld, with == 1);
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    app.handle(Action::Start);  // fake window
    app.handle(Action::A);      // the real collections settings on top of it
    app.handle(Action::Select);  // first handle builds
    col::set_reload_needed(true);
    app.cx.es.set_string("HiddenSystems", "nes");
    app.handle(Action::B);  // closes the collections settings: on_close
    if (with) {
      CHECK(app.cx.sv->toasts.items().empty());
      app.handle(Action::B);  // the last window: the reload happens now
      CHECK_EQ(cards(app.cx), v({"snes", "gb"}));
      CHECK(!col::reload_needed());
    } else {
      CHECK_EQ(app.cx.sv->toasts.items().size(), size_t(1));
      app.handle(Action::B);
      CHECK_EQ(cards(app.cx), v({"nes", "snes", "gb"}));
      CHECK(col::reload_needed());
    }
  }

  // ---- the real menu: toggling "SYSTEMS DISPLAYED" and closing everything
  {
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx, ld);
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    std::unique_ptr<Screen> sm = Registry::instance().make("menu.collections.systems", app.cx, "");
    CHECK(sm != nullptr);
    sm->handle(Action::Select, app.cx);
    sm->handle(Action::Down, app.cx);
    sm->handle(Action::A, app.cx);  // hides snes
    CHECK(col::reload_needed());
    CHECK_EQ(*app.cx.es.get("HiddenSystems"), std::string("snes"));
    app.handle(Action::Start);
    app.handle(Action::Start);
    CHECK_EQ(cards(app.cx), v({"nes", "gb"}));
  }

  // ---- a failing loader keeps everything and does not retry forever
  {
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx, ld);
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    ld->ok = false;
    int before = ld->calls;
    change_and_close(app, "HiddenSystems", "nes");
    CHECK_EQ(ld->calls, before + 1);
    CHECK_EQ(cards(app.cx), v({"nes", "snes", "gb"}));
    CHECK(!col::reload_needed());
    app.tick(0.0f);
    CHECK_EQ(ld->calls, before + 1);
    ld->ok = true;
  }

  // ---- a background job pending (its callback may hold a system index): the reload waits for it
  {
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx, ld);
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    bool done = false;
    app.cx.sv->jobs.post([] { std::this_thread::sleep_for(std::chrono::milliseconds(80)); }, [&done] { done = true; });
    int before = ld->calls;
    change_and_close(app, "HiddenSystems", "nes");
    CHECK_EQ(ld->calls, before);  // waiting
    CHECK_EQ(app.cx.items.size(), size_t(3));
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool reloaded = false;
    while (std::chrono::steady_clock::now() < until && !reloaded) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      app.drain_jobs(Clock::now());
      reloaded = app.cx.items.size() == 2;
    }
    CHECK(done);
    CHECK(reloaded);
    CHECK_EQ(cards(app.cx), v({"snes", "gb"}));
  }

  // ---- the counts of the new list are computed in the background (the wake-up still works after a reload), and known ones are kept
  {
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx, ld);
    App app(std::move(cx), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
    std::atomic<int> woke{0};
    app.set_count_wake([&woke] { ++woke; });  // installed before any worker exists, like the loop does
    auto counted = [&app] {  // waits until every system (shown or not) has its count
      auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      bool pending = true;
      while (pending && std::chrono::steady_clock::now() < until) {
        app.drain_counts();
        pending = false;
        for (const System& sy : app.cx.systems) pending = pending || sy.count == kCountPending;
        if (pending) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      return !pending;
    };
    change_and_close(app, "HiddenSystems", "nes");  // everything was still pending: the new worker counts it all
    CHECK_EQ(cards(app.cx), v({"snes", "gb"}));
    CHECK(counted());
    CHECK(woke > 0);
    CHECK_EQ(app.cx.items[0].games, 1u);
    CHECK_EQ(app.cx.items[1].games, 3u);
    CHECK_EQ(app.cx.systems[0].count, size_t(2));  // the hidden one was counted too
    change_and_close(app, "HiddenSystems", "");  // known counts are carried over: no number is pending, nothing to wait for
    CHECK_EQ(cards(app.cx), v({"nes", "snes", "gb"}));
    CHECK_EQ(app.cx.items[0].games, 2u);
    CHECK_EQ(app.cx.items[1].games, 1u);
    CHECK_EQ(app.cx.items[2].games, 3u);
  }

  // ---- speed: 80 systems, lazy loader, with the SD cache warm (the PC's file cache) -- printed, not asserted
  {
    std::string xml = "<systemList>";
    for (int i = 0; i < 80; ++i) {
      std::string n = "sys" + std::to_string(i);
      mkdir((g_tmp + "/roms/" + n).c_str(), 0755);
      touch(g_tmp + "/roms/" + n + "/g.rom");
      xml += "<system><name>" + n + "</name><fullname>System " + std::to_string(i) + "</fullname><path>/storage/roms/" + n + "</path><extension>.rom</extension><command>run</command></system>";
    }
    xml += "</systemList>";
    auto big = std::make_shared<Loader>();
    big->xml = xml;
    big->roms = g_tmp + "/roms";
    col::set_reload_needed(false);
    Ctx cx;
    setup(cx, big);
    App app(std::move(cx), std::make_unique<CarouselScreen>(10), make_status_source("/nonexistent"));
    double best = 1e9, sum = 0;
    std::vector<double> all;
    for (int r = 0; r < 20; ++r) {
      app.cx.es.set_string("HiddenSystems", r % 2 ? "sys3" : "sys4");
      col::set_reload_needed(true);
      auto t0 = std::chrono::steady_clock::now();
      app.tick(0.0f);
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      best = std::min(best, ms);
      sum += ms;
      all.push_back(ms);
    }
    std::sort(all.begin(), all.end());
    CHECK_EQ(app.cx.items.size(), size_t(79));
    std::fprintf(stderr, "reload of 80 systems (lazy loader, warm cache): best %.2f ms, median %.2f ms, worst %.2f ms, mean %.2f ms\n", best, all[all.size() / 2], all.back(), sum / 20);
  }

  std::string cmd = "rm -rf " + g_tmp;
  if (std::system(cmd.c_str()) != 0) {}
  return zt::done();
}
