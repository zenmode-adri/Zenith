// Screensaver / inactivity (screensaver.* + the policy in oscontract.*): black / dim / suspend behaviours, the overlay, waking
// (first key dropped, unmapped button, coming back from a suspend), no start while a game, a Busy or a job is running (and no
// spinning deadline then), and the wake-up budget while it is on (black: none; dim with a clock: one per minute).
// Time is passed in by the test and the suspend probe is injected: nothing waits.
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "app.hpp"
#include "check.hpp"
#include "oscontract.hpp"
#include "screensaver.hpp"
#include "status.hpp"

using namespace zf;
using namespace zf::oscontract;
using std::string;
using std::vector;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

string d;

string slurp(const string& p) {
  string s;
  read_file(p, s);
  return s;
}

void write_text(const string& path, const string& t) { std::ofstream(path) << t; }

void sh(const string& c) {
  if (std::system(c.c_str()) != 0) {
  }
}

struct DummyScreen : Screen {
  Transition handle(Action, Ctx&) override { return Transition::none(); }
  void draw(Ctx&, std::vector<Op>&) override {}
};

long long g_offset_ns = 0;  // CLOCK_BOOTTIME - CLOCK_MONOTONIC as the test wants it
long long probe() { return g_offset_ns; }

std::shared_ptr<Services> fresh() {
  reset_for_test();
  g_offset_ns = 5000000000LL;
  screensaver::set_clock_offset_probe(probe);
  sh("rm -rf " + d + "/bl " + d + "/tmp " + d + "/scripts " + d + "/pm-suspend");
  sh("mkdir -p " + d + "/bl/a " + d + "/tmp " + d + "/scripts/screensaver-start " + d + "/scripts/screensaver-stop");
  write_text(d + "/bl/a/bl_power", "0\n");
  Paths& p = paths();
  p.backlight_dir = d + "/bl";
  p.ready_file = d + "/ready";
  p.kill_file = d + "/kill";
  p.tmp_dir = d + "/tmp";
  p.suspend_helper = d + "/pm-suspend";
  set_script_dirs({d + "/scripts"});
  auto sv = std::make_shared<Services>();
  sv->cmd.dry_run = true;
  startup(sv, false);
  install_signals(false);
  return sv;
}

EsSettings cfg(const string& behavior, long ms = 1000, bool clock = true) {
  string x = "<config><int name=\"ScreenSaverTime\" value=\"" + std::to_string(ms) + "\" /><bool name=\"DrawClock\" value=\"" + (clock ? "true" : "false") +
             "\" />";
  if (!behavior.empty()) x += "<string name=\"ScreenSaverBehavior\" value=\"" + behavior + "\" />";
  return EsSettings::from_text(x + "</config>");
}

bool log_has(const Services& sv, const string& needle) {
  for (const string& l : sv.cmd.log)
    if (l.find(needle) != string::npos) return true;
  return false;
}

}  // namespace

int main() {
  char tpl[] = "/tmp/zf-idle-XXXXXX";
  CHECK(::mkdtemp(tpl) != nullptr);
  d = tpl;
  vector<Action> none;

  // ---- pure pieces
  CHECK(screensaver::mode_for_behavior("dim") == screensaver::Mode::Dim);
  CHECK(screensaver::mode_for_behavior("black") == screensaver::Mode::Black);
  CHECK(screensaver::mode_for_behavior("slideshow") == screensaver::Mode::Black);  // not ported: ES falls back to black too
  CHECK(screensaver::mode_for_behavior("random video") == screensaver::Mode::Black);
  CHECK(screensaver::mode_for_behavior("") == screensaver::Mode::Black);
  CHECK_EQ(screensaver::ms_to_next_minute(0.0), 60020L);
  CHECK_EQ(screensaver::ms_to_next_minute(59.5), 520L);
  CHECK_EQ(screensaver::ms_to_next_minute(30.0), 30020L);
  {
    screensaver::ResumeDetector r;
    CHECK(!r.check(100));                        // first call: only the base
    CHECK(!r.check(100 + 999LL * 1000000));      // under the threshold
    CHECK(!r.peek(100 + 999LL * 1000000));
    CHECK(r.peek(100 + 3000LL * 1000000));       // peek does not consume
    CHECK(r.peek(100 + 3000LL * 1000000));
    CHECK(r.check(100 + 3000LL * 1000000));      // consumed and re-based
    CHECK(!r.check(100 + 3000LL * 1000000));
    r.reset();
    CHECK(!r.peek(1LL << 40));
  }
  {  // the overlay: black is opaque, dim is ES's 0xA0
    Ctx cx;
    cx.w = 640;
    cx.h = 480;
    std::vector<Op> ops;
    screensaver::set_mode(screensaver::Mode::Off);
    screensaver::draw(cx, ops);
    CHECK(ops.empty());
    screensaver::set_mode(screensaver::Mode::Black);
    screensaver::draw(cx, ops);
    CHECK_EQ(ops.size(), 1ul);
    CHECK(ops[0].kind == Op::Rect && ops[0].x == 0 && ops[0].y == 0 && ops[0].w == 640 && ops[0].h == 480);
    CHECK(ops[0].color[0] == 0 && ops[0].color[1] == 0 && ops[0].color[2] == 0 && ops[0].color[3] == 255);
    ops.clear();
    screensaver::set_mode(screensaver::Mode::Dim);
    screensaver::draw(cx, ops);
    CHECK_EQ(int(ops[0].color[3]), 0xA0);
    screensaver::set_mode(screensaver::Mode::Off);
  }

  // ---- black: panel off, frozen UI, no wake-up at all while it is on
  {
    auto sv = fresh();
    EsSettings es = cfg("black");
    TimePoint t0 = Clock::now();
    input(t0);
    before_acts(es, t0 + milliseconds(1000), none);
    CHECK(idle().active());
    CHECK(screensaver::mode() == screensaver::Mode::Black);
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("4\n"));
    CHECK(take_redraw());
    CHECK(!saver_deadline(es, t0 + seconds(5)));    // black: the loop sleeps until an event
    CHECK(!idle_deadline(es));                       // and the inactivity timer has no deadline either
    // the frame is one black rectangle, whatever the screens draw
    Ctx cx;
    cx.w = 480;
    cx.h = 320;
    App app(std::move(cx), std::make_unique<DummyScreen>(), make_status_source("/nonexistent"));
    app.cx.sv->toasts.push("hello", 5000);
    const std::vector<Op>& ops = app.render();
    CHECK_EQ(ops.size(), 1ul);
    CHECK_EQ(int(ops[0].color[3]), 255);
    // the first key wakes the panel and is NOT acted upon
    vector<Action> acts = {Action::A};
    before_acts(es, t0 + seconds(10), acts);
    CHECK(acts.empty());
    CHECK(!idle().active());
    CHECK(screensaver::mode() == screensaver::Mode::Off);
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("0\n"));
    CHECK(take_redraw());
    auto dl = idle_deadline(es);
    CHECK(dl && *dl == t0 + seconds(11));  // a fresh count from the waking key
  }

  // ---- dim: the light stays, a clock needs one wake-up per minute, toasts are not drawn
  {
    auto sv = fresh();
    EsSettings es = cfg("dim");
    TimePoint t0 = Clock::now();
    input(t0);
    before_acts(es, t0 + milliseconds(1000), none);
    CHECK(idle().active() && screensaver::mode() == screensaver::Mode::Dim);
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("0\n"));
    TimePoint now = Clock::now();
    auto dl = saver_deadline(es, now);
    CHECK(dl && *dl > now && *dl <= now + milliseconds(60020));  // at most one per minute
    CHECK(!saver_deadline(cfg("dim", 1000, false), now));        // no clock: no wake-up at all
    Ctx cx;
    cx.w = 480;
    cx.h = 320;
    App app(std::move(cx), std::make_unique<DummyScreen>(), make_status_source("/nonexistent"));
    app.cx.sv->toasts.push("hello", 5000);
    const std::vector<Op>& ops = app.render();
    CHECK_EQ(ops.size(), 1ul);  // screens draw nothing here, the toast is hidden, the overlay is the only operation
    CHECK_EQ(int(ops[0].color[3]), 0xA0);
    screensaver::set_mode(screensaver::Mode::Off);
    screensaver_stop();
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("0\n"));
  }

  // ---- an unmapped button is activity: it postpones the start and wakes an active screensaver
  {
    auto sv = fresh();
    EsSettings es = cfg("black");
    TimePoint t0 = Clock::now();
    input(t0);
    before_acts(es, t0 + milliseconds(900), none, true);  // raw input at 900 ms
    auto dl = idle_deadline(es);
    CHECK(dl && *dl == t0 + milliseconds(1900));
    before_acts(es, t0 + milliseconds(1500), none);
    CHECK(!idle().active());
    before_acts(es, t0 + milliseconds(1900), none);
    CHECK(idle().active());
    vector<Action> acts;  // nothing mapped, but a button went down
    before_acts(es, t0 + seconds(5), acts, true);
    CHECK(!idle().active());
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("0\n"));
  }

  // ---- coming back from a suspend (sleep.sh lit the panel): the screensaver ends, the count restarts, no key needed
  {
    auto sv = fresh();
    EsSettings es = cfg("black");
    TimePoint t0 = Clock::now();
    input(t0);
    before_acts(es, t0 + milliseconds(10), none);  // records the clock base
    CHECK(!resume_pending());
    before_acts(es, t0 + milliseconds(1000), none);
    CHECK(idle().active());
    CHECK(!resume_pending());
    g_offset_ns += 40LL * 1000000000LL;  // 40 s of suspend: BOOTTIME ran on, MONOTONIC did not
    CHECK(resume_pending());
    vector<Action> acts = {Action::Down};  // whatever arrives together with the resume is swallowed like a waking key
    before_acts(es, t0 + milliseconds(1100), acts);
    CHECK(!idle().active() && acts.empty());
    CHECK(!resume_pending());  // consumed
    auto dl = idle_deadline(es);
    CHECK(dl && *dl == t0 + milliseconds(2100));
    // resume while NOT active: only the count restarts (a minute of idle before the suspend does not count)
    g_offset_ns += 40LL * 1000000000LL;
    before_acts(es, t0 + milliseconds(1900), none);
    CHECK(!idle().active());
    dl = idle_deadline(es);
    CHECK(dl && *dl == t0 + milliseconds(2900));
  }

  // ---- never starts over a game, a Busy or a running job; never leaves a deadline in the past
  {
    auto sv = fresh();
    EsSettings es = cfg("black");
    TimePoint t0 = Clock::now();
    input(t0);
    {
      Busy b;  // the scraper / a download
      before_acts(es, t0 + milliseconds(1000), none);
      CHECK(!idle().active());
      auto dl = idle_deadline(es);
      CHECK(dl && *dl == t0 + milliseconds(6000));  // looked again in 5 s, not "now" (the loop would spin)
      before_acts(es, t0 + milliseconds(6000), none);
      CHECK(!idle().active());
      dl = idle_deadline(es);
      CHECK(dl && *dl == t0 + milliseconds(11000));
    }
    before_acts(es, t0 + milliseconds(11000), none);
    CHECK(idle().active());
    screensaver_stop();
    input(t0 + seconds(20));
    game_started("/r/x.gb", "X");
    before_acts(es, t0 + seconds(30), none);
    CHECK(!idle().active());
    game_ended();
    // a long background job (Jobs::post) holds it off too
    std::atomic<bool> go{false};
    sv->jobs.post([&go] {
      while (!go) std::this_thread::sleep_for(milliseconds(1));
    });
    input(t0 + seconds(40));
    before_acts(es, t0 + seconds(42), none);
    CHECK(!idle().active());
    go = true;
    for (int i = 0; i < 2000 && !sv->jobs.idle(); ++i) ::usleep(1000);
    CHECK(sv->jobs.idle());
    before_acts(es, t0 + seconds(48), none);  // the deferred look: now it may start
    CHECK(idle().active());
    screensaver_stop();
  }

  // ---- suspend type: suspends (when the system can), leaves no screensaver and no dark panel behind
  {
    auto sv = fresh();
    EsSettings es = cfg("suspend");
    TimePoint t0 = Clock::now();
    input(t0);
    before_acts(es, t0 + milliseconds(1000), none);  // no pm-suspend: ES shows black instead
    CHECK(idle().active() && screensaver::mode() == screensaver::Mode::Black);
    CHECK(!log_has(*sv, "systemctl suspend"));
    screensaver_stop();
    write_text(d + "/pm-suspend", "");
    input(t0 + seconds(5));
    before_acts(es, t0 + seconds(6), none);
    CHECK(log_has(*sv, "systemctl suspend"));
    CHECK(!idle().active() && screensaver::mode() == screensaver::Mode::Off);
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("0\n"));  // sleep.sh owns the panel here
    auto dl = idle_deadline(es);
    CHECK(dl && *dl > Clock::now());  // the count restarted
  }

  // ---- video / slideshow types are not ported: black
  {
    auto sv = fresh();
    EsSettings es = cfg("slideshow");
    TimePoint t0 = Clock::now();
    input(t0);
    before_acts(es, t0 + milliseconds(1000), none);
    CHECK(idle().active() && screensaver::mode() == screensaver::Mode::Black);
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("4\n"));
    screensaver_stop();
  }

  screensaver::set_clock_offset_probe(nullptr);
  reset_for_test();
  sh("rm -rf " + d);
  return zt::done();
}
