// Smart collections: membership rules, ordering, kid / hidden filtering, settings keys, empty ones left out, unloaded systems.
#include <sys/stat.h>

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "check.hpp"
#include "collections.hpp"
#include "smartcol.hpp"
#include "util.hpp"

using namespace zf;
namespace sc = zf::smartcol;
namespace fs = std::filesystem;

namespace {

constexpr time_t kDay = 24 * 3600;
const time_t k2026 = 1767225600;  // 2026-01-01T00:00:00 UTC

Game game(const char* name, const char* path, uint32_t plays, uint32_t secs, uint64_t lastplayed, bool hidden = false, bool kid = false) {
  Game g;
  g.name = name;
  g.path = path;
  g.meta.playcount = plays;
  g.meta.gametime = secs;
  g.meta.lastplayed = lastplayed;
  g.meta.hidden = hidden;
  g.meta.kid = kid;
  return g;
}

System loaded_system(const char* name, std::vector<Game> games) {
  System s;
  s.name = name;
  s.fullname = name;
  s.games = std::move(games);
  s.loaded = true;
  s.count = s.games.size();
  return s;
}

std::vector<std::string> names(const std::vector<sc::Member>& v) {
  std::vector<std::string> o;
  for (const auto& m : v) o.push_back(m.name);
  return o;
}

void spit(const std::string& p, const std::string& s) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream(p, std::ios::binary) << s;
}

}  // namespace

int main() {
  ::setenv("TZ", "UTC", 1);
  ::tzset();
  const time_t now = k2026 + 100 * kDay;

  // ---- fake library: 2 loaded systems
  std::vector<System> systems;
  systems.push_back(loaded_system("snes", {
      game("Alpha", "/r/snes/alpha.sfc", 0, 0, 0, false, true),                  // never played, kid
      game("beta", "/r/snes/beta.sfc", 0, 0, 0),                                 // never played, not kid
      game("Hidden", "/r/snes/hidden.sfc", 0, 0, 0, true, true),                 // never played but hidden
      game("Gamma", "/r/snes/gamma.sfc", 2, 1800, 20251201120000ULL, false, true),   // unfinished (old)
      game("Delta", "/r/snes/delta.sfc", 3, 3600, 20251215000000ULL, false, false),  // unfinished, newer than Gamma
  }));
  systems.push_back(loaded_system("gba", {
      game("Epsilon", "/r/gba/eps.gba", 4, 600, 20251101000000ULL),         // 4 plays: not unfinished
      game("Zeta", "/r/gba/zeta.gba", 1, 7200, 20251101000000ULL),          // 2 h total: not unfinished
      game("Eta", "/r/gba/eta.gba", 1, 60, 20260405000000ULL),              // played 4 days ago... now = 2026-04-11: 6 d: too recent
      game("Theta", "/r/gba/theta.gba", 1, 60, 20260403000000ULL, false, true),  // 8 days ago: unfinished
      game("HiddenU", "/r/gba/hu.gba", 1, 60, 20250101000000ULL, true),     // old but hidden
  }));

  // ---- file times (injected): Alpha/beta/Gamma recent, Delta 31 days old, the rest unknown (0)
  std::map<std::string, time_t> ft = {
      {"/r/snes/alpha.sfc", now - 2 * kDay}, {"/r/snes/beta.sfc", now - 1 * kDay},  {"/r/snes/gamma.sfc", now - 30 * kDay},
      {"/r/snes/delta.sfc", now - 31 * kDay}, {"/r/gba/eps.gba", now - 1 * kDay},   {"/r/snes/hidden.sfc", now - 1 * kDay},
  };
  sc::Options o;
  o.now = now;
  o.file_time = [&](const std::string& p) { return ft.count(p) ? ft[p] : time_t(0); };

  // ---- settings keys
  {
    CHECK_EQ(std::string(sc::kSettingsKey), std::string("ZfCollectionSystemsAuto"));
    CHECK(std::string(sc::kSettingsKey) != "CollectionSystemsAuto");
    EsSettings es = EsSettings::from_text("<config><string name=\"ZfCollectionSystemsAuto\" value=\"weekly, unfinished,,\"/></config>");
    auto on = sc::enabled(es);  // trimmed, unlike ES's own list
    CHECK_EQ(on.size(), size_t(2));
    CHECK_EQ(on[0], std::string("weekly"));
    CHECK_EQ(on[1], std::string("unfinished"));
    CHECK(sc::enabled(EsSettings::from_text("<config/>")).empty());
    // the new names do not clash with ES's catalogue and are reserved for custom collections
    for (const sc::Decl& d : sc::decls())
      for (const collections::AutoDecl& a : collections::auto_decls()) CHECK(d.name != a.name && d.theme_folder != a.theme_folder);
    std::vector<std::string> res = collections::reserved_names();
    for (const sc::Decl& d : sc::decls()) {
      CHECK(std::find(res.begin(), res.end(), d.name) != res.end());
      CHECK(std::find(res.begin(), res.end(), d.theme_folder) != res.end());
    }
    CHECK_EQ(sc::decls().size(), size_t(3));
  }

  // ---- never played: playcount 0 and no lastplayed, hidden out, name order
  {
    auto m = sc::members("neverplayed", systems, {}, o);
    CHECK_EQ(names(m).size(), size_t(2));
    CHECK_EQ(m[0].name, std::string("Alpha"));
    CHECK_EQ(m[1].name, std::string("beta"));  // case-insensitive order
    CHECK_EQ(m[0].sys, size_t(0));
    CHECK_EQ(m[0].game, size_t(0));
    // a game with lastplayed but playcount 0 is not "never played"
    std::vector<System> s2;
    s2.push_back(loaded_system("x", {game("A", "/r/a", 0, 0, 20250101000000ULL), game("B", "/r/b", 0, 0, 0)}));
    CHECK_EQ(names(sc::members("neverplayed", s2, {}, o)).size(), size_t(1));
    // show_hidden brings the hidden one in (Full mode only)
    sc::Options oh = o;
    oh.show_hidden = true;
    CHECK_EQ(sc::members("neverplayed", systems, {}, oh).size(), size_t(3));
    oh.mode = uimode::Mode::Kiosk;
    CHECK_EQ(sc::members("neverplayed", systems, {}, oh).size(), size_t(2));
  }

  // ---- kid mode: only kid games
  {
    sc::Options ok = o;
    ok.mode = uimode::Mode::Kid;
    auto m = sc::members("neverplayed", systems, {}, ok);
    CHECK_EQ(m.size(), size_t(1));
    CHECK_EQ(m[0].name, std::string("Alpha"));
    ok.show_hidden = true;  // no effect in Kid mode
    CHECK_EQ(sc::members("neverplayed", systems, {}, ok).size(), size_t(1));
    auto u = sc::members("unfinished", systems, {}, ok);
    CHECK_EQ(u.size(), size_t(2));  // Gamma, Theta (Delta is not a kid game)
  }

  // ---- recently added: 30-day window, newest first, hidden out
  {
    auto m = sc::members("recentlyadded", systems, {}, o);
    CHECK_EQ(m.size(), size_t(4));  // alpha(2d) beta(1d) gamma(30d) eps(1d); delta 31 d and hidden out
    CHECK_EQ(m[0].name, std::string("beta"));    // 1 d, ties with Epsilon: name order (BETA < EPSILON)
    CHECK_EQ(m[1].name, std::string("Epsilon"));
    CHECK_EQ(m[2].name, std::string("Alpha"));
    CHECK_EQ(m[3].name, std::string("Gamma"));
    CHECK_EQ(m[3].key, now - 30 * kDay);
    ft["/r/snes/gamma.sfc"] = now - 30 * kDay - 1;  // just past the window
    CHECK_EQ(sc::members("recentlyadded", systems, {}, o).size(), size_t(3));
    ft["/r/snes/gamma.sfc"] = now + 5 * kDay;  // clock set back: counts as new
    CHECK_EQ(sc::members("recentlyadded", systems, {}, o).size(), size_t(4));
    // default clock source: a file made just now
    const std::string d = fs::temp_directory_path().string() + "/zf-smartcol-" + std::to_string(::getpid());
    spit(d + "/new.gb", "x");
    std::vector<System> s2;
    s2.push_back(loaded_system("gb", {game("New", (d + "/new.gb").c_str(), 0, 0, 0), game("Missing", (d + "/none.gb").c_str(), 0, 0, 0)}));
    sc::Options od;
    od.now = ::time(nullptr);
    auto r = sc::members("recentlyadded", s2, {}, od);
    CHECK_EQ(r.size(), size_t(1));
    CHECK_EQ(r[0].name, std::string("New"));
    fs::remove_all(d);
  }

  // ---- unfinished: 1-3 plays, < 2 h, last played > 7 days ago, newest first
  {
    auto m = sc::members("unfinished", systems, {}, o);
    CHECK_EQ(m.size(), size_t(3));
    CHECK_EQ(m[0].name, std::string("Theta"));  // 8 days ago
    CHECK_EQ(m[1].name, std::string("Delta"));
    CHECK_EQ(m[2].name, std::string("Gamma"));
    CHECK(m[0].key > m[1].key && m[1].key > m[2].key);
    CHECK_EQ(sc::lastplayed_time(20260101000000ULL), k2026);
    CHECK_EQ(sc::lastplayed_time(0), time_t(0));
    CHECK_EQ(sc::lastplayed_time(20261340000000ULL), time_t(0));  // month 13
    // boundaries: exactly 7 days is not "more than 7 days"; 2 h exactly is not "less than 2 h"; 3 plays in, 4 plays out
    std::vector<System> s2;
    s2.push_back(loaded_system("x", {
        game("Edge7", "/r/e7", 1, 10, 20260404000000ULL),            // exactly 7 d before 2026-04-11 00:00
        game("Edge7b", "/r/e7b", 1, 10, 20260403235959ULL),          // 7 d and 1 s
        game("Edge2h", "/r/e2h", 1, 7199, 20250101000000ULL),
        game("Edge2h2", "/r/e2h2", 1, 7200, 20250101000000ULL),
        game("Plays3", "/r/p3", 3, 10, 20250101000000ULL),
        game("Plays4", "/r/p4", 4, 10, 20250101000000ULL),
        game("Plays0", "/r/p0", 0, 10, 20250101000000ULL),
        game("NoDate", "/r/nd", 1, 10, 0),
    }));
    sc::Options o2 = o;
    o2.now = k2026 + 100 * kDay;
    auto e = sc::members("unfinished", s2, {}, o2);
    std::vector<std::string> n = names(e);
    CHECK_EQ(n.size(), size_t(3));
    CHECK_EQ(n[0], std::string("Edge7b"));
    CHECK(std::find(n.begin(), n.end(), "Edge7") == n.end());
    CHECK(std::find(n.begin(), n.end(), "Edge2h") != n.end());
    CHECK(std::find(n.begin(), n.end(), "Edge2h2") == n.end());
    CHECK(std::find(n.begin(), n.end(), "Plays3") != n.end());
    CHECK(std::find(n.begin(), n.end(), "Plays4") == n.end());
    CHECK(std::find(n.begin(), n.end(), "NoDate") == n.end());
  }

  // ---- most played this week: from the play log, same 7-day window as the statistics screen
  {
    auto ent = [&](time_t start, uint64_t secs, const char* rom) {
      playstats::LogEntry e;
      e.start = start;
      e.secs = secs;
      e.system = "snes";
      e.rom = rom;
      return e;
    };
    std::vector<playstats::LogEntry> log = {
        ent(now - 1 * kDay, 600, "/r/snes/alpha.sfc"),
        ent(now - 2 * kDay, 900, "/r/snes/alpha.sfc"),   // alpha 1500 s
        ent(now - 3 * kDay, 1500, "/r/gba/eps.gba"),     // ties with alpha: name order
        ent(now - 6 * kDay, 100, "/r/snes/beta.sfc"),
        ent(now - 7 * kDay, 5000, "/r/snes/gamma.sfc"),  // 7 d old = last week (week index 1): out
        ent(now - 1 * kDay, 5, "/r/gba/zeta.gba"),       // shorter than kMinSecs: ignored
        ent(now - 1 * kDay, 800, "/r/snes/hidden.sfc"),  // hidden: out
        ent(now - 1 * kDay, 800, "/r/gone/removed.sfc"), // no longer in the library
        ent(now + 3600, 50, "/r/gba/theta.gba"),         // clock went back: counts as now
    };
    auto m = sc::members("weekly", systems, log, o);
    CHECK_EQ(m.size(), size_t(4));
    CHECK_EQ(m[0].name, std::string("Alpha"));
    CHECK_EQ(m[0].key, time_t(1500));
    CHECK_EQ(m[1].name, std::string("Epsilon"));
    CHECK_EQ(m[2].name, std::string("beta"));
    CHECK_EQ(m[3].name, std::string("Theta"));
    // the same numbers the statistics screen uses for "this week"
    playstats::Stats st = playstats::compute({}, log, now);
    CHECK_EQ(st.week_secs[0], uint64_t(600 + 900 + 1500 + 100 + 800 + 800 + 50));
    sc::Options ok = o;
    ok.mode = uimode::Mode::Kid;
    auto k = sc::members("weekly", systems, log, ok);
    CHECK_EQ(k.size(), size_t(2));  // Alpha and Theta are kid games
    CHECK_EQ(k[0].name, std::string("Alpha"));
    CHECK(sc::members("weekly", systems, {}, o).empty());
  }

  // ---- build: only enabled, non-empty collections; neverplayed lives in ES's key, the others in zfront's
  {
    std::vector<playstats::LogEntry> none;
    EsSettings off = EsSettings::from_text("<config/>");
    CHECK(sc::build(systems, off, none, o).empty());  // off by default

    EsSettings es = EsSettings::from_text(
        "<config><string name=\"CollectionSystemsAuto\" value=\"favorites,neverplayed\"/>"
        "<string name=\"ZfCollectionSystemsAuto\" value=\"unfinished,weekly,recentlyadded\"/></config>");
    auto b = sc::build(systems, es, none, o);  // weekly is empty (no log): not shown
    CHECK_EQ(b.size(), size_t(3));
    CHECK_EQ(b[0].name, std::string("neverplayed"));
    CHECK_EQ(b[0].theme_folder, std::string("auto-neverplayed"));
    CHECK_EQ(b[1].name, std::string("recentlyadded"));
    CHECK_EQ(b[1].theme_folder, std::string("auto-recentlyadded"));
    CHECK_EQ(b[2].name, std::string("unfinished"));
    CHECK(!b[1].label.empty());
    // LoadEmptySystems keeps the empty one
    auto all = sc::build(systems, es, none, o, true);
    CHECK_EQ(all.size(), size_t(4));
    CHECK_EQ(all[3].name, std::string("weekly"));
    CHECK(all[3].games.empty());
    // ES's key alone does not enable the new ones, nor zfront's key neverplayed
    EsSettings only_es = EsSettings::from_text("<config><string name=\"CollectionSystemsAuto\" value=\"unfinished,recentlyadded\"/></config>");
    CHECK(sc::build(systems, only_es, none, o).empty());
    EsSettings only_zf = EsSettings::from_text("<config><string name=\"ZfCollectionSystemsAuto\" value=\"neverplayed\"/></config>");
    CHECK(sc::build(systems, only_zf, none, o).empty());
    // no systems at all: nothing, even when enabled
    CHECK(sc::build({}, es, none, o).empty());
    CHECK(sc::members("bogus", systems, none, o).empty());
  }

  // ---- a system that is not loaded (device: load_systems_lazy) is read from its gamelist, with the same filters
  {
    char tmpl[] = "/tmp/zf-test-smartcol-XXXXXX";
    const std::string d = ::mkdtemp(tmpl);
    spit(d + "/roms/a.gb", "x");
    spit(d + "/roms/b.gb", "x");
    spit(d + "/roms/c.gb", "x");
    spit(d + "/roms/gamelist.xml",
         "<gameList>"
         "<game><path>./a.gb</path><name>Aaa</name><kidgame>true</kidgame></game>"
         "<game><path>./b.gb</path><name>Bbb</name><hidden>true</hidden></game>"
         "<game><path>./c.gb</path><name>Ccc</name><playcount>1</playcount><lastplayed>20250101T000000</lastplayed></game>"
         "</gameList>");
    System s;
    s.name = "gb";
    s.rom_dir = d + "/roms";
    s.gamelist = d + "/roms/gamelist.xml";
    s.exts = {".gb"};
    std::vector<System> v = {s};
    CHECK(!v[0].loaded);
    auto m = sc::members("neverplayed", v, {}, o);
    CHECK_EQ(names(m).size(), size_t(1));  // Bbb hidden, Ccc played
    CHECK_EQ(m[0].name, std::string("Aaa"));
    CHECK_EQ(m[0].game, sc::kNoGame);
    CHECK_EQ(m[0].path, d + "/roms/a.gb");
    CHECK(v[0].games.empty());  // the system itself is untouched
    auto u = sc::members("unfinished", v, {}, o);
    CHECK_EQ(u.size(), size_t(1));
    CHECK_EQ(u[0].name, std::string("Ccc"));
    sc::Options ok = o;
    ok.mode = uimode::Mode::Kid;
    CHECK(sc::members("unfinished", v, {}, ok).empty());
    CHECK_EQ(sc::members("neverplayed", v, {}, ok).size(), size_t(1));
    fs::remove_all(d);
  }

  return zt::done();
}
