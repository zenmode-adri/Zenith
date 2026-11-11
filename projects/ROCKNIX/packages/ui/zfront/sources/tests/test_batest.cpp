// Battery estimate per game (batest.cpp): the session recorded in the play log (charger and short sessions ignored, the old line format
// still read), the rate from the last sessions (recency weighting, fallbacks), the text, the top drainers, and the BATTERY row of the game
// options (appears only with a battery, says "NOT ENOUGH DATA YET" without sessions). Temp dirs only.
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app.hpp"
#include "batest.hpp"
#include "check.hpp"
#include "i18n.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "playstats.hpp"
#include "power.hpp"
#include "registry.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;
using playstats::LogEntry;

namespace {

void spit(const std::string& p, const std::string& s) {
  const std::string d = p.substr(0, p.find_last_of('/'));
  if (std::system(("mkdir -p '" + d + "'").c_str())) {
  }
  FILE* f = std::fopen(p.c_str(), "wb");
  CHECK(f != nullptr);
  if (f) {
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
  }
}

LogEntry ent(time_t start, uint64_t secs, const char* sys, const char* rom, int drop10) {
  LogEntry e;
  e.start = start, e.secs = secs, e.system = sys, e.rom = rom, e.drop_pct10 = drop10;
  return e;
}

power::Now now(int pct, long uah, long full, bool charging = false) {
  power::Now n;
  n.have = true, n.percent = pct, n.charge_uah = uah, n.full_uah = full, n.charging = charging;
  return n;
}

bool near(double a, double b, double eps = 0.01) { return std::fabs(a - b) < eps; }

Menu& as_menu(std::unique_ptr<Screen>& s) { return *static_cast<Menu*>(s.get()); }
int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

}  // namespace

int main() {
  ::setenv("TZ", "UTC", 1);
  ::tzset();
  char tmpl[] = "/tmp/zf-test-batest-XXXXXX";
  const std::string d = ::mkdtemp(tmpl);
  const time_t t0 = 1767225600;

  // ---- the log line: old lines still read, the new column is optional
  {
    CHECK_EQ(playstats::format_line(t0, 3600, "gb", "/r/a.gb"), std::string("2026-01-01T00:00:00\t3600\tgb\t/r/a.gb\n"));  // unchanged without a drop
    CHECK_EQ(playstats::format_line(t0, 3600, "gb", "/r/a.gb", 172), std::string("2026-01-01T00:00:00\t3600\tgb\t/r/a.gb\t172\n"));
    CHECK_EQ(playstats::format_line(t0, 3600, "gb", "/r/a.gb", 0), std::string("2026-01-01T00:00:00\t3600\tgb\t/r/a.gb\t0\n"));
    const std::string text = "2026-01-01T00:00:00\t3600\tgb\t/r/old.gb\n"                // old format
                             "2026-01-01T01:00:00\t1800\tgb\t/r/new.gb\t85\n"            // with the column
                             "2026-01-01T02:00:00\t600\tgb\t/r/x.gb\t12\textra\tcolumns\n"  // more columns after it: ignored
                             "2026-01-01T03:00:00\t600\tgb\t/r/y.gb\tabc\n"              // not a number: the session stays, without a drop
                             "2026-01-01T04:00:00\t600\tgb\t/r/z.gb\t5000\n"             // impossible: no drop
                             "garbage\n";
    const std::vector<LogEntry> v = playstats::parse_log(text);
    CHECK_EQ(v.size(), size_t(5));
    if (v.size() == 5) {
      CHECK_EQ(v[0].rom, std::string("/r/old.gb"));
      CHECK_EQ(v[0].drop_pct10, -1);
      CHECK_EQ(v[1].rom, std::string("/r/new.gb"));
      CHECK_EQ(v[1].drop_pct10, 85);
      CHECK_EQ(v[2].rom, std::string("/r/x.gb"));
      CHECK_EQ(v[2].drop_pct10, 12);
      CHECK_EQ(v[3].rom, std::string("/r/y.gb"));
      CHECK_EQ(v[3].drop_pct10, -1);
      CHECK_EQ(v[4].drop_pct10, -1);
    }
    // the statistics read the new lines like the old ones
    const playstats::Stats st = playstats::compute({}, v, t0 + 5 * 3600);
    CHECK_EQ(st.week_secs[0], uint64_t(3600 + 1800 + 600 + 600 + 600));
    // record() writes the column
    const std::string p = d + "/playlog.tsv";
    CHECK(playstats::record(p, t0, 900, "gb", "/r/a.gb", 40));
    CHECK(playstats::record(p, t0 + 1000, 900, "gb", "/r/b.gb"));
    const std::vector<LogEntry> w = playstats::parse_log([&] {
      std::string s;
      read_file(p, s);
      return s;
    }());
    CHECK_EQ(w.size(), size_t(2));
    if (w.size() == 2) {
      CHECK_EQ(w[0].drop_pct10, 40);
      CHECK_EQ(w[1].drop_pct10, -1);
    }
  }

  // ---- the session between two readings
  {
    using power::make_session;
    // from the charge counter: 3000000 -> 2850000 uAh of 3000000 = 5.0 %
    power::Session s = make_session(now(80, 3000000, 3000000), now(75, 2850000, 3000000), 3600);
    CHECK(s.valid);
    CHECK_EQ(s.drop_pct10, 50);
    s = make_session(now(80, 0, 0), now(77, 0, 0), 3600);  // no counter: percentages
    CHECK(s.valid);
    CHECK_EQ(s.drop_pct10, 30);
    CHECK(!make_session(now(80, 3000000, 3000000, true), now(75, 2850000, 3000000), 3600).valid);   // on the charger at the start
    CHECK(!make_session(now(80, 3000000, 3000000), now(75, 2850000, 3000000, true), 3600).valid);   // ... or at the end
    CHECK(!make_session(now(80, 3000000, 3000000), now(75, 2850000, 3000000), 299).valid);          // too short
    CHECK(make_session(now(80, 3000000, 3000000), now(75, 2850000, 3000000), 300).valid);
    CHECK(!make_session(now(70, 2400000, 3000000), now(75, 2850000, 3000000), 3600).valid);          // it went up
    CHECK(!make_session(now(80, 2400000, 3000000), now(80, 2400000, 3000000), 3600).valid);          // no drop to tell
    power::Now none;
    CHECK(!make_session(none, now(75, 2850000, 3000000), 3600).valid);                               // no battery

    // the real path: game_started / game_ended read the driver, take_session hands the result over once
    const std::string root = d + "/bat";
    const std::string b = root + "/sys/class/power_supply/battery/";
    ::setenv("ZF_BATTERY_ROOT", root.c_str(), 1);
    ::setenv("ZF_BATTERY_LOG", (root + "/battery.log").c_str(), 1);
    spit(b + "capacity", "80\n");
    spit(b + "status", "Discharging\n");
    spit(b + "charge_now", "2400000\n");
    spit(b + "charge_full", "3000000\n");
    power::game_started("Some Game");
    spit(b + "capacity", "72\n");
    spit(b + "charge_now", "2160000\n");
    power::game_ended();
    s = power::take_session(3600);
    CHECK(s.valid);
    CHECK_EQ(s.drop_pct10, 80);
    CHECK(!power::take_session(3600).valid);  // once
    power::game_started("Short");
    power::game_ended();
    CHECK(!power::take_session(3600).valid);  // no drop
    power::game_started("Plugged");
    spit(b + "status", "Charging\n");
    power::game_ended();
    CHECK(!power::take_session(3600).valid);
    spit(b + "status", "Discharging\n");
  }

  // ---- the rate
  {
    using namespace batest;
    const std::string R = "/r/a.gb";
    // two sessions of one hour: 20 %/h (newest) and 10 %/h; the newest weighs 1, the older 0.6 -> (20 + 6) / 1.6 = 16.25
    std::vector<LogEntry> log = {ent(t0, 3600, "gb", R.c_str(), 100), ent(t0 + 86400, 3600, "gb", R.c_str(), 200)};  // the file order is not the time order
    Rate r = estimate(log, "gb", R);
    CHECK(r.valid());
    CHECK(r.source == Rate::Game);
    CHECK_EQ(r.sessions, size_t(2));
    CHECK(near(r.pct_per_hour, 16.25));
    // the same sessions the other way round count the other way
    log = {ent(t0, 3600, "gb", R.c_str(), 200), ent(t0 + 86400, 3600, "gb", R.c_str(), 100)};
    r = estimate(log, "gb", R);
    CHECK(near(r.pct_per_hour, (10 + 0.6 * 20) / 1.6));
    // weighted by length too: a 2 h session of 20 % (10 %/h) and a 30 min one of 10 % (20 %/h), newest the short one
    log = {ent(t0, 7200, "gb", R.c_str(), 200), ent(t0 + 10, 1800, "gb", R.c_str(), 100)};
    r = estimate(log, "gb", R);
    CHECK(near(r.pct_per_hour, (10.0 + 0.6 * 20.0) / (0.5 + 0.6 * 2.0)));
    // ignored: the charger (-1), under five minutes, no drop, other games
    log = {ent(t0, 3600, "gb", R.c_str(), 100), ent(t0 + 100, 3600, "gb", R.c_str(), -1), ent(t0 + 200, 299, "gb", R.c_str(), 500), ent(t0 + 300, 3600, "gb", R.c_str(), 0),
           ent(t0 + 400, 3600, "gb", "/r/other.gb", 900)};
    r = estimate(log, "gb", R);
    CHECK(r.source == Rate::Game);
    CHECK_EQ(r.sessions, size_t(1));
    CHECK(near(r.pct_per_hour, 10.0));
    CHECK(usable(ent(t0, 300, "gb", R.c_str(), 1)));
    CHECK(!usable(ent(t0, 299, "gb", R.c_str(), 1)));
    CHECK(!usable(ent(t0, 3600, "gb", R.c_str(), 0)));
    CHECK(!usable(ent(t0, 3600, "gb", R.c_str(), -1)));
    // only the newest kGameN count: eight old 100 %/h sessions and five new 10 %/h ones
    log.clear();
    for (int i = 0; i < 8; ++i) log.push_back(ent(t0 + i, 3600, "gb", R.c_str(), 1000));
    for (int i = 0; i < 5; ++i) log.push_back(ent(t0 + 1000 + i, 3600, "gb", R.c_str(), 100));
    r = estimate(log, "gb", R);
    CHECK_EQ(r.sessions, kGameN);
    CHECK(near(r.pct_per_hour, 10.0));
    // fallback: no session of this game -> its system, then every game
    log = {ent(t0, 3600, "gb", "/r/b.gb", 100), ent(t0 + 1, 3600, "snes", "/r/c.sfc", 400)};
    r = estimate(log, "gb", R);
    CHECK(r.source == Rate::System);
    CHECK(near(r.pct_per_hour, 10.0));
    r = estimate(log, "psp", "/r/g.iso");
    CHECK(r.source == Rate::Global);
    CHECK_EQ(r.sessions, size_t(2));
    CHECK(near(r.pct_per_hour, (40.0 + 0.6 * 10.0) / 1.6));  // the snes one is the newer
    CHECK(!estimate({}, "gb", R).valid());
    CHECK(!estimate({ent(t0, 100, "gb", R.c_str(), 100), ent(t0, 3600, "gb", R.c_str(), -1)}, "gb", R).valid());  // nothing usable
    // the game's own sessions win over the system's even when there is only one
    log = {ent(t0, 3600, "gb", R.c_str(), 300), ent(t0 + 1, 3600, "gb", "/r/b.gb", 100), ent(t0 + 2, 3600, "gb", "/r/b.gb", 100)};
    r = estimate(log, "gb", R);
    CHECK(r.source == Rate::Game);
    CHECK(near(r.pct_per_hour, 30.0));

    // hours and text
    Rate q;
    q.source = Rate::Game, q.pct_per_hour = 16.25;
    CHECK(near(hours_left(q, 50), 50.0 / 16.25));
    CHECK(hours_left(Rate(), 50) < 0);
    CHECK(near(hours_left(q, 0), 0.0));
    CHECK_EQ(format_left(3.0 + 20.0 / 60.0), std::string("~3 h 20 min"));
    CHECK_EQ(format_left(3.0 + 22.0 / 60.0), std::string("~3 h 20 min"));  // five minutes at a time
    CHECK_EQ(format_left(3.0 + 24.0 / 60.0), std::string("~3 h 25 min"));
    CHECK_EQ(format_left(0.5), std::string("~30 min"));
    CHECK_EQ(format_left(2.0 / 60.0), std::string("~2 min"));
    CHECK_EQ(format_left(0.0), std::string("<1 min"));
    CHECK_EQ(format_left(100.0), std::string("> 48 h"));

    // the drainers: one row per game, fastest first, only usable sessions
    log = {ent(t0, 3600, "gb", "/r/slow.gb", 100), ent(t0 + 1, 3600, "gb", "/r/fast.gb", 300), ent(t0 + 2, 3600, "psp", "/r/mid.iso", 200),
           ent(t0 + 3, 3600, "gb", "/r/fast.gb", 300), ent(t0 + 4, 3600, "gb", "/r/never.gb", -1), ent(t0 + 5, 120, "gb", "/r/short.gb", 900)};
    std::vector<Drainer> top = top_drainers(log, 5);
    CHECK_EQ(top.size(), size_t(3));
    if (top.size() == 3) {
      CHECK_EQ(top[0].rom, std::string("/r/fast.gb"));
      CHECK_EQ(top[0].sessions, size_t(2));
      CHECK(near(top[0].pct_per_hour, 30.0));
      CHECK_EQ(top[1].rom, std::string("/r/mid.iso"));
      CHECK_EQ(top[2].rom, std::string("/r/slow.gb"));
    }
    CHECK_EQ(top_drainers(log, 2).size(), size_t(2));
    CHECK(top_drainers({}, 5).empty());
  }

  // ---- the BATTERY row of the game options
  {
    const std::string dir = d + "/menu";
    spit(dir + "/g0.gb", "rom0");
    spit(dir + "/g1.gb", "rom1");
    spit(dir + "/gamelist.xml",
         "<?xml version=\"1.0\"?>\n<gameList>\n\t<game>\n\t\t<path>./g0.gb</path>\n\t\t<name>G0</name>\n\t</game>\n\t<game>\n\t\t<path>./g1.gb</path>\n\t\t<name>G1</name>\n\t</game>\n</gameList>\n");
    spit(dir + "/root/storage/.config/emulationstation/x", "");
    Ctx cx;
    System s;
    s.name = "gb", s.fullname = "Game Boy", s.rom_dir = dir, s.gamelist = dir + "/gamelist.xml", s.exts = {".gb"};
    cx.systems.push_back(std::move(s));
    cx.open_system(0);
    std::string err;
    CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
    cx.w = cx.h = 640;
    mg::env().root = dir + "/root";
    mg::reset_caches();
    const std::string rom0 = dir + "/g0.gb", rom1 = dir + "/g1.gb";
    const std::string bat = d + "/bat2", bd = bat + "/sys/class/power_supply/battery/";
    const std::string plog = d + "/menu-playlog.tsv";
    ::setenv("ZF_PLAYLOG", plog.c_str(), 1);
    auto row = [&](const std::string& rom, std::string* value, std::string* hint) -> int {
      auto sc = Registry::instance().make("menu.game_options", cx, mg::game_arg("gb", rom));
      CHECK(sc != nullptr);
      Menu& m = as_menu(sc);
      m.rebuild(cx);
      const int i = find(m, tr("BATTERY"));
      if (i >= 0 && value) *value = m.entry(size_t(i)).value_text;
      if (i >= 0 && hint) *hint = m.entry(size_t(i)).hint_text;
      return i;
    };
    std::string v, h;
    // no battery: no row
    ::setenv("ZF_BATTERY_ROOT", (d + "/nobat").c_str(), 1);
    CHECK(row(rom0, &v, &h) < 0);
    // a battery, no log: the honest answer
    ::setenv("ZF_BATTERY_ROOT", bat.c_str(), 1);
    spit(bd + "capacity", "50\n");
    spit(bd + "status", "Discharging\n");
    CHECK(row(rom0, &v, &h) >= 0);
    CHECK_EQ(v, tr("NOT ENOUGH DATA YET"));
    // one measured hour of this game at 20 %/h with 50 % left: two and a half hours
    spit(plog, playstats::format_line(t0, 3600, "gb", rom0, 200));
    CHECK(row(rom0, &v, &h) >= 0);
    CHECK_EQ(v, std::string("~2 h 30 min"));
    // the other game falls back to the system average (the same rate here)
    const std::string h_game = h;
    CHECK(row(rom1, &v, &h) >= 0);
    CHECK_EQ(v, std::string("~2 h 30 min"));
    CHECK(h != h_game);  // and says so
    // the charge changes the answer; charging still shows it (the charge in the battery is what counts)
    spit(bd + "capacity", "100\n");
    CHECK(row(rom0, &v, &h) >= 0);
    CHECK_EQ(v, std::string("~5 h 00 min"));
    spit(bd + "status", "Charging\n");
    CHECK(row(rom0, &v, &h) >= 0);
    // an old log without the column: not enough data
    spit(plog, playstats::format_line(t0, 3600, "gb", rom0));
    CHECK(row(rom0, &v, &h) >= 0);
    CHECK_EQ(v, tr("NOT ENOUGH DATA YET"));
    mg::env().root.clear();
    mg::reset_caches();
  }

  if (std::system(("rm -rf '" + d + "'").c_str())) {
  }
  return 0;
}
