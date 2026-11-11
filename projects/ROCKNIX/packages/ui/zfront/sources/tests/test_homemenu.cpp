// In-game HOME menu (homemenu.hpp): chord timing from fake evdev events, SIGSTOP / SIGCONT ordering through the injectable signal
// function, the QUIT path, fail-safe resumes, process lookup, RetroArch's network config, `launch::run`'s supervisor, off by default.
// NOT covered here (needs the console): the panel child on screen, sway focus, the emulator itself.
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "homemenu.hpp"
#include "launch.hpp"
#include "settings.hpp"

using namespace zf;
using namespace zf::homemenu;
using std::chrono::milliseconds;

namespace {

TimePoint T0 = Clock::now();
TimePoint at(int ms) { return T0 + milliseconds(ms); }

input_event ev(int type, int code, int value) {
  input_event e;
  std::memset(&e, 0, sizeof e);
  e.type = uint16_t(type);
  e.code = uint16_t(code);
  e.value = value;
  return e;
}

void test_combo_setting() {
  CHECK(combo_from_setting("") == Combo::StartR1);  // default
  CHECK(combo_from_setting("r1") == Combo::StartR1);
  CHECK(combo_from_setting("r2") == Combo::StartR2);
  CHECK(combo_from_setting("l2") == Combo::StartL2);
  CHECK(combo_from_setting("junk") == Combo::StartR1);
  // never Select (RetroArch hotkey button) nor L1 (input_sense modifier)
  for (Combo c : {Combo::StartR1, Combo::StartR2, Combo::StartL2}) {
    std::vector<int> v = combo_codes(c);
    CHECK_EQ(v.size(), size_t(2));
    for (int code : v) CHECK(code != BTN_SELECT && code != BTN_TL);
    CHECK(v[0] == BTN_START);
  }
}

void test_detector_timing() {
  ComboDetector d(combo_codes(Combo::StartR1), 1000);
  CHECK(!d.deadline().has_value());
  d.key(BTN_START, 1, at(0));
  CHECK(!d.poll(at(5000)));  // one button is not a chord
  d.key(BTN_TR, 1, at(100));
  CHECK(d.deadline().has_value());
  CHECK(*d.deadline() == at(1100));
  CHECK(!d.poll(at(1099)));
  CHECK(d.poll(at(1100)));  // exactly 1 s after the chord was complete
  CHECK(!d.poll(at(1200)));  // once per hold
  CHECK(!d.deadline().has_value());
  d.key(BTN_TR, 0, at(1300));  // re-armed by a release
  CHECK(!d.poll(at(5000)));
  d.key(BTN_TR, 1, at(2000));
  CHECK(!d.poll(at(2999)));
  CHECK(d.poll(at(3000)));
}

void test_detector_cancel_and_noise() {
  ComboDetector d(combo_codes(Combo::StartR1), 1000);
  d.key(BTN_START, 1, at(0));
  d.key(BTN_TR, 1, at(0));
  d.key(BTN_TR, 0, at(900));  // released before 1 s: nothing
  CHECK(!d.poll(at(2000)));
  d.key(BTN_TR, 1, at(2000));
  d.key(BTN_TR, 2, at(2500));  // autorepeat does not restart the clock
  d.key(BTN_A, 1, at(2600));   // other buttons are ignored
  d.key(BTN_SELECT, 1, at(2600));
  CHECK(d.poll(at(3000)));
  // SYN_DROPPED forgets what was held: a stale "held" must not fire later
  ComboDetector e(combo_codes(Combo::StartR2), 1000);
  e.key(BTN_START, 1, at(0));
  e.key(BTN_TR2, 1, at(0));
  e.dropped();
  CHECK(!e.poll(at(5000)));
  CHECK(!e.any_held());
  // a different chord (L2) is not triggered by R1
  ComboDetector f(combo_codes(Combo::StartL2), 1000);
  f.key(BTN_START, 1, at(0));
  f.key(BTN_TR, 1, at(0));
  CHECK(!f.poll(at(5000)));
}

void test_feed_events() {
  ComboDetector d(combo_codes(Combo::StartR1), 1000);
  std::vector<input_event> v = {ev(EV_KEY, BTN_START, 1), ev(EV_SYN, SYN_REPORT, 0), ev(EV_KEY, BTN_TR, 1), ev(EV_ABS, ABS_X, 5)};
  feed_events(v.data(), v.size() * sizeof(input_event), d, at(0));
  CHECK(d.poll(at(1000)));
  std::vector<input_event> w = {ev(EV_SYN, SYN_DROPPED, 0)};
  feed_events(w.data(), w.size() * sizeof(input_event), d, at(1100));
  CHECK(!d.any_held());
  // a partial record at the end is ignored
  feed_events(v.data(), sizeof(input_event) / 2, d, at(1200));
  CHECK(!d.any_held());
}

void test_watcher_pipe() {
  int p[2];
  CHECK(::pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0);
  PadWatcher w(ComboDetector(combo_codes(Combo::StartR1), 300));
  w.add_fd(p[0]);
  CHECK_EQ(w.count(), size_t(1));
  CHECK(!w.wait(30));  // nothing yet
  input_event down[2] = {ev(EV_KEY, BTN_START, 1), ev(EV_KEY, BTN_TR, 1)};
  CHECK(::write(p[1], down, sizeof down) == ssize_t(sizeof down));
  TimePoint t = Clock::now();
  bool fired = false;
  while (!fired && Clock::now() - t < milliseconds(2000)) fired = w.wait(100);
  double ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
  CHECK(fired);
  CHECK(ms >= 280 && ms < 1200);  // the hold time, with no further events
  // release: wait_released returns once both are up
  input_event up[2] = {ev(EV_KEY, BTN_START, 0), ev(EV_KEY, BTN_TR, 0)};
  CHECK(::write(p[1], up, sizeof up) == ssize_t(sizeof up));
  CHECK(w.wait_released(500));
  ::close(p[1]);
}

void test_watcher_wait_released_times_out() {
  int p[2];
  CHECK(::pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0);
  PadWatcher w(ComboDetector(combo_codes(Combo::StartR1), 300));
  w.add_fd(p[0]);
  input_event down[1] = {ev(EV_KEY, BTN_START, 1)};
  CHECK(::write(p[1], down, sizeof down) == ssize_t(sizeof down));
  w.wait(20);
  TimePoint t = Clock::now();
  CHECK(!w.wait_released(150));  // bounded: never blocks the resume for ever
  CHECK(Clock::now() - t < milliseconds(1000));
  ::close(p[1]);
}

// ---- the sequence ------------------------------------------------------------------------------------------------------------------

struct Rec {
  std::vector<std::string> log;
  Io io;
  std::vector<int> pids = {100, 101};
  PanelResult panel = PanelResult::Resume;
  int fail_stop_on = -1;  // pid whose SIGSTOP fails
  bool term = false;
  std::vector<std::vector<int>> recorded;
  Rec() {
    io.kill = [this](int pid, int sig) {
      log.push_back((sig == SIGSTOP ? "STOP " : sig == SIGCONT ? "CONT " : "SIG? ") + std::to_string(pid));
      if (sig == SIGSTOP && pid == fail_stop_on) return -1;
      return 0;
    };
    io.find_pids = [this] { return pids; };
    io.record = [this](const std::vector<int>& p) { recorded.push_back(p); };
    io.panel = [this](const std::vector<int>&) {
      log.push_back("panel");
      return panel;
    };
    io.term_requested = [this] { return term; };
    io.quit_game = [this] { log.push_back("emukill"); };
    io.send_state = [this](const char* c) { log.push_back(std::string("udp ") + c); };
    io.refocus = [this](const std::vector<int>&) { log.push_back("refocus"); };
    io.wait_buttons_up = [this] { log.push_back("buttons-up"); };
  }
};

void test_resume_order() {
  Rec r;
  CHECK(trigger(r.io) == Outcome::Resumed);
  std::vector<std::string> want = {"STOP 100", "STOP 101", "panel", "buttons-up", "CONT 101", "CONT 100", "refocus"};
  CHECK(r.log == want);
  // the stopped pids were written before the first SIGSTOP and cleared at the end
  CHECK_EQ(r.recorded.size(), size_t(2));
  CHECK(r.recorded[0] == r.pids);
  CHECK(r.recorded[1].empty());
}

void test_nothing_to_pause() {
  Rec r;
  r.pids.clear();
  CHECK(trigger(r.io) == Outcome::Nothing);
  CHECK(r.log.empty());  // no signal, no panel
}

void test_quit_path() {
  Rec r;
  r.panel = PanelResult::Quit;
  CHECK(trigger(r.io) == Outcome::Quit);
  // SIGCONT first (a stopped process cannot act on SIGTERM), then the existing emukill
  std::vector<std::string> want = {"STOP 100", "STOP 101", "panel", "buttons-up", "CONT 101", "CONT 100", "refocus", "emukill"};
  CHECK(r.log == want);
}

void test_state_commands_after_resume() {
  Rec r;
  r.panel = PanelResult::SaveState;
  CHECK(trigger(r.io) == Outcome::StateSaved);
  CHECK(r.log.back() == "udp SAVE_STATE");
  CHECK(r.log[r.log.size() - 3] == "CONT 100");  // sent after the resume: a stopped RetroArch cannot read its socket
  Rec l;
  l.panel = PanelResult::LoadState;
  CHECK(trigger(l.io) == Outcome::StateLoaded);
  CHECK(l.log.back() == "udp LOAD_STATE");
}

void test_failsafe_stop_failure() {
  Rec r;
  r.fail_stop_on = 101;  // the second SIGSTOP fails
  CHECK(trigger(r.io) == Outcome::PauseFailed);
  std::vector<std::string> want = {"STOP 100", "STOP 101", "CONT 100"};  // only what was stopped is continued, and no panel
  CHECK(r.log == want);
  CHECK(r.recorded.back().empty());
}

void test_failsafe_panel_failed() {
  Rec r;
  r.panel = PanelResult::Failed;  // the child crashed / could not start
  CHECK(trigger(r.io) == Outcome::Resumed);
  CHECK(r.log[r.log.size() - 2] == "CONT 100");  // game running again, nothing else happened
  for (const std::string& s : r.log) CHECK(s != "emukill");
}

void test_failsafe_term_while_paused() {
  Rec r;
  r.panel = PanelResult::Quit;
  r.term = true;  // SIGTERM arrived: it is the game's problem now; resume it and do not run emukill
  CHECK(trigger(r.io) == Outcome::Resumed);
  for (const std::string& s : r.log) CHECK(s != "emukill");
  CHECK(r.log.back() == "refocus");
}

void test_failsafe_no_panel_fn() {
  Rec r;
  r.io.panel = nullptr;  // nothing can show it: resume at once
  CHECK(trigger(r.io) == Outcome::Resumed);
  CHECK(r.log.front() == "STOP 100");
  CHECK(r.log[2] == "buttons-up");
}

void test_panel_exit_codes() {
  auto st = [](int code) { return code << 8; };  // WIFEXITED layout
  CHECK(panel_result_from_exit(st(kExitResume)) == PanelResult::Resume);
  CHECK(panel_result_from_exit(st(kExitQuit)) == PanelResult::Quit);
  CHECK(panel_result_from_exit(st(kExitSave)) == PanelResult::SaveState);
  CHECK(panel_result_from_exit(st(kExitLoad)) == PanelResult::LoadState);
  CHECK(panel_result_from_exit(st(1)) == PanelResult::Failed);   // any error is a plain resume
  CHECK(panel_result_from_exit(st(139)) == PanelResult::Failed);
  CHECK(panel_result_from_exit(SIGSEGV) == PanelResult::Failed);  // killed by a signal
  CHECK(panel_result_from_exit(-1) == PanelResult::Failed);
  for (PanelResult r : {PanelResult::Resume, PanelResult::Quit, PanelResult::SaveState, PanelResult::LoadState})
    CHECK(panel_result_from_exit(st(exit_code_of(r))) == r);
}

void test_panel_child_without_args() {
  set_panel_args("", {});
  CHECK(!panel_args_set());
  CHECK(run_panel_child(false, [] { return false; }) == PanelResult::Failed);
}

void test_panel_child_exit_code() {
  // `sh -c "exit 21"` stands for the panel: the arguments are fixed ones, then --home-panel (ignored by sh -c after the script)
  set_panel_args("/bin/sh", {"-c", "exit 21", "zfront"});
  CHECK(panel_args_set());
  CHECK(run_panel_child(false, [] { return false; }) == PanelResult::SaveState);
  set_panel_args("/bin/sh", {"-c", "exit 0", "zfront"});
  CHECK(run_panel_child(true, [] { return false; }) == PanelResult::Resume);
  set_panel_args("/bin/sh", {"-c", "kill -SEGV $$", "zfront"});
  CHECK(run_panel_child(false, [] { return false; }) == PanelResult::Failed);
  set_panel_args("/nonexistent/zfront", {});
  CHECK(run_panel_child(false, [] { return false; }) == PanelResult::Failed);
  // a SIGTERM to the frontend while the panel runs: the panel is killed and the game resumes
  set_panel_args("/bin/sh", {"-c", "sleep 30", "zfront"});
  TimePoint t = Clock::now();
  CHECK(run_panel_child(false, [] { return true; }) == PanelResult::Resume);
  CHECK(Clock::now() - t < milliseconds(3000));
  set_panel_args("", {});
}

// ---- helpers ---------------------------------------------------------------------------------------------------------------------

void test_find_pids() {
  char tmpl[] = "/tmp/zf_home_procXXXXXX";
  CHECK(::mkdtemp(tmpl) != nullptr);
  std::string root = tmpl;
  auto mk = [&](const char* pid, const char* comm) {
    ::mkdir((root + "/" + pid).c_str(), 0755);
    std::ofstream(root + "/" + pid + "/comm") << comm << "\n";
  };
  mk("200", "retroarch");
  mk("201", "retroarch32");
  mk("202", "PPSSPPSDL");
  mk("203", "runemu.sh");
  mk("204", "mupen64plus-gui");  // the kernel cuts comm at 15 characters: the long name below must still match it
  mk("self", "retroarch");                  // not a pid
  std::vector<int> got = find_pids_by_name({"retroarch", "retroarch32"}, 999, root);
  CHECK(got == (std::vector<int>{200, 201}));
  got = find_pids_by_name({"retroarch"}, 200, root);  // never ourselves
  CHECK(got.empty());
  got = find_pids_by_name({"mupen64plus-gui-extra-long"}, 999, root);
  CHECK(got == (std::vector<int>{204}));
  CHECK(find_pids_by_name({"nothing"}, 999, root).empty());
  CHECK(find_pids_by_name({"retroarch"}, 999, "/nonexistent").empty());
  std::string cmd = "rm -rf " + root;
  CHECK(std::system(cmd.c_str()) == 0);
}

void test_retroarch_net() {
  NetCmd n = parse_retroarch_net("foo = \"1\"\nnetwork_cmd_enable = \"false\"\nnetwork_cmd_port = \"55355\"\n");
  CHECK(!n.enabled);
  CHECK_EQ(n.port, 55355);  // the image default: SAVE/LOAD STATE rows hidden
  n = parse_retroarch_net("network_cmd_enable = \"true\"\nnetwork_cmd_port = \"4242\"\n");
  CHECK(n.enabled);
  CHECK_EQ(n.port, 4242);
  n = parse_retroarch_net("");
  CHECK(!n.enabled);
  n = parse_retroarch_net("network_cmd_enable = \"true\"\nnetwork_cmd_port = \"junk\"\n");
  CHECK(n.enabled && n.port == 55355);
  CHECK(is_retroarch({"retroarch", "retroarch32"}));
  CHECK(!is_retroarch({"PPSSPPSDL"}));
  CHECK(!is_retroarch({}));
}

void test_stopped_file() {
  char tmpl[] = "/tmp/zf_home_fileXXXXXX";
  CHECK(::mkdtemp(tmpl) != nullptr);
  std::string path = std::string(tmpl) + "/sub/home-stopped";
  write_stopped_file(path, {300, 301});
  std::string t;
  CHECK(read_file(path, t));
  CHECK(t == "300\n301\n");
  std::vector<std::pair<int, int>> sent;
  size_t n = recover_stopped(path, [&](int pid, int sig) {
    sent.push_back({pid, sig});
    return pid == 301 ? -1 : 0;  // 301 is gone already
  });
  CHECK_EQ(n, size_t(1));
  CHECK_EQ(sent.size(), size_t(2));
  CHECK(sent[0].second == SIGCONT);
  CHECK(!file_exists(path));  // consumed
  CHECK_EQ(recover_stopped(path, [](int, int) { return 0; }), size_t(0));  // free when there is no file
  write_stopped_file(path, {400});
  write_stopped_file(path, {});  // cleared on a normal resume
  CHECK(!file_exists(path));
  std::string cmd = std::string("rm -rf ") + tmpl;
  CHECK(std::system(cmd.c_str()) == 0);
}

// a real process stopped and continued through the real kill(2): nothing is left stopped
void test_real_signals() {
  pid_t c = ::fork();
  if (c == 0) {
    ::execl("/bin/sleep", "sleep", "5", static_cast<char*>(nullptr));
    ::_exit(127);
  }
  CHECK(c > 0);
  Io io;
  io.kill = [](int pid, int sig) { return ::kill(pid_t(pid), sig); };
  io.find_pids = [c] { return std::vector<int>{int(c)}; };
  bool stopped_in_panel = false;
  io.panel = [&](const std::vector<int>& p) {
    for (int i = 0; i < 100 && !stopped_in_panel; ++i) {
      int st = 0;
      stopped_in_panel = ::waitpid(p[0], &st, WUNTRACED | WNOHANG) == p[0] && WIFSTOPPED(st);
      if (!stopped_in_panel) ::usleep(5000);
    }
    return PanelResult::Failed;
  };
  CHECK(trigger(io) == Outcome::Resumed);
  CHECK(stopped_in_panel);
  int st = 0;
  CHECK(::waitpid(c, &st, WCONTINUED | WNOHANG) == c && WIFCONTINUED(st));  // running again
  ::kill(c, SIGKILL);
  ::waitpid(c, &st, 0);
}

// ---- launch::run's supervisor ------------------------------------------------------------------------------------------------------

void test_run_without_supervisor() {
  launch::RunResult r = launch::run("true");
  CHECK_EQ(r.exit_code, 0);
  r = launch::run("sh -c 'exit 3'");
  CHECK_EQ(r.exit_code, 3);
}

void test_run_with_supervisor() {
  std::atomic<int> calls{0};
  int seen_pid = 0;
  TimePoint t = Clock::now();
  launch::RunResult r = launch::run("sleep 0.4", [&](int pid) {
    ++calls;
    seen_pid = pid;
    // a supervisor that returns at once must not make `run` spin
  });
  double ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
  CHECK_EQ(r.exit_code, 0);
  CHECK(calls > 0 && calls < 100);  // floor of ~20 ms per round
  CHECK(seen_pid > 0);
  CHECK(ms >= 350 && ms < 2500);
  r = launch::run("sh -c 'exit 7'", [](int) {});
  CHECK_EQ(r.exit_code, 7);  // the exit code survives the polling path
}

// ---- off by default ----------------------------------------------------------------------------------------------------------------

void test_off_by_default() {
  SystemConf c;
  CHECK(!enabled(c));  // no key: off
  c.set(kKeyEnable, "0");
  CHECK(!enabled(c));
  c.set(kKeyEnable, "1");
  CHECK(enabled(c));
  CHECK(combo(c) == Combo::StartR1);
  c.set(kKeyCombo, "r2");
  CHECK(combo(c) == Combo::StartR2);
  // the Env of a launch has no supervisor unless sdlr sets one (it does so only when `enabled` and the panel command is known)
  launch::Env e;
  CHECK(!e.supervise);
  CHECK(!panel_args_set());
}

}  // namespace

int main() {
  test_off_by_default();
  test_combo_setting();
  test_detector_timing();
  test_detector_cancel_and_noise();
  test_feed_events();
  test_watcher_pipe();
  test_watcher_wait_released_times_out();
  test_resume_order();
  test_nothing_to_pause();
  test_quit_path();
  test_state_commands_after_resume();
  test_failsafe_stop_failure();
  test_failsafe_panel_failed();
  test_failsafe_term_while_paused();
  test_failsafe_no_panel_fn();
  test_panel_exit_codes();
  test_panel_child_without_args();
  test_panel_child_exit_code();
  test_find_pids();
  test_retroarch_net();
  test_stopped_file();
  test_real_signals();
  test_run_without_supervisor();
  test_run_with_supervisor();
  return zt::done();
}
