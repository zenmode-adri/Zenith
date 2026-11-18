// PLAY TIME LIMIT (playtime.hpp): the day's counter with an injected clock. Day change, a clock that jumps back or forward, a restart in
// the middle of a game, a limit changed during the day, Off, the warning at exactly 5 minutes, an empty or corrupt file, the atomic
// save (at most once a minute), and the watcher thread (warning and close hooks, with a real but very short wait).
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "check.hpp"
#include "playtime.hpp"
#include "systems.hpp"
#include "uimode.hpp"

using namespace zf;
using namespace zf::playtime;

namespace {

constexpr long kMin = 60 * 1000;

Date d(int y, int m, int day) {
  Date x;
  x.year = y;
  x.month = m;
  x.day = day;
  return x;
}

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool exists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

void test_limit_setting() {
  CHECK_EQ(limit_minutes(""), 0);
  CHECK_EQ(limit_minutes("0"), 0);
  CHECK_EQ(limit_minutes("off"), 0);
  CHECK_EQ(limit_minutes("15"), 15);
  CHECK_EQ(limit_minutes("120"), 120);
  CHECK_EQ(limit_minutes("-5"), 0);
  CHECK_EQ(limit_minutes("abc"), 0);
  CHECK_EQ(limit_minutes("30x"), 0);
  CHECK_EQ(limit_minutes("99999"), 0);  // nothing the menu offers: treated as Off, never as "a lot of time"
}

void test_date() {
  CHECK(d(2026, 11, 15).valid());
  CHECK(!d(2023, 12, 31).valid());  // a console with no battery clock boots in 1970 / 2000
  CHECK(!d(1970, 1, 1).valid());
  CHECK(!d(2026, 0, 5).valid());
  CHECK(!d(2026, 13, 5).valid());
  CHECK(!d(2026, 5, 0).valid());
  CHECK(!d(2026, 5, 32).valid());
  CHECK(!d(2100, 1, 1).valid());
  CHECK(!Date().valid());
  CHECK_EQ(d(2026, 11, 5).key(), std::string("2026-11-05"));
  CHECK(d(2026, 11, 5).key() < d(2026, 11, 15).key());
  const Date t = local_today();  // the real clock of the PC: only the shape
  CHECK(t.year == 0 || t.valid());
}

void test_future_stored_day() {
  const std::string h = "zfront-playtime 1\n";
  Tracker t = Tracker::parse(h + "day=2030-01-01\nused=600\n");
  CHECK_EQ(t.day(), std::string("2030-01-01"));
  t.set_date(d(2026, 11, 15));  // far ahead: today is adopted, what was counted stays
  CHECK_EQ(t.day(), std::string("2026-11-15"));
  CHECK_EQ(t.used_ms(), 600 * 1000L);
  CHECK(t.dirty());
  t.set_date(d(2026, 11, 16));  // the real next day starts again
  CHECK_EQ(t.used_ms(), 0L);
  CHECK_EQ(t.day(), std::string("2026-11-16"));
  Tracker u = Tracker::parse(h + "day=2026-11-16\nused=600\n");  // 1 day: the clock went back a little, nothing changes
  u.set_date(d(2026, 11, 15));
  CHECK_EQ(u.day(), std::string("2026-11-16"));
  Tracker v = Tracker::parse(h + "day=2026-11-17\nused=600\n");  // 2 days: adopted
  v.set_date(d(2026, 11, 15));
  CHECK_EQ(v.day(), std::string("2026-11-15"));
  Tracker w = Tracker::parse(h + "day=2027-01-01\nused=60\n");  // across a year end: 2 days
  w.set_date(d(2026, 12, 30));
  CHECK_EQ(w.day(), std::string("2026-12-30"));
  Tracker x = Tracker::parse(h + "day=2027-01-01\nused=60\n");  // 1 day
  x.set_date(d(2026, 12, 31));
  CHECK_EQ(x.day(), std::string("2027-01-01"));
  Tracker y = Tracker::parse(h + "day=2028-03-01\nused=60\n");  // leap year, 29 Feb in between: 2 days
  y.set_date(d(2028, 2, 28));
  CHECK_EQ(y.day(), std::string("2028-02-28"));
  Tracker y2 = Tracker::parse(h + "day=2028-03-01\nused=60\n");  // 1 day
  y2.set_date(d(2028, 2, 29));
  CHECK_EQ(y2.day(), std::string("2028-03-01"));
  Tracker z = Tracker::parse(h + "day=2030-01-01\nused=600\n");  // an invalid today adopts nothing
  z.set_date(d(1970, 1, 1));
  CHECK_EQ(z.day(), std::string("2030-01-01"));
  Tracker q = Tracker::parse(h + "day=2030-01-01\nused=1200\n");  // time spent stays spent
  q.set_date(d(2026, 11, 15));
  CHECK_EQ(q.left_ms(30), 10 * kMin);
}

void test_games_and_tools() {
  System gb, ports, tools, music, shots;
  gb.name = "gb", gb.platform = "gb";
  ports.name = "ports", ports.platform = "pc";
  tools.name = "tools", tools.platform = "tools", tools.theme = "tools";
  music.name = "music", music.platform = "music";
  shots.name = "imageviewer", shots.platform = "imageviewer", shots.theme = "imageviewer";
  CHECK(counts_as_game(&gb));
  CHECK(counts_as_game(&ports));   // PortMaster counts
  CHECK(!counts_as_game(&tools));  // the Tuner, the Tools scripts
  CHECK(!counts_as_game(&music));
  CHECK(!counts_as_game(&shots));
  CHECK(counts_as_game(nullptr));  // not a listed system: the safe side is the limit
}

void test_counting_and_roll() {
  Tracker t;
  CHECK_EQ(t.used_ms(), 0L);
  CHECK_EQ(t.left_ms(0), -1L);  // Off
  t.set_date(d(2026, 11, 15));
  CHECK_EQ(t.day(), std::string("2026-11-15"));
  t.add(10 * kMin, 30);
  CHECK_EQ(t.used_ms(), 10 * kMin);
  CHECK_EQ(t.left_ms(30), 20 * kMin);
  CHECK(!t.exhausted(30));
  CHECK(t.exhausted(10));
  CHECK(!t.exhausted(0));  // Off never blocks
  t.add(-5000, 30);        // a negative step gives no time and takes none
  CHECK_EQ(t.used_ms(), 10 * kMin);

  t.set_date(d(2026, 11, 15));  // same day: kept
  CHECK_EQ(t.used_ms(), 10 * kMin);
  t.set_date(d(2026, 11, 14));  // the clock went BACK: no free time, nothing lost
  CHECK_EQ(t.used_ms(), 10 * kMin);
  CHECK_EQ(t.day(), std::string("2026-11-15"));
  t.set_date(d(1970, 1, 1));  // clock lost: invalid, ignored
  t.set_date(d(2000, 6, 1));
  t.set_date(Date());
  CHECK_EQ(t.used_ms(), 10 * kMin);
  CHECK_EQ(t.day(), std::string("2026-11-15"));
  t.set_date(d(2026, 11, 16));  // a later valid day: new day, zero
  CHECK_EQ(t.used_ms(), 0L);
  CHECK_EQ(t.day(), std::string("2026-11-16"));
  t.add(3 * kMin, 30);
  t.set_date(d(2027, 1, 1));  // a month or a year later is the same: zero
  CHECK_EQ(t.used_ms(), 0L);

  // no stored day yet (new console / bad file) and the clock is still invalid: the time counts, and is kept when the clock arrives
  Tracker u;
  u.set_date(d(1970, 1, 1));
  u.add(7 * kMin, 30);
  CHECK_EQ(u.used_ms(), 7 * kMin);
  u.set_date(d(2026, 11, 15));
  CHECK_EQ(u.used_ms(), 7 * kMin);
  CHECK_EQ(u.day(), std::string("2026-11-15"));
}

void test_warning_and_expiry() {
  Tracker t;
  t.set_date(d(2026, 11, 15));
  Events e = t.add(25 * kMin - 1, 30);  // 5 min and 1 ms left
  CHECK(!e.warn && !e.expired);
  e = t.add(1, 30);  // exactly 5 minutes left
  CHECK(e.warn);
  CHECK(!e.expired);
  e = t.add(1000, 30);  // not again
  CHECK(!e.warn && !e.expired);
  e = t.add(5 * kMin - 1001, 30);  // 1 ms left
  CHECK(!e.warn && !e.expired);
  e = t.add(1, 30);  // at the limit
  CHECK(e.expired);
  CHECK(!e.warn);
  CHECK(t.exhausted(30));
  e = t.add(1000, 30);  // once
  CHECK(!e.expired);
  CHECK_EQ(t.left_ms(30), 0L);  // never below zero

  // a short limit: the first look is already inside the last 5 minutes
  Tracker s;
  s.set_date(d(2026, 11, 15));
  e = s.add(1, 15);
  CHECK(!e.warn);  // 15 min: 14:59.999 left, still more than 5
  Tracker r;
  r.set_date(d(2026, 11, 15));
  r.add(12 * kMin, 15);
  CHECK(r.add(0, 15).expired == false);
}

void test_limit_changes() {
  Tracker t;
  t.set_date(d(2026, 11, 15));
  t.add(20 * kMin, 30);  // 10 min left: no warning yet
  Events e = t.add(0, 30);
  CHECK(!e.warn);
  e = t.add(5 * kMin + 1, 30);  // under 5 min: warning
  CHECK(e.warn);
  e = t.add(0, 60);  // the parent raises the limit to 60: 34:59 left, nothing happens
  CHECK(!e.warn && !e.expired);
  CHECK_EQ(t.left_ms(60), 60 * kMin - (25 * kMin + 1));
  e = t.add(30 * kMin, 60);  // used 55:00.001: the warning is armed again
  CHECK(e.warn);
  e = t.add(0, 15);  // lowered below what was already used: closes now
  CHECK(e.expired);
  CHECK(t.exhausted(15));
  e = t.add(0, 0);  // Off: free again, no events
  CHECK(!e.warn && !e.expired);
  CHECK(!t.exhausted(0));
  t.add(10 * kMin, 0);  // Off still counts (the day's total), it just never blocks
  CHECK_EQ(t.used_ms(), 25 * kMin + 1 + 30 * kMin + 10 * kMin);
  e = t.add(0, 120);
  CHECK(!e.warn && !e.expired);
}

void test_text_and_corruption() {
  Tracker t;
  t.set_date(d(2026, 11, 15));
  t.add(12 * kMin + 345, 30);
  const std::string txt = t.text();
  Tracker u = Tracker::parse(txt);
  CHECK_EQ(u.day(), std::string("2026-11-15"));
  CHECK_EQ(u.used_ms(), 12 * kMin);  // saved in whole seconds
  // empty, garbage, half a file, wrong numbers: no time carried over, no crash, a clean state
  const char* bad[] = {"",
                       "\n",
                       "garbage",
                       "zfront-playtime 1\n",
                       "zfront-playtime 1\nday=2026-11-15\n",
                       "zfront-playtime 1\nday=2026-11-15\nused=abc\n",
                       "zfront-playtime 1\nday=2026-11-15\nused=-4\n",
                       "zfront-playtime 1\nday=2026-11-15\nused=99999999999999999999\n",
                       "zfront-playtime 1\nday=2026-11-15\nused=200000\n",  // more than two days in one: impossible
                       "zfront-playtime 1\nday=15/11/2026\nused=60\n",
                       "zfront-playtime 2\nday=2026-11-15\nused=60\n",
                       "day=2026-11-15\nused=60\n"};
  for (const char* b : bad) {
    Tracker x = Tracker::parse(b);
    if (x.used_ms() != 0 || !x.day().empty()) std::fprintf(stderr, "test_playtime: accepted [%s]\n", b);
    CHECK_EQ(x.used_ms(), 0L);
    CHECK(x.day().empty());
  }
  {  // a file with NUL bytes in it (a torn write): not ours
    const char raw[] = "zfront-playtime 1\nday=2026-11-15\nused=6\0\0\0";
    Tracker x = Tracker::parse(std::string(raw, sizeof raw - 1));
    CHECK_EQ(x.used_ms(), 0L);
    CHECK(x.day().empty());
  }
  // a stored day that is not a valid date is as bad as none
  Tracker y = Tracker::parse("zfront-playtime 1\nday=2026-13-45\nused=60\n");
  CHECK(y.day().empty());
  CHECK_EQ(y.used_ms(), 0L);
  // trailing blank lines and CRLF are fine (somebody edited it)
  Tracker z = Tracker::parse("zfront-playtime 1\r\nday=2026-11-15\r\nused=60\r\n\r\n");
  CHECK_EQ(z.used_ms(), 60 * 1000L);
}

void test_saver_and_restart() {
  const std::string dir = zt::tmp_base() + "/zf_playtime_" + std::to_string(::getpid());
  ::mkdir(dir.c_str(), 0755);
  const std::string path = dir + "/playtime";

  // no file / empty file / corrupt file: a clean tracker
  CHECK_EQ(load_file(path).used_ms(), 0L);
  { std::ofstream(path, std::ios::binary).close(); }
  CHECK_EQ(load_file(path).used_ms(), 0L);
  { std::ofstream(path, std::ios::binary) << "\x01\x02 not our file"; }
  CHECK_EQ(load_file(path).used_ms(), 0L);
  ::unlink(path.c_str());

  Tracker t = load_file(path);
  Saver sv(path);
  t.set_date(d(2026, 11, 15));
  t.add(5 * kMin, 30);
  CHECK(t.dirty());
  CHECK(sv.tick(t, 1000));  // the first change is written at once
  CHECK(!t.dirty());
  CHECK(exists(path));
  CHECK_EQ(slurp(path), t.text());
  t.add(kMin, 30);
  CHECK(!sv.tick(t, 1000 + 59 * 1000));  // within a minute: not again
  CHECK(t.dirty());
  CHECK_EQ(load_file(path).used_ms(), 5 * kMin);  // the disk still has the older total
  CHECK(sv.tick(t, 1000 + 60 * 1000));
  CHECK_EQ(load_file(path).used_ms(), 6 * kMin);
  CHECK(!sv.tick(t, 1000 + 200 * 1000));  // nothing changed: nothing written
  t.add(1000, 30);
  CHECK(sv.tick(t, 1000 + 201 * 1000, true));  // forced (end of the game): ignores the minute
  CHECK_EQ(load_file(path).used_ms(), 6 * kMin + 1000);
  // no temporary file left behind
  int files = 0;
  if (DIR* dd = ::opendir(dir.c_str())) {
    while (const dirent* de = ::readdir(dd))
      if (de->d_name[0] != '.') ++files;
    ::closedir(dd);
  }
  CHECK_EQ(files, 1);  // only `playtime`: the temporary file was renamed over it

  // restart in the middle of a game: the new process starts from what the disk had; the same day keeps it, a new day does not
  Tracker after = load_file(path);
  after.set_date(d(2026, 11, 15));
  CHECK_EQ(after.used_ms(), 6 * kMin + 1000);
  Tracker next_day = load_file(path);
  next_day.set_date(d(2026, 11, 16));
  CHECK_EQ(next_day.used_ms(), 0L);

  // a path that cannot be written: false, the tracker stays dirty (it tries again), nothing crashes
  Saver bad(dir + "/no/such/dir/playtime");
  Tracker w;
  w.set_date(d(2026, 11, 15));
  w.add(kMin, 30);
  CHECK(!bad.tick(w, 5000));
  CHECK(w.dirty());

  ::unlink(path.c_str());
  ::rmdir(dir.c_str());
}

void test_session() {
  const std::string dir = zt::tmp_base() + "/zf_playtime_s_" + std::to_string(::getpid());
  ::mkdir(dir.c_str(), 0755);
  const std::string path = dir + "/playtime";
  Tracker t;
  Saver sv(path);
  Session s(t, sv, 30, 100000);
  // game start: 100000 ms on the steady clock. 90 s later:
  Session::Step st = s.advance(100000 + 90 * 1000, d(2026, 11, 15));
  CHECK_EQ(t.used_ms(), 90 * 1000L);
  CHECK(!st.ev.warn && !st.ev.expired);
  CHECK_EQ(load_file(path).used_ms(), 90 * 1000L);  // written (first change)
  // the next wake is the sooner of the warning point (25 min in) and the minute of the save
  CHECK(st.wake_ms > 0 && st.wake_ms <= 60 * 1000);
  // a restart now (the process dies): the disk has 90 s. 30 s more played, then killed: the disk keeps 90 s (lost < 1 minute)
  st = s.advance(100000 + 120 * 1000, d(2026, 11, 15));
  CHECK_EQ(t.used_ms(), 120 * 1000L);
  CHECK_EQ(load_file(path).used_ms(), 90 * 1000L);
  // the steady clock cannot go back, but a bad step must not give time back either
  st = s.advance(100000 + 100 * 1000, d(2026, 11, 15));
  CHECK_EQ(t.used_ms(), 120 * 1000L);
  // jump the day's date back (wall clock set to yesterday): nothing lost
  st = s.advance(100000 + 130 * 1000, d(2026, 11, 14));
  CHECK_EQ(t.used_ms(), 130 * 1000L);
  CHECK_EQ(t.day(), std::string("2026-11-15"));
  // 24 min in: the warning is exactly 5 min before the end; the wake points to it
  st = s.advance(100000 + 24 * kMin, d(2026, 11, 15));
  CHECK(!st.ev.warn);
  CHECK(st.wake_ms <= 60 * 1000);
  Tracker probe = t;
  CHECK_EQ(probe.left_ms(30), 6 * kMin);
  st = s.advance(100000 + 25 * kMin, d(2026, 11, 15));
  CHECK(st.ev.warn);
  CHECK(!st.ev.expired);
  CHECK_EQ(st.wake_ms, 60 * 1000L);  // never longer than a minute
  st = s.advance(100000 + 30 * kMin, d(2026, 11, 15));
  CHECK(st.ev.expired);
  CHECK(t.exhausted(30));
  CHECK_EQ(st.wake_ms, 30 * 1000L);  // it comes back to close again if the game is still there (not sooner: a second SIGTERM kills RetroArch without saving)
  // midnight during the game: a new day, the counter starts again
  st = s.advance(100000 + 31 * kMin, d(2026, 11, 16));
  CHECK_EQ(t.used_ms(), kMin);
  CHECK(!t.exhausted(30));
  s.finish(100000 + 32 * kMin, d(2026, 11, 16));
  CHECK_EQ(load_file(path).used_ms(), 2 * kMin);  // finish saves at once

  // the wake point close to the warning: exact
  Tracker t2;
  Saver sv2(dir + "/p2");
  t2.set_date(d(2026, 11, 15));
  t2.add(24 * kMin + 50 * 1000, 30);  // 10 s before the warning
  Session s2(t2, sv2, 30, 0);
  st = s2.advance(0, d(2026, 11, 15));
  CHECK_EQ(st.wake_ms, 10 * 1000L);
  st = s2.advance(10 * 1000, d(2026, 11, 15));
  CHECK(st.ev.warn);

  // Off: nothing is counted nor written by a session
  Tracker t3;
  Saver sv3(dir + "/p3");
  Session s3(t3, sv3, 0, 0);
  st = s3.advance(5 * kMin, d(2026, 11, 15));
  CHECK_EQ(t3.used_ms(), 0L);
  CHECK(!exists(dir + "/p3"));
  CHECK(!st.ev.warn && !st.ev.expired);

  ::unlink(path.c_str());
  ::unlink((dir + "/p2").c_str());
  ::rmdir(dir.c_str());
}

void test_watcher() {
  const std::string dir = zt::tmp_base() + "/zf_playtime_w_" + std::to_string(::getpid());
  ::mkdir(dir.c_str(), 0755);
  const std::string path = dir + "/playtime";
  Tracker t = Tracker::parse("zfront-playtime 1\nday=2026-11-15\nused=899\n");  // 15 min limit: 1 s left
  Saver sv(path);
  std::atomic<int> warns{0}, expires{0};
  Hooks h;
  std::atomic<long> warned_left{-1};
  h.warn = [&](long left) {
    warned_left = left;
    ++warns;
  };
  h.expire = [&](int attempt) {
    (void)attempt;
    ++expires;
  };
  {
    Watcher w(t, sv, 15, h, [] { return d(2026, 11, 15); }, 0);
    w.start();
    for (int i = 0; i < 100 && expires.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(warns.load(), 1);  // inside the last 5 minutes from the start
    CHECK(warned_left.load() > 0 && warned_left.load() <= 1000);  // it says how much is left, not a fixed 5 minutes
    CHECK(expires.load() >= 1);
    const auto t0 = std::chrono::steady_clock::now();
    w.stop();  // wakes the sleeping thread at once
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500));
    w.stop();  // twice is fine
  }
  CHECK(t.exhausted(15));
  CHECK(load_file(path).used_ms() >= 900 * 1000L);  // stop saves
  // a watcher with nothing to do stops at once and never calls the hooks
  Tracker t2;
  Saver sv2(dir + "/p2");
  warns = 0;
  expires = 0;
  {
    Watcher w(t2, sv2, 30, h, [] { return d(2026, 11, 15); }, 0);
    w.start();
    const auto t0 = std::chrono::steady_clock::now();
    w.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000));
  }
  CHECK_EQ(warns.load(), 0);
  CHECK_EQ(expires.load(), 0);
  // the start-up grace: a game that is already inside its last minutes gets no hook until the emulator has had time to exist
  {
    Tracker t3 = Tracker::parse("zfront-playtime 1\nday=2026-11-15\nused=899\n");
    Saver sv3(dir + "/p3");
    warns = 0;
    expires = 0;
    Watcher w(t3, sv3, 15, h, [] { return d(2026, 11, 15); }, 600);
    w.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK_EQ(warns.load(), 0);  // inside the grace: nothing, although 1 s was all that was left
    CHECK_EQ(expires.load(), 0);
    for (int i = 0; i < 100 && expires.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(warns.load(), 1);  // then it acts
    CHECK(expires.load() >= 1);
    // a stop during the grace wakes it at once and still counts the time
    Tracker t4;
    Saver sv4(dir + "/p4");
    Watcher w4(t4, sv4, 30, h, [] { return d(2026, 11, 15); }, 60 * 1000);
    w4.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto t0 = std::chrono::steady_clock::now();
    w4.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1000));
    CHECK(t4.used_ms() >= 100);
    CHECK(exists(dir + "/p4"));
    ::unlink((dir + "/p3").c_str());
    ::unlink((dir + "/p4").c_str());
  }
  ::unlink(path.c_str());
  ::unlink((dir + "/p2").c_str());
  ::rmdir(dir.c_str());
}

void test_who_can_change_it() {
  // the setting lives in GAME SETTINGS and the date in DATE & TIME: neither opens in Kid or Kiosk (a child cannot raise the limit
  // nor move the date forward to start a new day)
  for (uimode::Mode m : {uimode::Mode::Kid, uimode::Mode::Kiosk}) {
    CHECK(!uimode::screen_allowed(m, "menu.game"));
    CHECK(!uimode::screen_allowed(m, "menu.datetime"));
    CHECK(uimode::screen_allowed(m, "menu.system.info"));  // TIME LEFT TODAY is shown here
  }
  CHECK(uimode::screen_allowed(uimode::Mode::Full, "menu.game"));
}

}  // namespace

int main() {
  test_limit_setting();
  test_date();
  test_counting_and_roll();
  test_future_stored_day();
  test_games_and_tools();
  test_warning_and_expiry();
  test_limit_changes();
  test_text_and_corruption();
  test_saver_and_restart();
  test_session();
  test_watcher();
  test_who_can_change_it();
  return zt::done();
}
