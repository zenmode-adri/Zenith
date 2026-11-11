// Automatic covers (autocover.hpp): the gate (every condition blocks on its own), the options of the run (missing media only, nothing
// overwritten, pictures only, at most one look per day), never starting while the setting is off, stopping at once on any input,
// resuming in the next idle period, a toast only when something was added, and a run that ended by itself waiting for new input.
// The engine runs end to end with the fake `curl` of scraper_fake.hpp; the clock the controller sees is virtual (`v`).
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "autocover.hpp"
#include "check.hpp"
#include "menu_scraper.hpp"
#include "scraper_fake.hpp"

using namespace zf;
using namespace zf::autocover;
using namespace scrfake;
using std::chrono::seconds;

static const TimePoint T0 = Clock::now();

static Conditions ok_conditions() {
  Conditions c;
  c.enabled = true;
  c.idle = seconds(300);
  return c;
}

// the rig as a console that may start: wifi up, battery fine, setting on
static void ready(Rig& t) {
  t.cx.status.wifi = true;
  Battery b;
  b.percent = 80;
  b.charging = false;
  t.cx.status.battery = b;
  t.cx.es.set_bool(kKeyEnabled, true);
}

static void fast(Options& o) {  // the engine's pacing of the tests (the real one is 1 request per second)
  o.min_interval_ms = 0;
  o.follow_account_rate = false;
  o.busy_retry_ms = 20;
  o.flush_every = 2;
}

// drives the main thread like the device loop: jobs, then `poll` at the virtual time `v`
template <class F>
static bool pump(Rig& t, Controller& c, const TimePoint& v, F stop, int ms = 20000) {
  auto end = Clock::now() + std::chrono::milliseconds(ms);
  while (Clock::now() < end) {
    t.cx.sv->jobs.drain();
    c.poll(v);
    if (stop()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

static int first_number(const std::string& s) { return int(std::strtol(s.c_str(), nullptr, 10)); }

int main() {
  // ---- the gate: the base passes, each condition blocks on its own
  {
    CHECK(gate(ok_conditions()) == Block::None);
    Conditions c = ok_conditions();
    c.enabled = false;
    CHECK(gate(c) == Block::Off);
    c = ok_conditions();
    c.idle = seconds(119);
    CHECK(gate(c) == Block::Idle);
    c.idle = seconds(120);
    CHECK(gate(c) == Block::None);  // exactly two minutes is enough
    c = ok_conditions();
    c.game_running = true;
    CHECK(gate(c) == Block::Game);
    c = ok_conditions();
    c.busy = true;
    CHECK(gate(c) == Block::Busy);
    c = ok_conditions();
    c.network = false;
    CHECK(gate(c) == Block::Network);
    c = ok_conditions();
    Battery b;
    b.percent = 29;
    b.charging = false;
    c.battery = b;
    CHECK(gate(c) == Block::Battery);
    b.percent = 30;
    c.battery = b;
    CHECK(gate(c) == Block::None);
    b.percent = 5;
    b.charging = true;
    c.battery = b;
    CHECK(gate(c) == Block::None);  // charging: any level
    c.battery.reset();
    CHECK(gate(c) == Block::None);  // no battery at all
    c = ok_conditions();
    c.source_usable = false;
    CHECK(gate(c) == Block::Source);
    c = ok_conditions();
    c.systems = false;
    CHECK(gate(c) == Block::NoSystems);
  }

  // ---- the source: ScreenScraper needs this build's key, IGDB the user's id and secret, ArcadeDB nothing
  {
    Rig t;
    CHECK(source_usable(t.cx));  // the rig sets a developer login
    t.cx.es.set_string("Scraper", "IGDB");
    CHECK(!source_usable(t.cx));
    t.cx.es.set_string("IGDBClientID", "id");
    CHECK(!source_usable(t.cx));
    t.cx.es.set_string("IGDBSecret", "secret");
    CHECK(source_usable(t.cx));
    t.cx.es.set_string("Scraper", "ArcadeDB");
    t.cx.es.set_string("IGDBClientID", "");
    CHECK(source_usable(t.cx));
  }

  // ---- the options of the run: missing media, nothing overwritten, pictures only, one look per day at least, no Busy
  {
    Rig t;
    ready(t);
    t.cx.es.set_bool("ScrapeOverWrite", true);  // the user's "replace existing media" must not leak in
    t.cx.es.set_bool("ScrapeVideos", true);
    t.cx.es.set_bool("ScrapeManual", true);
    t.cx.es.set_string("ZfScraperFilter", "all");  // nor the menu's "all games"
    t.cx.es.set_string("RecentlyScrappedFilter", "0");  // "no" means none in the menu: a day here
    Options o = make_options(t.cx);
    CHECK(o.filter == Options::Filter::MissingAny);
    CHECK(!o.overwrite_media && !o.overwrite_names && !o.overwrite_desc);
    CHECK(!o.wish.video && !o.wish.manual && !o.wish.map && !o.wish.fanart && !o.wish.boxback && !o.wish.titleshot);
    CHECK(!o.wish.image_src.empty() && !o.wish.thumb_src.empty());  // the user's picture choices stay
    CHECK_EQ(o.skip_days, 1);
    CHECK(o.background);
    CHECK(o.only_game.empty());
    CHECK(o.systems == scraper_run_options(t.cx).systems);  // the user's choice of systems, as the menu's own run
    t.cx.es.set_string("RecentlyScrappedFilter", "5");  // 3 months: the user's longer wait is kept
    CHECK_EQ(make_options(t.cx).skip_days, 90);
  }

  // ---- never runs while the setting is off, however long it is left alone
  {
    Rig t;
    ready(t);
    t.cx.es.set_bool(kKeyEnabled, false);
    t.rom("a.sfc");
    t.known("a");
    t.systems();
    Controller c(t.cx, T0);
    for (int m = 1; m <= 600; m += 30) c.poll(T0 + std::chrono::minutes(m));
    CHECK(!c.running());
    CHECK_EQ(c.runs_started(), 0u);
    CHECK(c.last_block() == Block::Off);
    CHECK(!c.deadline().has_value());  // nothing to wake for
    CHECK(t.requests().empty());
    CHECK(t.cx.sv->toasts.items().empty());
  }

  // ---- each condition blocks the controller in turn; when they are all clear it starts, with the missing-media options
  {
    Rig t;
    ready(t);
    t.rom("a.sfc");
    t.known("a");
    t.systems();
    bool game = false, busy = false;
    Controller c(t.cx, T0);
    c.game_running = [&] { return game; };
    c.busy = [&] { return busy; };
    Options seen;
    c.tune = [&](Options& o) {
      seen = o;
      fast(o);
    };
    TimePoint v = T0 + seconds(60);
    c.poll(v);
    CHECK_EQ(c.runs_started(), 0u);  // only a minute since the last input: not yet
    CHECK(c.deadline().has_value() && *c.deadline() == T0 + seconds(120));
    v = T0 + seconds(121);
    game = true;
    c.poll(v);
    CHECK(c.last_block() == Block::Game && !c.running());
    v += seconds(31);
    game = false;
    busy = true;
    c.poll(v);
    CHECK(c.last_block() == Block::Busy && !c.running());
    v += seconds(31);
    busy = false;
    t.cx.status.wifi = false;
    c.poll(v);
    CHECK(c.last_block() == Block::Network && !c.running());
    v += seconds(31);
    t.cx.status.wifi = true;
    Battery b;
    b.percent = 20;
    b.charging = false;
    t.cx.status.battery = b;
    c.poll(v);
    CHECK(c.last_block() == Block::Battery && !c.running());
    v += seconds(31);
    t.cx.status.battery->charging = true;
    t.cx.es.set_string("ZfScraperSkip", "snes");  // every system left out
    c.poll(v);
    CHECK(c.last_block() == Block::NoSystems && !c.running());
    v += seconds(31);
    t.cx.es.set_string("ZfScraperSkip", "");
    t.cx.es.set_string("Scraper", "IGDB");  // no client id / secret
    c.poll(v);
    CHECK(c.last_block() == Block::Source && !c.running());
    CHECK_EQ(c.runs_started(), 0u);
    CHECK(t.requests().empty());  // not one request while blocked
    v += seconds(31);
    t.cx.es.set_string("Scraper", "ScreenScraper");
    c.poll(v);
    CHECK_EQ(c.runs_started(), 1u);  // all clear
    CHECK(c.running());
    CHECK(seen.filter == Options::Filter::MissingAny);
    CHECK(seen.background && !seen.overwrite_media);
    CHECK(pump(t, c, v, [&] { return !c.running(); }));
    CHECK(c.last_phase() == Phase::Finished);
    CHECK_EQ(t.cx.sv->toasts.items().size(), 1u);  // "1 COVERS ADDED", the run's own toasts are off
    CHECK_EQ(first_number(t.cx.sv->toasts.items().back().text), 1);
    CHECK(t.cx.sv->toasts.items().back().text.find("COVERS ADDED") != std::string::npos);
  }

  // ---- missing media only: what is there is not touched, and only the games that lack pictures are looked up
  {
    Rig t;
    ready(t);
    t.rom("full.sfc");
    t.rom("part.sfc");
    t.rom("miss.sfc");
    t.known("full");
    t.known("part");
    t.known("miss");
    put(t.roms() + "/images/full-image.png", "\x89PNG\r\n\x1a\nFULLIMG");
    put(t.roms() + "/images/full-thumb.png", "\x89PNG\r\n\x1a\nFULLTHUMB");
    put(t.roms() + "/images/full-marquee.png", "\x89PNG\r\n\x1a\nFULLLOGO");
    put(t.roms() + "/images/part-image.png", "\x89PNG\r\n\x1a\nMINE");
    put(t.roms() + "/gamelist.xml",
        "<gameList>\n<game><path>./full.sfc</path><name>Full Name</name><image>./images/full-image.png</image>"
        "<thumbnail>./images/full-thumb.png</thumbnail><marquee>./images/full-marquee.png</marquee></game>\n"
        "<game><path>./part.sfc</path><name>Part Name</name><image>./images/part-image.png</image></game>\n</gameList>\n");
    t.systems();
    Controller c(t.cx, T0);
    c.tune = fast;
    TimePoint v = T0 + seconds(130);
    size_t total = 0;
    CHECK(pump(t, c, v, [&] {
      if (c.run()) total = c.run()->stats().total;
      return c.runs_started() > 0 && !c.running();
    }));
    CHECK_EQ(total, 2u);  // part and miss; full has nothing missing
    CHECK(c.last_phase() == Phase::Finished);
    CHECK(slurp(t.roms() + "/images/part-image.png").find("MINE") != std::string::npos);
    CHECK(slurp(t.roms() + "/images/full-image.png").find("FULLIMG") != std::string::npos);
    CHECK(fs::exists(t.roms() + "/images/part-thumb.jpg") && fs::exists(t.roms() + "/images/miss-image.png"));
    CHECK(!fs::exists(t.roms() + "/videos"));  // pictures only
    std::string gl = t.gamelist();
    CHECK(gl.find("<name>Part Name</name>") != std::string::npos);  // names stay
    CHECK(gl.find("<name>Full Name</name>") != std::string::npos);
    CHECK_EQ(t.cx.sv->toasts.items().size(), 1u);
    CHECK_EQ(first_number(t.cx.sv->toasts.items().back().text), 2);  // games that got pictures, not files
    // the run ended by itself: nothing starts again until someone presses something and the count of idle time restarts
    TimePoint later = v + std::chrono::hours(5);
    for (int i = 0; i < 5; ++i) c.poll(later + seconds(40 * i));
    CHECK_EQ(c.runs_started(), 1u);
    CHECK(!c.deadline().has_value());
    c.input(later + seconds(300));
    CHECK(c.deadline().has_value());
    c.poll(later + seconds(300) + seconds(60));
    CHECK_EQ(c.runs_started(), 1u);  // not yet two minutes
    TimePoint w = later + seconds(300) + seconds(121);
    CHECK(pump(t, c, w, [&] { return c.runs_started() == 2u && !c.running(); }));
    CHECK(c.last_phase() == Phase::Finished);
    CHECK_EQ(t.cx.sv->toasts.items().size(), 1u);  // everything was looked at less than a day ago: nothing to do, no toast
  }

  // ---- no toast when nothing was added (games the server does not know)
  {
    Rig t;
    ready(t);
    t.rom("x.sfc");  // no answer prepared: not found
    t.systems();
    Controller c(t.cx, T0);
    c.tune = fast;
    TimePoint v = T0 + seconds(130);
    CHECK(pump(t, c, v, [&] { return c.runs_started() > 0 && !c.running(); }));
    CHECK(c.last_phase() == Phase::Finished);
    CHECK(t.cx.sv->toasts.items().empty());
  }

  // ---- any input stops the run at once; the next idle period resumes with the games that are left
  {
    Rig t;
    ready(t);
    const char* names[] = {"a", "b", "c", "d", "e", "f"};
    for (const char* n : names) {
      t.rom(std::string(n) + ".sfc");
      t.known(n);
    }
    t.systems();
    Controller c(t.cx, T0);
    c.tune = [](Options& o) {
      fast(o);
      o.min_interval_ms = 150;  // slow enough to be caught in the middle
    };
    TimePoint v = T0 + seconds(130);
    CHECK(pump(t, c, v, [&] { return c.running() && c.run()->stats().done >= 1; }));
    CHECK(c.running());
    c.input(v);  // a key
    CHECK(c.run() && (c.run()->phase() == Phase::Stopping || c.run()->phase() == Phase::Cancelled));  // no longer Running, before any poll
    CHECK(pump(t, c, v, [&] { return !c.running(); }));
    CHECK(c.last_phase() == Phase::Cancelled);
    CHECK_EQ(c.runs_started(), 1u);
    size_t stamped = count_of(t.gamelist(), "<scrap ");
    CHECK(stamped >= 1 && stamped < 6);  // what was done is saved, the rest waits
    for (int i = 0; i < 5; ++i) c.poll(v + seconds(10 * i));  // still being used: stays stopped
    CHECK_EQ(c.runs_started(), 1u);
    TimePoint w = v + seconds(121);
    CHECK(pump(t, c, w, [&] { return c.runs_started() == 2u && !c.running(); }));
    CHECK(c.last_phase() == Phase::Finished);
    CHECK_EQ(count_of(t.gamelist(), "<scrap "), 6u);
    int sum = 0;
    for (const Toast& tt : t.cx.sv->toasts.items()) sum += first_number(tt.text);
    CHECK_EQ(sum, 6);  // each game counted once, in the run that added it
  }

  // ---- a game starting stops the run too (as many games and the same pace as the key case: caught in the middle, not after the end)
  {
    Rig t;
    ready(t);
    for (const char* n : {"a", "b", "c", "d", "e", "f", "g", "h"}) {
      t.rom(std::string(n) + ".sfc");
      t.known(n);
    }
    t.systems();
    Controller c(t.cx, T0);
    c.tune = [](Options& o) {
      fast(o);
      o.min_interval_ms = 300;
    };
    TimePoint v = T0 + seconds(130);
    CHECK(pump(t, c, v, [&] { return c.running() && c.run()->stats().done >= 1; }));
    CHECK(c.running());
    c.game_started();
    CHECK(pump(t, c, v, [&] { return !c.running(); }));
    CHECK(c.last_phase() == Phase::Cancelled);
    CHECK_EQ(c.runs_started(), 1u);  // back from the game: no new run at once, the idle wait starts again
    for (int i = 0; i < 5; ++i) c.poll(v + seconds(20 * i));
    CHECK_EQ(c.runs_started(), 1u);
  }
  return zt::done();
}
