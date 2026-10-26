#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <string>

#include "check.hpp"
#include "launch.hpp"

using namespace zf;
using std::string;
using std::vector;

namespace {

string tmpdir(const char* tag) {
  string d = string("/tmp/zf-launch-") + tag + "-" + std::to_string(::getpid());
  string c = "rm -rf " + d + " && mkdir -p " + d;
  if (std::system(c.c_str()) != 0) std::abort();
  return d;
}

void write_exec(const string& path, const string& body) {
  std::ofstream(path) << "#!/bin/sh\n" << body;
  ::chmod(path.c_str(), 0755);
}

string slurp(const string& p) {
  string s;
  read_file(p, s);
  return s;
}

bool wait_file(const string& p, int ms) {
  for (int i = 0; i < ms; ++i) {
    if (file_exists(p)) return true;
    ::usleep(1000);
  }
  return file_exists(p);
}

double us_per(int n, const std::function<void()>& f) {
  auto t0 = Clock::now();
  for (int i = 0; i < n; ++i) f();
  return std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / n;
}

vector<string> sp(const string& s) {
  vector<string> v;
  if (!launch::split_simple(s, v)) v.assign(1, "<none>");
  return v;
}

}  // namespace

int main() {
  // ---- escape / command (ported from the Rust tests of zf/launch)
  CHECK(launch::escape_path("/r/a b (1)/x.gb") == "/r/a\\ b\\ \\(1\\)/x.gb");
  CHECK(launch::escape_path("/r/it's [!].gb") == "/r/it\\'s\\ \\[\\!\\].gb");
  CHECK(launch::escape_path("/r//x/") == "/r/x");
  CHECK(launch::escape_path("/r/a&b$c^d.gb") == "/r/a\\&b\\$c\\^d.gb");
  CHECK(launch::escape_path("/r/a`b#c~d|e.gb") == "/r/a`b#c~d|e.gb");  // not escaped by ES

  System sys;
  sys.name = "gbc";
  sys.fullname = "Game Boy Color";
  sys.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR% %GAMENAME% --controllers=\"%CONTROLLERSCONFIG%\"";
  sys.emulator = "retroarch";
  sys.core = "gambatte";
  sys.emus = {{"retroarch", {"gambatte", "mgba"}}};
  Game g;
  g.name = "Mario Golf";
  g.path = "/roms/gbc/Mario Golf.gbc";
  ConfMap conf;
  conf["gbc.core"] = "mgba";
  vector<JoyInfo> joys = {{0, "03000000aa", "pad"}, {1, "03000000bb", "pad2"}};
  string c = launch::build_command(sys, g, conf, {});
  CHECK(c.find("runemu.sh /roms/gbc/Mario\\ Golf.gbc -Pgbc --core=mgba --emulator=retroarch \"Mario Golf\"") == 0);
  // controllers: one block per player, none -> empty, at most 8
  CHECK_EQ(launch::controllers_config({}), string());
  CHECK_EQ(launch::controllers_config(joys), string(" -p1index 0 -p1guid 03000000aa  -p2index 1 -p2guid 03000000bb "));
  vector<JoyInfo> many(10, JoyInfo{3, "g", "n"});
  string cc = launch::controllers_config(many);
  CHECK(cc.find("-p8index") != string::npos && cc.find("-p9index") == string::npos);
  string cj = launch::build_command(sys, g, conf, joys);
  CHECK(cj.find("--controllers=\" -p1index 0 -p1guid 03000000aa  -p2index 1 -p2guid 03000000bb \"") != string::npos);

  // ---- exit rules
  launch::AfterLaunch ok = launch::after_launch(0, 100);
  CHECK(ok.count_play && ok.reload_lists && ok.add_gametime == 100);
  CHECK_EQ(launch::after_launch(0, 9).add_gametime, 0ul);
  CHECK_EQ(launch::after_launch(0, 10).add_gametime, 10ul);
  launch::AfterLaunch fail = launch::after_launch(1, 100);
  CHECK(!fail.count_play && fail.add_gametime == 0 && !fail.reload_lists);
  launch::AfterLaunch err = launch::after_launch(210, 5);
  CHECK(err.error.kind == launch::ExitError::Code);
  CHECK(launch::after_launch(250, 5).error.kind == launch::ExitError::LogFile);
  CHECK(launch::after_launch(299, 5).error.kind == launch::ExitError::LogFile);
  CHECK(launch::after_launch(301, 5).error.kind == launch::ExitError::None);
  CHECK_EQ(launch::error_message(err.error, nullptr), string("UKNOWN ERROR : 210"));
  string logtxt = "boom";
  CHECK_EQ(launch::error_message({launch::ExitError::LogFile, 250}, &logtxt), logtxt);
  CHECK_EQ(launch::error_message({launch::ExitError::Code, 204}, nullptr), string("EMULATOR IS MISSING"));

  // ---- split_simple
  CHECK(sp("a b  c") == (vector<string>{"a", "b", "c"}));
  CHECK(sp("x /r/a\\ b\\(1\\).gb -Pgb") == (vector<string>{"x", "/r/a b(1).gb", "-Pgb"}));
  CHECK(sp("r --controllers=\"\"") == (vector<string>{"r", "--controllers="}));
  CHECK(sp("r --c=\" -p1index 0 \" z") == (vector<string>{"r", "--c= -p1index 0 ", "z"}));
  CHECK(sp("r \"a\\\"b\"") == (vector<string>{"r", "a\"b"}));
  CHECK(sp("r \"a;b\"") == (vector<string>{"r", "a;b"}));  // inside quotes `;` is plain
  for (const char* bad : {"a && b", "a $HOME", "a \"$HOME\"", "a 'b'", "a \"b", "", "FOO=1 a", "a \\", "a | b", "a `x`", "a ~", "a #c", "a \"\\n\""})
    CHECK(sp(bad) == vector<string>{"<none>"});
  CHECK(sp("a\\\nb") == vector<string>{"<none>"});
  CHECK(sp(c).size() > 3);  // the command ES builds is always simple
  CHECK(sp(cj).size() > 3);
  { vector<string> tmp; CHECK(launch::split_simple(launch::build_command(sys, g, conf, joys), tmp)); }

  // ---- run
  CHECK_EQ(launch::run("/bin/true").exit_code, 0);
  CHECK_EQ(launch::run("/bin/false").exit_code, 1);
  CHECK_EQ(launch::run("/bin/sh -c exit\\ 5").exit_code, 5);  // direct: sh gets "exit 5"
  CHECK_EQ(launch::run("exit 3 ; true").exit_code, 3);        // not simple: through sh
  CHECK_EQ(launch::run("kill -9 $$").exit_code, 1);           // killed by a signal
  CHECK_EQ(launch::run("/nonexistent/zf-x").exit_code, 127);  // direct spawn fails -> sh reports 127
  CHECK_EQ(launch::run("true").exit_code, 0);                 // PATH lookup (posix_spawnp)
  {
    string d = tmpdir("run");
    write_exec(d + "/noshebang.sh", "");  // placeholder, rewritten without shebang below
    std::ofstream(d + "/noshebang.sh") << "exit 4\n";
    ::chmod((d + "/noshebang.sh").c_str(), 0755);
    CHECK_EQ(launch::run(d + "/noshebang.sh").exit_code, 4);  // ENOEXEC -> sh fallback
    auto r = launch::run("/bin/sh -c sleep\\ 1");
    CHECK(r.secs >= 1 && r.secs <= 2);
    std::system(("rm -rf " + d).c_str());
  }

  // ---- hooks
  {
    string d = tmpdir("hooks");
    string out = d + "/out.txt";
    std::system(("mkdir -p " + d + "/game-start " + d + "/game-end " + d + "/screensaver-start").c_str());
    vector<string> dirs = {d, d};
    CHECK(!launch::has_hooks(dirs, "game-start"));
    CHECK(launch::game_start(dirs, "/r/My Game.gb", "My Game").empty());
    write_exec(d + "/screensaver-start/10-x.sh", "true\n");
    CHECK(!launch::has_hooks(dirs, "game-start"));  // another event's script does not count
    write_exec(d + "/game-start/10-a.sh", "echo \"S $@\" >> " + out + "\n");
    write_exec(d + "/game-end/10-b.sh", "echo \"E $#\" >> " + out + "\n");
    write_exec(d + "/single.sh", "echo \"U $1\" >> " + out + "\n");
    std::ofstream(d + "/notexec.sh") << "#!/bin/sh\necho NOEXEC >> " << out << "\n";
    CHECK(launch::has_hooks(dirs, "game-start"));
    vector<string> ran = launch::fire_event(dirs, "game-start", {launch::escape_path("/r/My Game.gb"), "My Game", "My Game"}, true);
    CHECK_EQ(ran.size(), 2ul);  // dedup, one in the event dir + one single, the non-executable file is skipped
    if (ran.size() == 2) CHECK(ran[0].find("10-a.sh /r/My\\ Game.gb \"My Game\" \"My Game\"") != string::npos || ran[0].find("10-a.sh \"/r/My\\ Game.gb\"") != string::npos);
    CHECK_EQ(launch::fire_event(dirs, "game-end", {}, true).size(), 2ul);
    string txt = slurp(out);
    CHECK(txt.find("S ") != string::npos && txt.find("E 0") != string::npos && txt.find("U game-start") != string::npos && txt.find("U game-end") != string::npos);
    CHECK(txt.find("NOEXEC") == string::npos);
    // async: returns without waiting
    write_exec(d + "/game-start/20-slow.sh", "sleep 1\necho SLOW >> " + out + "\n");
    auto t0 = Clock::now();
    launch::game_start(dirs, "/r/x.gb", "x");
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    CHECK(ms < 200);
    CHECK(slurp(out).find("SLOW") == string::npos);
    CHECK(wait_file(d + "/out.txt", 100));
    ::usleep(1300000);
    CHECK(slurp(out).find("SLOW") != string::npos);
    launch::reap_hooks();
    CHECK(launch::game_start({"/nonexistent-zf"}, "/r/x", "x").empty());
    std::system(("rm -rf " + d).c_str());
  }

  // ---- LaunchPrep with a fake runemu.sh
  {
    string d = tmpdir("prep");
    string log = d + "/log.txt";
    ::setenv("ZF_FAKE_LOG", log.c_str(), 1);
    write_exec(d + "/runemu.sh",
               "echo \"$@\" >> $ZF_FAKE_LOG\n"
               "if [ \"$1\" = --prepare ]; then\n"
               "  touch " + d + "/started\n"
               "  sleep ${ZF_PREP_SLEEP:-0}\n"
               "  touch " + d + "/finished\n"
               "  exit ${ZF_PREP_EXIT:-0}\n"
               "fi\n");
    string cmd = d + "/runemu.sh /r/a.gb -Pgb --core=gambatte --controllers=\" -p1index 0 -p1guid g \"";
    string rest = cmd.substr(cmd.find(' '));
    TimePoint t0 = Clock::now();
    auto ms = [](int n) { return std::chrono::milliseconds(n); };
    // prepare_line: exactly the launch arguments, only `--prepare` inserted after the program
    CHECK_EQ(launch::prepare_line(cmd), d + "/runemu.sh --prepare" + rest);
    CHECK_EQ(launch::prepare_line("runemu.sh"), string("runemu.sh --prepare"));
    CHECK_EQ(launch::prepare_line("/usr/bin/true x"), string());
    CHECK_EQ(launch::prepare_line("/usr/bin/xrunemu.sh x"), string());
    CHECK_EQ(launch::prepare_line(""), string());
    {  // injectable runner: prepare() builds the line and hands it over
      vector<string> lines;
      launch::Spawner fake = [&](const string& l) { lines.push_back(l); return 4242; };
      CHECK_EQ(launch::prepare(cmd, fake), 4242);
      CHECK(lines.size() == 1 && lines[0] == d + "/runemu.sh --prepare" + rest);
      CHECK_EQ(launch::prepare("/usr/bin/true x", fake), 0);
      CHECK_EQ(lines.size(), 1ul);
      launch::Spawner bad = [](const string&) { return -2; };
      CHECK_EQ(launch::prepare(cmd, bad), -1);
    }
    {  // dwell, one at a time, never killed, no repeat for the same command
      vector<string> lines;
      launch::LaunchPrep::Config cf;
      cf.dwell_ms = 1000;
      cf.retry_ms = 250;
      cf.spawn = [&](const string& l) {  // a live child (1.5 s) stands for a slow prepare
        lines.push_back(l);
        pid_t c = ::fork();
        if (c == 0) {
          ::usleep(1500000);
          ::_exit(0);
        }
        return int(c);
      };
      launch::LaunchPrep p(cf);
      CHECK(!p.next_deadline());
      p.set_target(cmd, t0);
      CHECK(p.next_deadline() && *p.next_deadline() == t0 + ms(1000));
      p.set_target(cmd, t0 + ms(500));  // same command: timer not restarted
      CHECK(*p.next_deadline() == t0 + ms(1000));
      p.poll(t0 + ms(999));
      CHECK(lines.empty());
      p.poll(t0 + ms(1000));
      CHECK(lines.size() == 1 && lines[0] == launch::prepare_line(cmd));
      CHECK(!p.next_deadline());
      p.set_target(cmd, t0 + ms(1100));  // same command already prepared
      CHECK(!p.next_deadline());
      string cmd2 = d + "/runemu.sh /r/b.gb -Pgb";
      p.set_target(cmd2, t0 + ms(1200));  // the previous one is still "running" (our own pid is not waitable)
      CHECK(p.running());
      p.poll(t0 + ms(2200));
      CHECK_EQ(lines.size(), 1ul);  // waits for the previous: nothing killed, nothing started
      CHECK(p.next_deadline() && *p.next_deadline() == t0 + ms(2450));
      p.set_target("/usr/bin/true x", t0 + ms(2300));  // not preparable
      CHECK(!p.next_deadline());
      p.set_target("", t0);
      CHECK(!p.next_deadline());
      p.launched();
      p.set_target(cmd, t0 + ms(5000));  // after a game the same command is prepared again
      CHECK(p.next_deadline());
    }
    {  // a settings write (ratio, shader, game options) invalidates the preparation and repeats it
      vector<string> lines;
      unsigned gen = 7;
      launch::LaunchPrep::Config cf;
      cf.dwell_ms = 100;
      cf.retry_ms = 50;
      cf.generation = [&] { return gen; };
      bool slow = false;
      cf.spawn = [&](const string& l) {
        lines.push_back(l);
        pid_t c = ::fork();
        if (c == 0) {
          ::usleep(slow ? 400000 : 0);
          ::_exit(0);
        }
        return int(c);
      };
      launch::LaunchPrep p(cf);
      p.set_target(cmd, t0);
      p.poll(t0 + ms(100));
      CHECK_EQ(lines.size(), 1ul);
      ::usleep(50000);
      p.poll(t0 + ms(200));  // finished and collected
      CHECK(!p.running() && !p.next_deadline());
      p.set_target(cmd, t0 + ms(300));  // same command, same settings: nothing to do
      CHECK(!p.next_deadline());
      gen = 8;  // a menu wrote system.cfg
      p.set_target("", t0 + ms(400));  // (the menu on top has no target)
      p.set_target(cmd, t0 + ms(500));
      CHECK(p.next_deadline() && *p.next_deadline() == t0 + ms(600));
      p.poll(t0 + ms(600));
      CHECK_EQ(lines.size(), 2ul);
      // written while the preparation runs: it started before the write, so it is done again afterwards
      slow = true;
      ::usleep(50000);
      p.poll(t0 + ms(700));
      lines.clear();
      gen = 9;
      p.set_target("", t0 + ms(800));
      p.set_target(cmd + " x", t0 + ms(800));
      p.poll(t0 + ms(1000));  // starts the slow one for "x"
      CHECK_EQ(lines.size(), 1ul);
      gen = 10;  // written while it runs
      p.poll(t0 + ms(1100));  // re-armed, previous still running: waits
      CHECK_EQ(lines.size(), 1ul);
      CHECK(p.next_deadline());
      ::usleep(500000);
      p.poll(t0 + ms(1300));
      CHECK_EQ(lines.size(), 2ul);
      CHECK_EQ(lines[1], launch::prepare_line(cmd + " x"));
      ::usleep(500000);
      p.poll(t0 + ms(1400));
    }
    {  // real spawner with the fake runemu.sh: the child gets --prepare + the same arguments, exit 0
      launch::LaunchPrep::Config cf;
      cf.dwell_ms = 10;
      launch::LaunchPrep p(cf);
      p.set_target(cmd, t0);
      p.poll(t0 + ms(20));
      CHECK_EQ(p.spawned(), 1ul);
      for (int i = 0; i < 2000 && p.running(); ++i) {
        ::usleep(1000);
        p.poll(t0 + ms(20));
      }
      CHECK(!p.running());
      CHECK(slurp(log).find("--prepare /r/a.gb -Pgb --core=gambatte --controllers= -p1index 0 -p1guid g ") != string::npos);
    }
    {  // a slow preparation is not killed when the cursor moves or the game launches
      ::setenv("ZF_PREP_SLEEP", "0.5", 1);
      std::remove((d + "/finished").c_str());
      launch::LaunchPrep::Config cf;
      cf.dwell_ms = 0;
      launch::LaunchPrep p(cf);
      p.set_target(cmd, t0);
      p.poll(t0);
      CHECK(p.running());
      p.set_target(cmd + " other", t0);
      p.launched();
      ::usleep(1000000);
      CHECK(file_exists(d + "/finished"));
      ::unsetenv("ZF_PREP_SLEEP");
    }
    {  // missing runemu.sh = not prepared, nothing else happens
      launch::LaunchPrep::Config cf;
      cf.dwell_ms = 0;
      launch::LaunchPrep p(cf);
      p.set_target("/nonexistent-zf/runemu.sh /r/a.gb -Pgb", t0);
      p.poll(t0);
      CHECK(!p.running() && p.spawned() == 0 && !p.next_deadline());
    }

    // ---- execute: order of steps, hooks, outcome
    {
      string out = d + "/order.txt";
      std::system(("mkdir -p " + d + "/scripts").c_str());
      write_exec(d + "/scripts/single.sh", "echo \"$1 $2\" >> " + out + "\n");
      launch::Env env;
      env.script_dirs = {d + "/scripts"};
      string order;
      env.release = [&] { order += "release;"; };
      env.restore = [&] { order += "restore;"; };
      launch::Request rq;
      rq.cmd = "/bin/sh -c exit\\ 0";
      rq.rom_raw = "/r/My Game.gb";
      rq.name = "My Game";
      launch::Outcome o = launch::execute(rq, env);
      CHECK_EQ(order, string("release;restore;"));
      CHECK(o.run.exit_code == 0 && o.after.count_play && o.message.empty());
      string t;
      for (int i = 0; i < 100; ++i) {  // the hooks run detached: wait for both lines (slow when the machine is loaded)
        ::usleep(50000);
        launch::reap_hooks();
        t = slurp(out);
        if (t.find("game-start ") != string::npos && t.find("game-end") != string::npos) break;
      }
      CHECK(t.find("game-start ") != string::npos && t.find("game-end") != string::npos);
      rq.cmd = "/bin/sh -c exit\\ 210";
      o = launch::execute(rq, env);
      CHECK(o.run.exit_code == 210 && !o.after.count_play && o.message == "UKNOWN ERROR : 210");
      std::ofstream("/tmp/launch_error.log") << "no core";
      rq.cmd = "/bin/sh -c exit\\ 250";
      o = launch::execute(rq, env);
      CHECK_EQ(o.message, string("no core"));
      CHECK(!file_exists("/tmp/launch_error.log"));
      // with a preparation object: the launch command is untouched and the target is forgotten
      launch::LaunchPrep::Config cf;
      cf.dwell_ms = 0;
      launch::LaunchPrep p(cf);
      launch::Env e2;
      e2.prep = &p;
      e2.script_dirs = {"/nonexistent-zf"};
      p.set_target(cmd, t0);
      rq.cmd = cmd;
      std::remove(log.c_str());
      launch::execute(rq, e2);
      CHECK(slurp(log).find("--prepare") == string::npos);   // the launch is the plain command
      CHECK(slurp(log).find("/r/a.gb -Pgb") != string::npos);
      CHECK(!p.next_deadline());
    }

    // ---- timings (microseconds of our own cost, PC)
    {
      std::printf("-- launch timings (x86 PC, us per call)\n");
      Game gg = g;
      std::printf("build_command            %8.2f\n", us_per(20000, [&] { string s = launch::build_command(sys, gg, conf, joys); (void)s; }));
      vector<string> av;
      std::printf("split_simple             %8.2f\n", us_per(100000, [&] { launch::split_simple(cj, av); }));
      vector<string> none_dirs = {d + "/nodir", "/nonexistent-zf"};
      std::printf("game_start, no scripts   %8.2f\n", us_per(20000, [&] { launch::game_start(none_dirs, "/r/x.gb", "x"); }));
      vector<string> empty_dirs = {d + "/emptyhooks"};
      std::system(("mkdir -p " + d + "/emptyhooks/game-start " + d + "/emptyhooks/game-end").c_str());
      std::printf("game_start, empty dirs   %8.2f\n", us_per(20000, [&] { launch::game_start(empty_dirs, "/r/x.gb", "x"); }));
      launch::LaunchPrep p;
      std::printf("prep.set_target same     %8.2f\n", us_per(200000, [&] { p.set_target(cmd, t0); }));
      int flip = 0;
      string c1 = cmd, c2 = cmd + "z";
      std::printf("prep.set_target change   %8.2f\n", us_per(50000, [&] { p.set_target(flip++ & 1 ? c1 : c2, t0); }));
      std::printf("after_launch             %8.2f\n", us_per(1000000, [&] { launch::after_launch(0, 100); }));
      std::printf("run /bin/true direct     %8.2f\n", us_per(300, [&] { launch::run("/bin/true"); }));
      std::printf("run via sh -c            %8.2f\n", us_per(300, [&] { launch::run("/bin/true ; true"); }));
      // spawn cost seen by the UI thread: poll() that starts a preparation (child ends at once)
      double tot = 0;
      for (int i = 0; i < 100; ++i) {
        launch::LaunchPrep::Config qc;
        qc.dwell_ms = 0;
        launch::LaunchPrep q(qc);
        q.set_target(cmd, t0);
        auto a = Clock::now();
        q.poll(t0 + std::chrono::seconds(2));
        tot += std::chrono::duration<double, std::micro>(Clock::now() - a).count();
      }
      std::printf("prep.poll() spawning     %8.2f\n", tot / 100);
    }
    std::system(("rm -rf " + d).c_str());
  }

  // ---- SIGTERM while a game runs (kept last: the flag stays set for the rest of the process)
  {
    string d = tmpdir("term");
    write_exec(d + "/game.sh", "echo $$ > " + d + "/pid\nexec sleep 30\n");
    CHECK(!launch::term_requested());
    pid_t killer = ::fork();
    if (killer == 0) {  // the "init system": waits for the game to start, then stops zfront
      for (int i = 0; i < 3000 && !file_exists(d + "/pid"); ++i) ::usleep(1000);
      ::usleep(100000);
      ::kill(getppid(), SIGTERM);
      ::_exit(0);
    }
    auto t0 = Clock::now();
    launch::RunResult r = launch::run(d + "/game.sh");  // we are still alive here: the signal went to the game
    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    ::waitpid(killer, nullptr, 0);
    CHECK(launch::term_requested());
    CHECK(secs < 10.0);
    CHECK(r.exit_code != 0);  // killed by the signal: no playcount
    std::string pid = slurp(d + "/pid");
    CHECK(::kill(std::atoi(pid.c_str()), 0) != 0);  // the game is gone, not orphaned
    // execute(): no window rebuild once stopped
    launch::Env env;
    int restored = 0;
    env.restore = [&] { ++restored; };
    env.script_dirs = {d + "/none"};
    launch::Request rq;
    rq.cmd = "/bin/true";
    launch::execute(rq, env);
    CHECK_EQ(restored, 0);
    std::system(("rm -rf " + d).c_str());
  }
  return zt::done();
}
