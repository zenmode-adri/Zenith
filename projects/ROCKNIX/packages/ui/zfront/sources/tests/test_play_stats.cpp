// Play statistics: the play log (line format, bounding, short sessions), the arithmetic over a fake log and fake gamelists, the
// duration text and the empty state. The drawing is measured by test_layout.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "launch.hpp"
#include "playstats.hpp"
#include "registry.hpp"
#include "scene.hpp"
#include "util.hpp"

using namespace zf;
using namespace zf::playstats;
namespace fs = std::filesystem;

namespace {

std::string slurp(const std::string& p) {
  std::string s;
  read_file(p, s);
  return s;
}
void spit(const std::string& p, const std::string& s) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream(p, std::ios::binary) << s;
}
size_t count_lines(const std::string& s) {
  size_t n = 0;
  for (char c : s) n += c == '\n';
  return n;
}
GameTime gt(const char* name, const char* sys, uint64_t secs, uint32_t plays) {
  GameTime g;
  g.name = name, g.system = sys, g.secs = secs, g.plays = plays;
  return g;
}

}  // namespace

int main() {
  ::setenv("TZ", "UTC", 1);
  ::tzset();
  char tmpl[] = "/tmp/zf-test-playstats-XXXXXX";
  const std::string d = ::mkdtemp(tmpl);
  const time_t k2026 = 1767225600;  // 2026-01-01T00:00:00 UTC

  // ---- the line
  CHECK_EQ(format_line(k2026, 3725, "psp", "/roms/psp/God of War.iso"), std::string("2026-01-01T00:00:00\t3725\tpsp\t/roms/psp/God of War.iso\n"));
  CHECK_EQ(format_line(k2026 + 3661, 1, "a\tb", "x\ny\rz"), std::string("2026-01-01T01:01:01\t1\ta b\tx y z\n"));

  // ---- record: short sessions are not logged, the rest is appended
  {
    const std::string p = d + "/sub/dir/playlog.tsv";  // the folder does not exist yet
    CHECK(!record(p, k2026, 9, "snes", "/r/a.sfc"));
    CHECK(!fs::exists(p));
    CHECK(!record(p, k2026, 0, "snes", "/r/a.sfc"));
    CHECK(record(p, k2026, 10, "snes", "/r/a.sfc"));
    CHECK(record(p, k2026 + 100, 600, "gba", "/r/b b.gba"));
    CHECK_EQ(slurp(p), std::string("2026-01-01T00:00:00\t10\tsnes\t/r/a.sfc\n2026-01-01T00:01:40\t600\tgba\t/r/b b.gba\n"));
    // the path of the device can be replaced, and the start of a finished game is its end minus the time played
    ::setenv("ZF_PLAYLOG", (d + "/env.tsv").c_str(), 1);
    CHECK_EQ(log_path(), d + "/env.tsv");
    CHECK(record_ended("psp", "/r/g.iso", 90, k2026 + 90));
    CHECK(!record_ended("psp", "/r/g.iso", 5, k2026 + 200));
    CHECK_EQ(slurp(d + "/env.tsv"), std::string("2026-01-01T00:00:00\t90\tpsp\t/r/g.iso\n"));
    ::unsetenv("ZF_PLAYLOG");
    CHECK_EQ(log_path(), std::string("/storage/.config/zfront/playlog.tsv"));
  }

  // ---- bounded: past 512 KB the older half goes, whole lines only, the newest line is kept
  {
    const std::string p = d + "/big.tsv";
    const std::string rom = "/roms/psp/" + std::string(150, 'x') + ".iso";
    const int n = 4000;  // about 800 KB written in all
    for (int i = 0; i < n; ++i) CHECK(record(p, k2026 + i, 10000 + uint64_t(i), "psp", rom));
    const std::string t = slurp(p);
    CHECK(t.size() <= kMaxLogBytes + 400);
    CHECK(t.size() > kMaxLogBytes / 4);  // not emptied either
    const std::vector<LogEntry> e = parse_log(t);
    CHECK_EQ(e.size(), count_lines(t));  // every line whole
    CHECK(!e.empty() && e.back().secs == 10000 + uint64_t(n - 1));
    CHECK(e.size() < size_t(n) && e.front().secs > 10000);  // the oldest ones went
    for (size_t i = 1; i < e.size(); ++i) CHECK_EQ(e[i].secs, e[i - 1].secs + 1);  // in order, none lost in the middle
  }

  // ---- parse_log skips what is not a session
  {
    const std::string t = "2026-01-01T00:00:00\t60\tsnes\t/r/a.sfc\n"
                          "garbage\n"
                          "\n"
                          "2026-01-01T00:00:00\tabc\tsnes\t/r/a.sfc\n"
                          "2026-01-01T00:00:00\t-5\tsnes\t/r/a.sfc\n"
                          "2026-01-01T00:00:00\t+5\tsnes\t/r/a.sfc\n"
                          "not-a-date\t60\tsnes\t/r/a.sfc\n"
                          "2026-01-02T00:00:00\t30\tgba\t/r/with space.gba\n"
                          "2026-01-03T00:00:00\t30\tgba";  // cut by a power failure: no rom, no line break
    const std::vector<LogEntry> e = parse_log(t);
    CHECK_EQ(e.size(), size_t(2));
    CHECK(e.size() == 2 && e[0].start == k2026 && e[0].secs == 60 && e[0].system == "snes" && e[0].rom == "/r/a.sfc");
    CHECK(e.size() == 2 && e[1].start == k2026 + 86400 && e[1].rom == "/r/with space.gba");
    CHECK(parse_log("").empty());
  }

  // ---- compute: weeks from the log, totals / top / systems from the gamelists
  {
    const time_t now = k2026 + 100 * 86400;
    const time_t day = 86400;
    std::vector<LogEntry> log;
    auto add = [&](time_t age, uint64_t secs) {
      LogEntry le;
      le.start = now - age, le.secs = secs, le.system = "snes";
      log.push_back(le);
    };
    add(60, 100);                   // this week
    add(6 * day + 23 * 3600, 200);  // still this week
    add(7 * day + 60, 400);         // last week
    add(13 * day, 800);             // last week
    add(14 * day, 1600);            // 2 weeks ago (age exactly 14 d)
    add(29 * day, 3200);            // 4 weeks ago
    add(35 * day, 6400);            // fifth week back: out
    add(-3600, 50);                 // a clock that went back: counts as this week
    add(60, 9);                     // too short: ignored
    std::vector<GameTime> games = {gt("A", "SNES", 5000, 3), gt("B", "SNES", 7000, 1), gt("C", "PSP", 7000, 2), gt("D", "GBA", 0, 4)};
    const Stats s = compute(games, log, now);
    CHECK_EQ(s.week_secs[0], uint64_t(100 + 200 + 50));
    CHECK_EQ(s.week_secs[1], uint64_t(400 + 800));
    CHECK_EQ(s.week_secs[2], uint64_t(1600));
    CHECK_EQ(s.week_secs[3], uint64_t(0));
    CHECK_EQ(s.week_secs[4], uint64_t(3200));
    CHECK_EQ(s.total_secs, uint64_t(19000));
    CHECK_EQ(s.total_plays, uint64_t(10));
    CHECK_EQ(s.top.size(), size_t(3));  // D has no time: not listed
    CHECK(s.top.size() == 3 && s.top[0].name == "B" && s.top[1].name == "C" && s.top[2].name == "A");  // time, then name
    CHECK(s.top.size() == 3 && s.top[1].system == "PSP" && s.top[1].plays == 2);
    CHECK_EQ(s.systems.size(), size_t(2));
    CHECK(s.systems.size() == 2 && s.systems[0].first == "SNES" && s.systems[0].second == 12000 && s.systems[1].first == "PSP" && s.systems[1].second == 7000);
    CHECK(!s.empty());

    // twelve games: the ten with the most time
    std::vector<GameTime> many;
    for (int i = 0; i < 12; ++i) many.push_back(gt(("G" + std::to_string(i)).c_str(), "X", 1000 + uint64_t(i), 1));
    const Stats m = compute(many, {}, now);
    CHECK_EQ(m.top.size(), size_t(10));
    CHECK(m.top.size() == 10 && m.top[0].name == "G11" && m.top[9].name == "G2");

    // only the log: the weeks show although no gamelist has time yet
    const Stats w = compute({}, log, now);
    CHECK(!w.empty() && w.top.empty() && w.systems.empty());
  }

  // ---- empty state
  {
    const Stats e = compute({}, {}, k2026);
    CHECK(e.empty());
    CHECK(e.top.empty() && e.systems.empty() && e.total_secs == 0);
    CHECK(compute({gt("A", "X", 0, 0)}, {}, k2026).empty());
  }

  // ---- collect: games in memory, a system read from its gamelist for the call only, a system without gamelist
  {
    System loaded;
    loaded.name = "snes", loaded.fullname = "Super Nintendo", loaded.loaded = true;
    Game g1, g2, g3;
    g1.name = "Played", g1.path = "/r/snes/p.sfc", g1.meta.gametime = 120, g1.meta.playcount = 2;
    g2.name = "Never", g2.path = "/r/snes/n.sfc";
    g3.path = "/r/snes/no name.sfc", g3.meta.playcount = 1;  // played but no time (an old gamelist): still counted, named by the file
    loaded.games = {g1, g2, g3};

    const std::string rd = d + "/roms/gb";
    fs::create_directories(rd);
    spit(rd + "/a.gb", "x");
    spit(rd + "/b.gb", "x");
    spit(rd + "/gamelist.xml",
         "<?xml version=\"1.0\"?>\n<gameList>\n"
         "\t<game>\n\t\t<path>./a.gb</path>\n\t\t<name>Alpha</name>\n\t\t<playcount>3</playcount>\n\t\t<gametime>5400</gametime>\n\t</game>\n"
         "\t<game>\n\t\t<path>./b.gb</path>\n\t\t<name>Beta</name>\n\t</game>\n"
         "</gameList>\n");
    System lazy;
    lazy.name = "gb", lazy.fullname = "Game Boy", lazy.rom_dir = rd, lazy.gamelist = rd + "/gamelist.xml", lazy.exts = {".gb"};
    System none;  // no gamelist file: nothing to read
    none.name = "nes", none.rom_dir = d + "/roms/nes", none.gamelist = d + "/roms/nes/gamelist.xml", none.exts = {".nes"};
    std::vector<System> all = {loaded, lazy, none};
    const std::vector<GameTime> t = collect(all);
    CHECK(all[1].games.empty() && !all[1].loaded);  // the system was not left loaded
    CHECK_EQ(t.size(), size_t(3));
    if (t.size() == 3) {
      CHECK(t[0].name == "Played" && t[0].system == "Super Nintendo" && t[0].secs == 120 && t[0].plays == 2);
      CHECK(t[1].name == "no name" && t[1].secs == 0 && t[1].plays == 1);
      CHECK(t[2].name == "Alpha" && t[2].system == "Game Boy" && t[2].secs == 5400 && t[2].plays == 3);
    }
    const Stats s = compute(t, {}, k2026);
    CHECK_EQ(s.total_secs, uint64_t(5520));
    CHECK(s.systems.size() == 2 && s.systems[0].first == "Game Boy");
  }

  // ---- text
  CHECK_EQ(format_duration(0), std::string("0 min"));
  CHECK_EQ(format_duration(5), std::string("<1 min"));
  CHECK_EQ(format_duration(59), std::string("<1 min"));
  CHECK_EQ(format_duration(60), std::string("1 min"));
  CHECK_EQ(format_duration(45 * 60), std::string("45 min"));
  CHECK_EQ(format_duration(59 * 60 + 29), std::string("59 min"));
  CHECK_EQ(format_duration(3570), std::string("1 h 00 min"));  // 59.5 min rounds up
  CHECK_EQ(format_duration(3600), std::string("1 h 00 min"));
  CHECK_EQ(format_duration(12 * 3600 + 5 * 60), std::string("12 h 05 min"));
  CHECK_EQ(format_duration(123 * 3600 + 59 * 60), std::string("123 h 59 min"));

  // ---- the screen exists, opens with nothing to show and goes back; a finished game feeds the log through the launcher
  {
    ::setenv("ZF_PLAYLOG", (d + "/screen.tsv").c_str(), 1);
    Ctx cx;
    std::unique_ptr<Screen> s = Registry::instance().make("menu.game.stats", cx, "");
    CHECK(s != nullptr);
    if (s) {
      CHECK(s->handle(Action::Down, cx).kind == Transition::None);  // nothing to scroll
      CHECK(s->handle(Action::B, cx).kind == Transition::Pop);
    }
    launch::Outcome o;
    o.after.count_play = true;
    o.after.add_gametime = 75;
    launch::apply_after_path(cx, "gbc", "/roms/gbc/y.gb", o);  // the game is not in the lists: the log still gets the session
    const std::vector<LogEntry> e = parse_log(slurp(d + "/screen.tsv"));
    CHECK_EQ(e.size(), size_t(1));
    CHECK(e.size() == 1 && e[0].secs == 75 && e[0].system == "gbc" && e[0].rom == "/roms/gbc/y.gb");
    o.after.add_gametime = 0;  // failed or too short: counted nowhere
    launch::apply_after_path(cx, "gbc", "/roms/gbc/y.gb", o);
    CHECK_EQ(parse_log(slurp(d + "/screen.tsv")).size(), size_t(1));
    ::unsetenv("ZF_PLAYLOG");
  }

  fs::remove_all(d);
  return zt::done();
}
