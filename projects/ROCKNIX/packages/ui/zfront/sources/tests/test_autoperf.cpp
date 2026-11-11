// PERFORMANCE SUGGESTIONS (autoperf.hpp): the sampler on a fake sysfs / proc tree, the classifier on synthetic series, the thread and the
// hook of launch::run, the state kept between sessions, and the rows in the game's options (shown, enabling writes the key of the
// PERFORMANCE MODE row, dismissal persists). The drawing of the options screen is measured by test_layout.
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "autoperf.hpp"
#include "check.hpp"
#include "launch.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "registry.hpp"
#include "screens.hpp"
#include "util.hpp"

using namespace zf;
using namespace zf::autoperf;

namespace {

const char* kFeatures = R"(<features>
  <emulator name="libretro" features="ratio">
    <core name="gambatte">
      <feature name="performance mode" value="perfmode" preset="switchauto"/>
    </core>
  </emulator>
</features>)";

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

void mkdirs(const std::string& p) {
  std::string cur;
  for (size_t i = 0; i <= p.size(); ++i) {
    if (i == p.size() || p[i] == '/') {
      if (!cur.empty()) ::mkdir(cur.c_str(), 0755);
    }
    if (i < p.size()) cur += p[i];
  }
}

void spit(const std::string& p, const std::string& t) {
  std::ofstream f(p, std::ios::binary);
  f << t;
}

std::string slurp(const std::string& p) {
  std::string t;
  read_file(p, t);
  return t;
}

bool has_line(const std::string& text, const std::string& line) {
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    if (text.substr(i, e == std::string::npos ? std::string::npos : e - i) == line) return true;
    i = e == std::string::npos ? text.size() : e + 1;
  }
  return false;
}

// "<pid> (emu x) S <ppid> 9 zeros <utime> <stime> ..." as /proc writes it
std::string stat_line(int pid, int ppid, unsigned long ut, unsigned long st) {
  return std::to_string(pid) + " (emu x) S " + std::to_string(ppid) + " 0 0 0 0 0 0 0 0 0 " + std::to_string(ut) + " " + std::to_string(st) + " 0 0 20 0 1 0\n";
}

void put_task(const std::string& proc, int pid, int tid, int ppid, unsigned long ut, unsigned long st) {
  mkdirs(proc + "/" + std::to_string(pid) + "/task/" + std::to_string(tid));
  spit(proc + "/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/stat", stat_line(tid, ppid, ut, st));
  if (tid == pid) spit(proc + "/" + std::to_string(pid) + "/stat", stat_line(pid, ppid, ut, st));
}

Paths fake_paths(const std::string& d) {
  Paths p;
  p.gpu_dir = d + "/gpu";
  p.dmc_dir = d + "/dmc";
  p.proc_root = d + "/proc";
  p.busy_files = {d + "/nobusy", d + "/gpu/load"};
  p.clk_tck = 100;
  return p;
}

Series mk(size_t n, double gpu, double busy, double cpu, double dmc = 328) {
  Series s;
  s.gpu_min = 200, s.gpu_max = 600, s.dmc_min = 194, s.dmc_max = 666;
  for (size_t i = 0; i < n; ++i) {
    Sample x;
    x.t_s = double(i) * 2;
    x.gpu_mhz = gpu, x.gpu_busy = busy, x.cpu_hot = cpu, x.cpu_total = cpu, x.dmc_mhz = dmc;
    s.samples.push_back(x);
  }
  s.secs = double(n) * 2;
  return s;
}

System make_gb() {
  System s;
  s.name = "gb";
  s.fullname = "Game Boy";
  s.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR%";
  s.emulator = "libretro";
  s.core = "gambatte";
  s.emus = {{"libretro", {"gambatte"}}};
  s.rom_dir = "/roms/gb";
  s.loaded = true;
  Game a;
  a.name = "Tetris";
  a.path = "/roms/gb/Tetris.gb";
  s.games = {a};
  s.count = 1;
  return s;
}

System make_nes() {  // libretro too, but its core has no PERFORMANCE MODE
  System s;
  s.name = "nes";
  s.fullname = "NES";
  s.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR%";
  s.emulator = "libretro";
  s.core = "fceumm";
  s.emus = {{"libretro", {"fceumm"}}};
  s.rom_dir = "/roms/nes";
  s.loaded = true;
  Game a;
  a.name = "Mario";
  a.path = "/roms/nes/Mario.nes";
  s.games = {a};
  s.count = 1;
  return s;
}

Menu* as_menu(const std::unique_ptr<Screen>& s) { return static_cast<Menu*>(s.get()); }

int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-test-autoperf-XXXXXX";
  const char* dp = mkdtemp(tmpl);
  CHECK(dp != nullptr);
  if (!dp) return 1;
  const std::string dir = dp;

  // ---- the sampler: frequencies, GPU load, the busiest thread of the game's process tree (and not another process)
  {
    const std::string d = dir + "/fake";
    mkdirs(d + "/gpu");
    mkdirs(d + "/dmc");
    spit(d + "/gpu/cur_freq", "200000000\n");
    spit(d + "/gpu/available_frequencies", "200000000 300000000 400000000 480000000 600000000\n");
    spit(d + "/dmc/cur_freq", "328000000\n");
    spit(d + "/dmc/min_freq", "194000000\n");  // no available_frequencies: min / max
    spit(d + "/dmc/max_freq", "666000000\n");
    spit(d + "/gpu/load", "85@200000000Hz\n");
    const std::string proc = d + "/proc";
    put_task(proc, 100, 100, 1, 0, 0);    // the script we spawned
    put_task(proc, 100, 101, 1, 0, 0);    // a second thread of it
    put_task(proc, 200, 200, 100, 0, 0);  // the emulator it forked
    put_task(proc, 300, 300, 1, 0, 0);    // somebody else
    Sampler sm(fake_paths(d));
    Series rng;
    sm.fill_ranges(rng);
    CHECK_EQ(rng.gpu_min, 200.0);
    CHECK_EQ(rng.gpu_max, 600.0);
    CHECK_EQ(rng.dmc_min, 194.0);
    CHECK_EQ(rng.dmc_max, 666.0);
    Sample a = sm.take(100, 0);
    CHECK_EQ(a.gpu_mhz, 200.0);
    CHECK_EQ(a.dmc_mhz, 328.0);
    CHECK_EQ(a.gpu_busy, 85.0);
    CHECK(a.cpu_hot < 0);  // the first sample has nothing to compare with
    put_task(proc, 100, 100, 1, 150, 50);  // +200 ticks in 2 s at 100 Hz = one core
    put_task(proc, 100, 101, 1, 100, 0);   // half a core
    put_task(proc, 200, 200, 100, 20, 0);  // 10 %
    put_task(proc, 300, 300, 1, 5000, 0);  // not ours: must not count
    Sample b = sm.take(100, 2);
    CHECK(b.cpu_hot > 99.9 && b.cpu_hot < 100.1);
    CHECK(b.cpu_total > 159.9 && b.cpu_total < 160.1);
    // kbase's debugfs counters ("busy_time N idle_time M", cumulative): a percentage between two samples
    spit(d + "/gpu/load", "busy_time: 1000 idle_time: 3000\n");
    CHECK(sm.take(100, 3).gpu_busy < 0);  // the first reading has nothing to compare with
    spit(d + "/gpu/load", "busy_time: 1900 idle_time: 3100\n");
    const double kb = sm.take(100, 3.5).gpu_busy;
    CHECK(kb > 89.9 && kb < 90.1);
    // no load file readable: -1; an out of range number is no load
    spit(d + "/gpu/load", "garbage\n");
    CHECK(sm.take(100, 4).gpu_busy < 0);
    spit(d + "/gpu/load", "4242\n");
    CHECK(sm.take(100, 6).gpu_busy < 0);
    // the game is gone: unknown, not zero
    Paths gone = fake_paths(d);
    gone.proc_root = d + "/nothing";
    Sampler sg(gone);
    sg.take(100, 0);
    CHECK(sg.take(100, 2).cpu_hot < 0);
  }

  // ---- the classifier
  {
    CHECK(classify(mk(70, 200, 95, 60), false) == Verdict::GovernorLimited);   // GPU low, GPU busy
    CHECK(classify(mk(70, 200, -1, 98), false) == Verdict::GovernorLimited);   // no load data: saturated thread, GPU and RAM low
    CHECK(classify(mk(70, 200, -1, 98, 666), false) == Verdict::Unclear);      // ... but the RAM was at the top: not the governor
    CHECK(classify(mk(70, 200, -1, 60), false) == Verdict::Unclear);           // no load data and no saturated thread: nothing to say
    CHECK(classify(mk(70, 200, 20, 99), false) == Verdict::CpuBound);          // the GPU idles while a thread is saturated
    CHECK(classify(mk(70, 600, 95, 60, 666), false) == Verdict::Unclear);      // the GPU did climb: the governor was not the limit
    CHECK(classify(mk(70, 200, 5, 10), false) == Verdict::Idle);               // a menu left open
    CHECK(classify(mk(70, 200, -1, 10), false) == Verdict::Idle);
    CHECK(classify(mk(30, 200, 95, 60), false) == Verdict::TooShort);          // 60 s
    CHECK(classify(mk(59, 200, 95, 60), false) == Verdict::TooShort);          // 118 s
    CHECK(classify(mk(60, 200, 95, 60), false) != Verdict::TooShort);          // 120 s
    CHECK(classify(mk(70, 200, 95, 60), true) == Verdict::PerfmodeOn);         // already on: ignored
    Series none = mk(70, 200, 95, 60);
    none.gpu_max = 0;
    none.gpu_min = 0;
    CHECK(classify(none, false) == Verdict::NoData);                           // the steps are unknown
    Series nogpu = mk(70, -1, 95, 60);
    CHECK(classify(nogpu, false) == Verdict::NoData);                          // the GPU frequency was unreadable
    Series few = mk(70, 200, 95, 60);
    few.samples.resize(10);
    CHECK(classify(few, false) == Verdict::NoData);
    // loading is ignored: the first 20 s were limited, the rest was not
    Series warm = mk(70, 600, 95, 60, 666);
    for (size_t i = 0; i < 10; ++i) warm.samples[i].gpu_mhz = 200;
    CHECK(classify(warm, false) == Verdict::Unclear);
    // half and half is not "most of the samples"
    Series half = mk(70, 200, 95, 60);
    for (size_t i = 0; i < half.samples.size(); i += 2) half.samples[i].gpu_mhz = 600;
    CHECK(classify(half, false) == Verdict::Unclear);
    // 7 of 10 with the load data passes, 5 of 10 without it does not
    Series seven = mk(70, 600, 95, 60);
    for (size_t i = 0; i < seven.samples.size(); ++i)
      if (i % 10 < 7) seven.samples[i].gpu_mhz = 200;
    CHECK(classify(seven, false) == Verdict::GovernorLimited);
    Series seven_nb = mk(70, 600, -1, 98);
    for (size_t i = 0; i < seven_nb.samples.size(); ++i)
      if (i % 10 < 7) seven_nb.samples[i].gpu_mhz = 200;
    CHECK(classify(seven_nb, false) == Verdict::Unclear);  // 70 % < 75 %: the weaker evidence asks for more
    CHECK(std::string(verdict_name(Verdict::GovernorLimited)) == "governor-limited");
  }

  // ---- the thread and the hook of launch::run
  {
    const std::string d = dir + "/fake2";
    mkdirs(d + "/gpu");
    mkdirs(d + "/dmc");
    mkdirs(d + "/proc");
    spit(d + "/gpu/cur_freq", "200000000\n");
    spit(d + "/gpu/available_frequencies", "200000000 600000000\n");
    spit(d + "/dmc/cur_freq", "328000000\n");
    spit(d + "/dmc/available_frequencies", "194000000 666000000\n");
    {
      Monitor mon(fake_paths(d), 0.02);
      Series none = mon.stop();  // never started
      CHECK(none.samples.empty());
      mon.start(4242);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      Series s = mon.stop();
      CHECK(s.samples.size() >= 2);
      CHECK(s.secs >= 0.1);
      CHECK_EQ(s.gpu_max, 600.0);
      CHECK_EQ(s.samples.back().gpu_mhz, 200.0);
      CHECK(s.samples.back().t_s > s.samples.front().t_s);
    }
    CHECK(!armed());
    Series got;
    CHECK(!take_series(got));
    launch::run("sleep 0.2");  // not armed: nothing is sampled
    CHECK(!take_series(got));
    arm(fake_paths(d), 0.02);
    CHECK(armed());
    launch::RunResult rr = launch::run("sleep 0.3");
    CHECK_EQ(rr.exit_code, 0);
    CHECK(!armed());  // one run per arming
    CHECK(take_series(got));
    CHECK(got.samples.size() >= 2);
    CHECK(got.secs >= 0.2);
    CHECK(!take_series(got));  // handed over once
    arm(fake_paths(d), 0.02);
    disarm();
    launch::run("true");
    CHECK(!take_series(got));
  }

  // ---- the state kept between sessions
  const std::string store = dir + "/state/autoperf.tsv";
  ::setenv("ZF_AUTOPERF", store.c_str(), 1);
  CHECK_EQ(store_path(), store);
  {
    CHECK(get_state(store, "psp", "/r/GoW.iso") == State::None);
    CHECK(set_state(store, "psp", "/r/GoW.iso", State::Suggest));  // the folder is made
    CHECK(get_state(store, "psp", "/r/GoW.iso") == State::Suggest);
    CHECK(get_state(store, "psp", "/r/Other.iso") == State::None);
    CHECK(get_state(store, "n64", "/r/GoW.iso") == State::None);
    CHECK(set_state(store, "n64", "/r/Tennis\t.z64", State::Dismissed));  // a tab in a name cannot break the format
    CHECK(get_state(store, "n64", "/r/Tennis\t.z64") == State::Dismissed);
    CHECK(set_state(store, "psp", "/r/GoW.iso", State::Accepted));
    CHECK(get_state(store, "psp", "/r/GoW.iso") == State::Accepted);
    CHECK(set_state(store, "psp", "/r/GoW.iso", State::None));
    CHECK(get_state(store, "psp", "/r/GoW.iso") == State::None);
    CHECK(get_state(store, "n64", "/r/Tennis\t.z64") == State::Dismissed);
    CHECK_EQ(slurp(store), std::string("dismissed\tn64\t/r/Tennis .z64\n"));
    // record_result: only a governor-limited session of a game nobody has decided about makes a (new) suggestion
    ::unlink(store.c_str());
    CHECK(!record_result(store, "psp", "a.iso", Verdict::CpuBound));
    CHECK(!record_result(store, "psp", "a.iso", Verdict::Idle));
    CHECK(!record_result(store, "psp", "a.iso", Verdict::TooShort));
    CHECK(get_state(store, "psp", "a.iso") == State::None);
    CHECK(record_result(store, "psp", "a.iso", Verdict::GovernorLimited));
    CHECK(get_state(store, "psp", "a.iso") == State::Suggest);
    CHECK(!record_result(store, "psp", "a.iso", Verdict::GovernorLimited));  // not repeated
    CHECK(!record_result(store, "psp", "a.iso", Verdict::PerfmodeOn) && get_state(store, "psp", "a.iso") == State::Accepted);  // turned on by hand
    CHECK(set_state(store, "psp", "b.iso", State::Dismissed));
    CHECK(!record_result(store, "psp", "b.iso", Verdict::GovernorLimited));  // a dismissal is for good
    CHECK(get_state(store, "psp", "b.iso") == State::Dismissed);
    ::unlink(store.c_str());
  }

  // ---- with the settings and the menus
  mg::env().root = dir + "/root";
  mkdirs(dir + "/root/storage/.config/emulationstation");
  spit(dir + "/root/storage/.config/emulationstation/es_features.cfg", kFeatures);
  mg::reset_caches();
  Ctx cx;
  {
    std::string err;
    CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
    cx.w = 640;
    cx.h = 480;
  }
  cx.sv->cmd.dry_run = true;
  SystemConf::lock_path() = dir + "/lock";
  spit(dir + "/system.cfg", "audio.volume=50\n");
  cx.conf = SystemConf::load(dir + "/system.cfg");
  cx.systems = {make_gb(), make_nes()};
  const std::string rom = "/roms/gb/Tetris.gb";
  const std::string key = "gb[\"Tetris.gb\"].perfmode";
  {
    CHECK(enabled(cx));  // on by default: it only suggests
    CHECK(supported(cx, "gb", "Tetris.gb"));
    CHECK(!supported(cx, "nes", "Mario.nes"));
    CHECK(!supported(cx, "nope", "x"));
    CHECK(!perfmode_on(cx, "gb", "Tetris.gb"));
    // game, system and global values: the nearest one that says 1 or 0 wins
    mg::conf_put(cx, "global.perfmode", "1");
    CHECK(perfmode_on(cx, "gb", "Tetris.gb"));
    mg::conf_put(cx, "gb.perfmode", "0");
    CHECK(!perfmode_on(cx, "gb", "Tetris.gb"));
    mg::conf_put(cx, key, "1");
    CHECK(perfmode_on(cx, "gb", "Tetris.gb"));
    mg::conf_put(cx, key, "");
    mg::conf_put(cx, "gb.perfmode", "");
    mg::conf_put(cx, "global.perfmode", "");
    CHECK(!perfmode_on(cx, "gb", "Tetris.gb"));
  }
  // begin_session arms only for a game that could be suggested something
  {
    disarm();
    begin_session(cx, "gb", rom);
    CHECK(armed());
    begin_session(cx, "nes", "/roms/nes/Mario.nes");  // no PERFORMANCE MODE there
    CHECK(!armed());
    mg::conf_put(cx, key, "1");
    begin_session(cx, "gb", rom);  // already on
    CHECK(!armed());
    mg::conf_put(cx, key, "0");  // the game's own OFF is a decision: left alone
    begin_session(cx, "gb", rom);
    CHECK(!armed());
    mg::conf_put(cx, key, "");
    mg::conf_put(cx, kKey, "0");  // switched off in GAMES
    CHECK(!enabled(cx));
    begin_session(cx, "gb", rom);
    CHECK(!armed());
    mg::conf_put(cx, kKey, "");
    CHECK(enabled(cx));
    set_state(store_path(), "gb", rom, State::Dismissed);  // decided already: no sampler
    begin_session(cx, "gb", rom);
    CHECK(!armed());
    set_state(store_path(), "gb", rom, State::None);
    disarm();
  }
  // a finished session: toast once, the row appears, a failed or short one says nothing
  {
    CHECK(!pending(cx, "gb", rom));
    CHECK(conclude(cx, "gb", rom, mk(70, 200, 50, 80)).empty());                      // unclear
    CHECK(conclude(cx, "gb", rom, mk(20, 200, 95, 60)).empty());                      // short
    CHECK(!pending(cx, "gb", rom));
    arm(fake_paths(dir + "/fake2"), 0.02);
    launch::run("sleep 0.1");
    CHECK(end_session(cx, "gb", rom, 3, 1).empty());                                  // exit code 1: ignored (and the series consumed)
    CHECK(end_session(cx, "gb", rom, 3, 0).empty());                                  // nothing left
    const std::string toast = conclude(cx, "gb", rom, mk(70, 200, 95, 60));
    CHECK(!toast.empty());
    CHECK(pending(cx, "gb", rom));
    CHECK(conclude(cx, "gb", rom, mk(70, 200, 95, 60)).empty());                      // not twice
    CHECK(get_state(store_path(), "gb", rom) == State::Suggest);
    CHECK(!pending(cx, "gb", "/roms/gb/Other.gb"));
    CHECK(!pending(cx, "nes", "/roms/nes/Mario.nes"));
    mg::conf_put(cx, kKey, "0");
    CHECK(!pending(cx, "gb", rom));  // the global switch hides it
    mg::conf_put(cx, kKey, "");
    CHECK(pending(cx, "gb", rom));
    // perfmode on already (by hand, in the other row): nothing to suggest, and a standing suggestion is closed by the next session
    mg::conf_put(cx, key, "1");
    CHECK(!pending(cx, "gb", rom));
    // a real session reaches the verdict (end_session once dropped the series before reading it): the open suggestion is accepted
    arm(fake_paths(dir + "/fake2"), 0.02);
    launch::run("sleep 0.1");
    CHECK(end_session(cx, "gb", rom, 3, 0).empty());
    CHECK(get_state(store_path(), "gb", rom) == State::Accepted);
    CHECK(!armed());
    set_state(store_path(), "gb", rom, State::Suggest);
    mg::conf_put(cx, key, "0");
    CHECK(!pending(cx, "gb", rom));  // explicitly OFF for this game: not suggested
    mg::conf_put(cx, key, "");
    CHECK(pending(cx, "gb", rom));
  }
  // the rows of the game's options
  {
    auto open = [&] {
      std::unique_ptr<Screen> s = Registry::instance().make("menu.game_options", cx, mg::game_arg("gb", rom));
      CHECK(s != nullptr);
      as_menu(s)->rebuild(cx);
      return s;
    };
    std::string on = "TURN ON PERFORMANCE MODE FOR THIS GAME", off = "STOP SUGGESTING PERFORMANCE MODE";
    {
      auto s = open();
      Menu* m = as_menu(s);
      CHECK(find(*m, on) >= 0 && find(*m, off) == find(*m, on) + 1 && find(*m, on) < 3);  // at the top, where it is seen
      CHECK(find(*m, "PERFORMANCE MODE") >= 0);        // the normal row is still there
      // dismissed: gone, and it stays gone
      m->entry(size_t(find(*m, off))).on_a(cx);
      CHECK(get_state(store_path(), "gb", rom) == State::Dismissed);
      CHECK(!has_line(slurp(dir + "/system.cfg"), key + "=1"));
      auto s2 = open();
      CHECK(find(*as_menu(s2), on) < 0 && find(*as_menu(s2), off) < 0);
      CHECK(!record_result(store_path(), "gb", rom, Verdict::GovernorLimited));
    }
    {
      set_state(store_path(), "gb", rom, State::Suggest);
      auto s = open();
      Menu* m = as_menu(s);
      int at = find(*m, on);
      CHECK(at >= 0);
      m->entry(size_t(at)).on_a(cx);  // one press: the same key and value as the PERFORMANCE MODE row
      CHECK(has_line(slurp(dir + "/system.cfg"), key + "=1"));
      CHECK(get_state(store_path(), "gb", rom) == State::Accepted);
      CHECK(perfmode_on(cx, "gb", "Tetris.gb"));
      CHECK(!cx.sv->toasts.items().empty());
      auto s2 = open();
      CHECK(find(*as_menu(s2), on) < 0);
      // the normal row shows ON for it (the storage is the same)
      int pm = find(*as_menu(s2), "PERFORMANCE MODE");
      CHECK(pm >= 0 && as_menu(s2)->entry(size_t(pm)).get_s() == "1");
    }
    {
      // a game of a system whose emulator has no PERFORMANCE MODE never gets the rows, even with a stale line in the file
      set_state(store_path(), "nes", "/roms/nes/Mario.nes", State::Suggest);
      std::unique_ptr<Screen> s = Registry::instance().make("menu.game_options", cx, mg::game_arg("nes", "/roms/nes/Mario.nes"));
      CHECK(s != nullptr);
      as_menu(s)->rebuild(cx);
      CHECK(find(*as_menu(s), on) < 0);
    }
    // GAMES > PERFORMANCE SUGGESTIONS writes the switch
    {
      std::unique_ptr<Screen> s = Registry::instance().make("menu.game", cx, "");
      CHECK(s != nullptr);
      Menu* m = as_menu(s);
      m->rebuild(cx);
      int sw = find(*m, "PERFORMANCE SUGGESTIONS");
      CHECK(sw >= 0);
      CHECK(m->entry(size_t(sw)).get_b());  // on by default
      m->entry(size_t(sw)).set_b(false);
      CHECK(has_line(slurp(dir + "/system.cfg"), std::string(kKey) + "=0"));
      CHECK(!enabled(cx));
      m->entry(size_t(sw)).set_b(true);
      CHECK(enabled(cx));
      CHECK(slurp(dir + "/system.cfg").find(kKey) == std::string::npos);
    }
  }

  std::string rm = "rm -rf '" + dir + "'";
  if (std::system(rm.c_str()) != 0) {}
  return zt::done();
}
