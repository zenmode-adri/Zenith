// oscontract: waits, ready/kill/bl_power files, ES script events, quit modes, screensaver timer, game state, /launch lookup.
// Everything runs in a temp dir; system commands go to a dry-run runner; no sound, no real shutdown.
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "app.hpp"
#include "check.hpp"
#include "oscontract.hpp"
#include "screensaver.hpp"

using namespace zf;
using namespace zf::oscontract;
using std::string;
using std::vector;
using std::chrono::milliseconds;

namespace {

string d;  // temp dir

string slurp(const string& p) {
  string s;
  read_file(p, s);
  return s;
}

void write_exec(const string& path, const string& body) {
  std::ofstream(path) << "#!/bin/sh\n" << body;
  ::chmod(path.c_str(), 0755);
}

void write_text(const string& path, const string& t) { std::ofstream(path) << t; }

bool wait_content(const string& path, const string& needle, int ms = 3000) {
  for (int i = 0; i < ms; ++i) {
    if (slurp(path).find(needle) != string::npos) return true;
    ::usleep(1000);
  }
  return false;
}

struct DummyScreen : Screen {
  Transition handle(Action, Ctx&) override { return Transition::none(); }
  void draw(Ctx&, std::vector<Op>&) override {}
};

void sh(const string& c) {
  if (std::system(c.c_str()) != 0) {
  }
}

// fresh state + paths in the temp dir; backlight files hold "4"
std::shared_ptr<Services> fresh() {
  reset_for_test();
  sh("rm -rf " + d + "/bl " + d + "/tmp " + d + "/ready " + d + "/kill " + d + "/m_* " + d + "/scripts");
  sh("mkdir -p " + d + "/bl/a " + d + "/bl/b " + d + "/tmp");
  write_text(d + "/bl/a/bl_power", "4\n");
  write_text(d + "/bl/b/bl_power", "4\n");
  Paths& p = paths();
  p.backlight_dir = d + "/bl";
  p.ready_file = d + "/ready";
  p.kill_file = d + "/kill";
  p.tmp_dir = d + "/tmp";
  p.suspend_helper = d + "/pm-suspend";
  auto sv = std::make_shared<Services>();
  sv->cmd.dry_run = true;  // shutdown, systemctl, killall: only logged
  return sv;
}

bool log_has(const Services& sv, const string& needle) {
  for (const string& l : sv.cmd.log)
    if (l.find(needle) != string::npos) return true;
  return false;
}

bool dry_has(const string& needle) {
  for (const string& l : dry_log())
    if (l.find(needle) != string::npos) return true;
  return false;
}

}  // namespace

int main() {
  char tpl[] = "/tmp/zf-os-XXXXXX";
  CHECK(::mkdtemp(tpl) != nullptr);
  d = tpl;

  // ---- text helpers
  CHECK_EQ(json_escape("a\"b\\c\nd\x01"), string("a\\\"b\\\\c\\nd\\u0001"));
  CHECK_EQ(sanitize_notice("  Suspendiendo...\r\n"), string("Suspendiendo..."));
  CHECK_EQ(sanitize_notice("a\x01\x02" "b"), string("a  b"));
  CHECK_EQ(sanitize_notice(string(500, 'x')).size(), 200ul);
  {
    string s = string(199, 'x') + "\xc3\xa9";  // 'e acute' straddles the 200-byte limit
    CHECK_EQ(sanitize_notice(s).size(), 199ul);  // never cut in the middle of a UTF-8 sequence
  }

  // ---- bl_power, set_kill, startup
  {
    auto sv = fresh();
    CHECK_EQ(backlight(0), 2);
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("0\n"));
    CHECK_EQ(slurp(d + "/bl/b/bl_power"), string("0\n"));
    CHECK_EQ(backlight(4), 2);
    CHECK_EQ(slurp(d + "/bl/b/bl_power"), string("4\n"));
    CHECK(set_kill("retroarch"));
    CHECK_EQ(slurp(d + "/kill"), string("retroarch\n"));  // same bytes as `set_kill set` of 001-functions
    CHECK_EQ(kill_target(), string("retroarch"));

    startup(sv, false);
    install_signals(false);  // the real one also sends SIGTERM to us
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("0\n"));
    CHECK_EQ(slurp(d + "/kill"), string("emulationstation\n"));
    CHECK(idle().stamped());
    CHECK(!quit_requested());
    ::raise(SIGHUP);  // SIGHUP: clean quit request
    CHECK(quit_requested());
    CHECK_EQ(int(quit_mode()), int(Mode::Quit));
    backlight(0);
  }
  {  // dry-run: nothing is written, everything is recorded
    auto sv = fresh();
    startup(sv, true);
    install_signals(false);
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("4\n"));
    CHECK(!file_exists(d + "/kill"));
    CHECK(dry_has("bl_power 0") && dry_has("set_kill emulationstation"));
    on_first_frame();
    CHECK(!file_exists(d + "/ready") && dry_has("create ") && dry_has("event start"));
    finish();
    CHECK(dry_has("event quit"));
  }

  // ---- bounded waits
  {
    fresh();
    string f = d + "/waited";
    sh("rm -rf " + f);
    CHECK(!wait_for_path(f, 0));
    auto t0 = std::chrono::steady_clock::now();
    CHECK(!wait_for_path(f, 200));  // times out
    long ms = long(std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - t0).count());
    CHECK(ms >= 190 && ms < 600);
    std::thread t([&] {
      ::usleep(120000);
      write_text(f, "x");
    });
    t0 = std::chrono::steady_clock::now();
    CHECK(wait_for_path(f, 3000));  // woken by inotify well before the timeout
    ms = long(std::chrono::duration_cast<milliseconds>(std::chrono::steady_clock::now() - t0).count());
    CHECK(ms >= 100 && ms < 600);
    t.join();
    CHECK(wait_for_path(f, 0));  // already there

    // a socket is required for the display: a regular file does not count, a bound unix socket does
    string sock = d + "/wayland-1";
    write_text(sock, "not a socket");
    CHECK(!wait_for_path(sock, 100, true));
    ::unlink(sock.c_str());
    std::thread t2([&] {
      ::usleep(100000);
      int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
      sockaddr_un a{};
      a.sun_family = AF_UNIX;
      std::snprintf(a.sun_path, sizeof a.sun_path, "%s", sock.c_str());
      ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
      ::usleep(300000);
      ::close(s);
    });
    CHECK(wait_for_path(sock, 3000, true));
    t2.join();

    // parent directory not there yet: still works (falls back to short steps)
    string deep = d + "/later/file";
    sh("rm -rf " + d + "/later");
    std::thread t3([&] {
      ::usleep(100000);
      sh("mkdir -p " + d + "/later");
      ::usleep(60000);
      write_text(deep, "x");
    });
    CHECK(wait_for_path(deep, 3000));
    t3.join();

    // the environment wrappers
    ::unsetenv("ES_WAIT_WAYLAND_SOCKET");
    ::unsetenv("ES_WAIT_ROMS_READY");
    CHECK(wait_display() && wait_roms());  // not requested: nothing to wait for
    ::setenv("ES_WAIT_ROMS_READY", (d + "/roms-ready").c_str(), 1);
    sh("rm -f " + d + "/roms-ready");
    std::thread t4([&] {
      ::usleep(100000);
      write_text(d + "/roms-ready", "");
    });
    CHECK(wait_roms());
    t4.join();
    ::unsetenv("ES_WAIT_ROMS_READY");
  }

  // ---- events and the ready file
  {
    auto sv = fresh();
    startup(sv, false);
    install_signals(false);
    string s = d + "/scripts";
    sh("mkdir -p " + s + "/start " + s + "/quit " + s + "/reboot " + s + "/shutdown " + s + "/game-selected " + s + "/screensaver-start " + s +
       "/screensaver-stop " + s + "/sleep " + s + "/wake");
    set_script_dirs({s});
    write_exec(s + "/start/10.sh", "echo \"start $#\" >> " + d + "/m_start\n");
    write_exec(s + "/quit/10.sh", "sleep 0.2; echo \"quit [$*]\" >> " + d + "/m_quit\n");
    write_exec(s + "/reboot/10.sh", "echo reboot >> " + d + "/m_reboot\n");
    write_exec(s + "/shutdown/10.sh", "echo shutdown >> " + d + "/m_shutdown\n");
    write_exec(s + "/game-selected/10.sh", "echo \"$*\" >> " + d + "/m_sel\n");
    write_exec(s + "/screensaver-start/10.sh", "echo \"$*\" >> " + d + "/m_ssstart\n");
    write_exec(s + "/screensaver-stop/10.sh", "echo stop >> " + d + "/m_ssstop\n");
    write_exec(s + "/sleep/10.sh", "echo sleep >> " + d + "/m_sleep\n");
    write_exec(s + "/wake/10.sh", "echo wake >> " + d + "/m_wake\n");
    write_exec(s + "/any.sh", "echo \"$1\" >> " + d + "/m_any\n");  // single-script style: event name first

    on_first_frame();
    CHECK(file_exists(d + "/ready"));
    CHECK(wait_content(d + "/m_start", "start 0"));
    on_first_frame();  // idempotent
    ::usleep(150000);
    CHECK_EQ(slurp(d + "/m_start"), string("start 0\n"));
    CHECK(wait_content(d + "/m_any", "start"));

    // `quit` is synchronous (the script sleeps 0.2 s and has written when fire_event returns); the others are not waited for
    auto ran = fire_event("quit", {"reboot"});
    CHECK_EQ(ran.size(), 2ul);  // event dir + single script
    CHECK_EQ(slurp(d + "/m_quit"), string("quit [reboot]\n"));

    fire_game_selected("nes", "/r/a b.nes", "Name X");
    CHECK(wait_content(d + "/m_sel", "nes /r/a b.nes Name X"));

    // `*-selected` events fire on every cursor move: "nobody listens" is cached for 5 s, a script added later is not seen
    // until the cache is dropped
    ::unlink((s + "/any.sh").c_str());
    CHECK(fire_event("system-selected", {"x"}).empty());
    sh("mkdir -p " + s + "/system-selected");
    write_exec(s + "/system-selected/10.sh", "true\n");
    CHECK(fire_event("system-selected", {"x"}).empty());  // cached
    CHECK(fire_event("theme-changed", {"x"}).empty());
    set_script_dirs({s});                                 // drops the cache
    CHECK_EQ(fire_event("system-selected", {"x"}).size(), 1ul);
  }

  // ---- quit modes and finish
  {
    for (int pass = 0; pass < 4; ++pass) {
      auto sv = fresh();
      startup(sv, false);
      install_signals(false);
      string s = d + "/scripts";
      sh("mkdir -p " + s + "/quit " + s + "/reboot " + s + "/shutdown");
      set_script_dirs({s});
      write_exec(s + "/quit/10.sh", "echo \"quit [$*]\" >> " + d + "/m_quit\n");
      write_exec(s + "/reboot/10.sh", "echo reboot >> " + d + "/m_reboot\n");
      write_exec(s + "/shutdown/10.sh", "echo shutdown >> " + d + "/m_shutdown\n");
      on_first_frame();
      CHECK(file_exists(d + "/ready"));
      int hooked = 0;
      on_finish([&] { ++hooked; });
      Mode m[] = {Mode::Reboot, Mode::Shutdown, Mode::Restart, Mode::Quit};
      quit(m[pass]);
      CHECK(quit_requested());
      CHECK_EQ(int(quit_mode()), int(m[pass]));
      CHECK(sv->cmd.log.empty());  // nothing is executed before the window is gone
      if (pass == 0) {
        CHECK_EQ(slurp(d + "/m_quit"), string("quit [reboot]\n"));
        CHECK(wait_content(d + "/m_reboot", "reboot"));
      }
      if (pass == 1) {
        CHECK_EQ(slurp(d + "/m_quit"), string("quit [shutdown]\n"));
        CHECK(wait_content(d + "/m_shutdown", "shutdown"));
      }
      backlight(4);  // as if the screensaver was on
      finish();
      CHECK_EQ(hooked, 1);
      CHECK(!file_exists(d + "/ready"));
      CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("0\n"));
      if (pass == 0) {
        CHECK(file_exists(d + "/tmp/reboot.please") && !file_exists(d + "/tmp/shutdown.please"));
        CHECK(log_has(*sv, "shutdown -r now") && !log_has(*sv, "-P"));
      } else if (pass == 1) {
        CHECK(file_exists(d + "/tmp/shutdown.please") && !file_exists(d + "/tmp/reboot.please"));
        CHECK(log_has(*sv, "shutdown -P -h now"));
      } else if (pass == 2) {
        CHECK(file_exists(d + "/tmp/restart.please"));
        CHECK(sv->cmd.log.empty());
      } else {
        CHECK(sv->cmd.log.empty());
        CHECK_EQ(slurp(d + "/m_quit"), string("quit []\n"));  // plain exit: the `quit` event, no .please file
        CHECK(!file_exists(d + "/tmp/restart.please"));
      }
    }
  }

  // ---- Wayland app_id (SDL takes it from the exe name; this test binary is not called "emulationstation")
  {
    fresh();
    ::unsetenv("SDL_VIDEO_WAYLAND_WMCLASS");
    before_video();
    const char* v = std::getenv("SDL_VIDEO_WAYLAND_WMCLASS");
    CHECK(v && string(v) == "emulationstation");
    game_started("/r/x.gb", "X");
    CHECK(std::getenv("SDL_VIDEO_WAYLAND_WMCLASS") == nullptr);  // the emulator must not inherit our app_id
    game_ended();
    before_video();  // set again for the next window
    v = std::getenv("SDL_VIDEO_WAYLAND_WMCLASS");
    CHECK(v && string(v) == "emulationstation");
    game_started("/r/x.gb", "X");
    game_ended();
    ::setenv("SDL_VIDEO_WAYLAND_WMCLASS", "custom", 1);  // an explicit choice is respected and not removed
    fresh();
    before_video();
    game_started("/r/x.gb", "X");
    v = std::getenv("SDL_VIDEO_WAYLAND_WMCLASS");
    CHECK(v && string(v) == "custom");
    game_ended();
    ::unsetenv("SDL_VIDEO_WAYLAND_WMCLASS");
  }

  // ---- suspend
  {
    auto sv = fresh();
    startup(sv, false);
    install_signals(false);
    CHECK(!suspend_supported());
    write_text(d + "/pm-suspend", "");
    CHECK(suspend_supported());
    CHECK(suspend());
    CHECK(log_has(*sv, "systemctl suspend"));
    CHECK_EQ(Paths().suspend_helper, string("/usr/lib/systemd/system-sleep/sleep"));  // the default: Zenith has no pm-suspend
  }

  // ---- inactivity / screensaver
  {
    auto sv = fresh();
    startup(sv, false);
    install_signals(false);
    string s = d + "/scripts";
    sh("mkdir -p " + s + "/screensaver-start " + s + "/screensaver-stop " + s + "/sleep " + s + "/wake");
    set_script_dirs({s});
    write_exec(s + "/screensaver-start/10.sh", "echo \"$*\" >> " + d + "/m_ssstart\n");
    write_exec(s + "/screensaver-stop/10.sh", "echo stop >> " + d + "/m_ssstop\n");
    write_exec(s + "/sleep/10.sh", "echo sleep >> " + d + "/m_sleep\n");
    write_exec(s + "/wake/10.sh", "echo wake >> " + d + "/m_wake\n");
    EsSettings es = EsSettings::from_text("<config><int name=\"ScreenSaverTime\" value=\"1000\" /><string name=\"ScreenSaverBehavior\" value=\"dim\" /></config>");
    TimePoint t0 = Clock::now();
    input(t0);
    auto dl = idle_deadline(es);
    CHECK(dl && *dl == t0 + milliseconds(1000));
    vector<Action> none;
    before_acts(es, t0 + milliseconds(999), none);
    CHECK(!idle().active());
    before_acts(es, t0 + milliseconds(1000), none);
    CHECK(idle().active());
    CHECK(screensaver::mode() == screensaver::Mode::Dim);
    CHECK(take_redraw() && !take_redraw());  // the loop must draw the overlay, once
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("0\n"));  // dim keeps the light (black switches the panel off, see test_idle)
    CHECK(wait_content(d + "/m_ssstart", "dim"));  // the behaviour is the argument, as in ES
    CHECK(wait_content(d + "/m_sleep", "sleep"));
    CHECK(!idle_deadline(es));  // active: no deadline, the process sleeps for ever
    // the first key only wakes the panel
    vector<Action> acts = {Action::A, Action::Down};
    before_acts(es, t0 + milliseconds(2000), acts);
    CHECK(acts.empty());
    CHECK(!idle().active());
    CHECK_EQ(slurp(d + "/bl/a/bl_power"), string("0\n"));
    CHECK(wait_content(d + "/m_ssstop", "stop"));
    CHECK(wait_content(d + "/m_wake", "wake"));
    dl = idle_deadline(es);
    CHECK(dl && *dl == t0 + milliseconds(3000));
    // a key that arrives while not active is for the UI
    acts = {Action::A};
    before_acts(es, t0 + milliseconds(60000), acts);
    CHECK(!idle().active() && acts.size() == 1);
    input(t0 + milliseconds(60000));
    // disabled with 0, default 5 min when the key is missing
    EsSettings off = EsSettings::from_text("<config><int name=\"ScreenSaverTime\" value=\"0\" /></config>");
    CHECK(!idle_deadline(off));
    before_acts(off, t0 + std::chrono::hours(10), none);
    CHECK(!idle().active());
    EsSettings dflt = EsSettings::from_text("<config></config>");
    dl = idle_deadline(dflt);
    CHECK(dl && *dl == t0 + milliseconds(60000 + 300000));
    // a game of hours must not end in an instant screensaver
    game_started("/r/x.gb", "X");
    game_ended();
    auto after = idle_deadline(es);
    CHECK(after && *after > Clock::now() + milliseconds(900));
    // behaviour "suspend" suspends only when the helper exists (ES parity)
    EsSettings sus = EsSettings::from_text("<config><int name=\"ScreenSaverTime\" value=\"1000\" /><string name=\"ScreenSaverBehavior\" value=\"suspend\" /></config>");
    screensaver_stop();
    sv->cmd.log.clear();
    sh("rm -f " + d + "/pm-suspend");
    screensaver_start(sus);
    CHECK(!log_has(*sv, "systemctl suspend"));
    screensaver_stop();
    write_text(d + "/pm-suspend", "");
    screensaver_start(sus);
    CHECK(log_has(*sv, "systemctl suspend"));
    screensaver_stop();
  }

  // ---- running game, busy, emukill
  {
    auto sv = fresh();
    startup(sv, false);
    install_signals(false);
    CHECK(!game_running() && running_game_json().empty() && !busy());
    CHECK(!emukill());  // no game: nothing to kill
    game_started("/storage/roms/gb/A \"q\".gb", "A \"q\"");
    CHECK(game_running());
    CHECK_EQ(running_game_json(), string("{\"name\":\"A \\\"q\\\"\",\"path\":\"/storage/roms/gb/A \\\"q\\\".gb\"}"));
    set_kill("emulationstation");
    CHECK(!emukill());  // never kill ourselves
    set_kill("a;rm -rf /");
    CHECK(!emukill());
    set_kill("");
    CHECK(!emukill());
    set_kill("retroarch");
    CHECK(emukill());
    CHECK(log_has(*sv, "killall") && log_has(*sv, "retroarch"));
    {
      Busy b1;
      Busy b2;
      CHECK(busy());
    }
    CHECK(!busy());
    game_ended();
    CHECK(!game_running() && running_game_json().empty());
    CHECK(!emukill());
  }

  // ---- POST /launch lookup and the device-loop hand-over
  {
    auto sv = fresh();
    startup(sv, false);
    install_signals(false);
    Ctx cx;
    cx.sv = sv;
    for (const char* n : {"gb", "gba"}) {
      System sy;
      sy.name = n;
      sy.fullname = n;
      sy.theme = n;
      sy.rom_dir = d + "/roms/" + n;
      sy.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR% %GAMENAME% --controllers=\"%CONTROLLERSCONFIG%\"";
      sy.emulator = "libretro";
      sy.core = "gambatte";
      sy.loaded = true;
      Game g1;
      g1.name = "Tetris";
      g1.path = sy.rom_dir + "/Tetris." + n;
      Game g2;
      g2.name = "Zelda";
      g2.path = sy.rom_dir + "/sub/Zelda." + n;
      sy.games = {g1, g2};
      sy.count = 2;
      cx.systems.push_back(sy);
    }
    Outcome o;
    CHECK(resolve_launch(cx, d + "/roms/gb/Tetris.gb", o));
    CHECK(o.kind == Outcome::Launch && o.sys == 0 && o.game == 0);
    CHECK(o.cmd.find("Tetris.gb") != string::npos && o.cmd.find("runemu.sh") == 0);
    CHECK(resolve_launch(cx, "  " + d + "/roms/gba/sub/Zelda.gba \r\n", o));
    CHECK(o.sys == 1 && o.game == 1);
    CHECK(!resolve_launch(cx, d + "/roms/gb/Nope.gb", o));
    CHECK(!resolve_launch(cx, d + "/roms/gba/Tetris.gb", o));  // right file name, wrong system
    CHECK(!resolve_launch(cx, d + "/roms/gbx/Tetris.gbx", o)); // "gb" is a prefix of "gbx" but not a directory prefix
    CHECK(!resolve_launch(cx, "roms/gb/Tetris.gb", o));        // not absolute
    CHECK(!resolve_launch(cx, "", o));

    App app(std::move(cx), std::make_unique<DummyScreen>(), make_status_source("/nonexistent"));
    CHECK(!take_external(app));  // nothing pending
    request_launch(d + "/roms/gb/Tetris.gb");
    // a launch request ends an active screensaver (ES: cancelScreenSaver)
    EsSettings es = EsSettings::from_text("<config><int name=\"ScreenSaverTime\" value=\"1000\" /></config>");
    idle().set_active(true);
    auto ex = take_external(app);
    CHECK(ex && ex->kind == Outcome::Launch && ex->sys == 0 && ex->game == 0);
    CHECK(!idle().active());
    CHECK(!take_external(app));  // consumed
    request_launch(d + "/roms/gb/Missing.gb");
    CHECK(!take_external(app));  // unknown game: dropped, no crash
    CHECK(!take_external(app));
    (void)es;
  }

  sh("rm -rf " + d);
  return zt::done();
}
