// scan module tests + benchmarks.
//   ctest                                   -> unit tests on temp trees
//   test_scan --bench-count|--bench-has|--bench-list [compact|vector]|--bench-load [eager|index|lazy]|--bench-worker
//   (benchmarks read the fake library in $ZLIB, default /dev/shm/zlib; run each in its own process for clean RSS)
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#include "check.hpp"
#include "scan.hpp"
#include "systems.hpp"
#include "util.hpp"

using namespace zf;

namespace {

std::string make_tree(const char* tag, const std::vector<std::string>& files) {
  std::string root = "/tmp/zf-scan-" + std::string(tag) + "-" + std::to_string(::getpid());
  std::string rm = "rm -rf '" + root + "'";
  if (std::system(rm.c_str()) != 0) {}
  ::mkdir(root.c_str(), 0755);
  for (const auto& f : files) {
    std::string p = root + "/" + f;
    for (size_t i = root.size() + 1; i < p.size(); ++i)
      if (p[i] == '/') ::mkdir(p.substr(0, i).c_str(), 0755);
    std::ofstream(p).put('x');
  }
  return root;
}

void cleanup(const std::string& root) {
  std::string rm = "rm -rf '" + root + "'";
  if (std::system(rm.c_str()) != 0) {}
}

std::vector<std::string> rel(const std::vector<std::string>& v, const std::string& root) {
  std::vector<std::string> o;
  for (const auto& p : v) o.push_back(p.substr(root.size() + 1));
  std::sort(o.begin(), o.end());
  return o;
}

void test_exts() {
  Exts x({".nes", ".ZIP"});
  CHECK(x.matches("a.NES") && x.matches("a.Nes") && x.matches("b.zip") && x.matches("b.ZiP"));
  CHECK(!x.matches("a.nesx") && !x.matches("a.ne"));
  Exts y({"nes"});  // dot optional
  CHECK(!y.matches("nes") && !y.matches(".nes") && !y.matches("a.") && !y.matches(""));
  CHECK(y.matches("a.b.nes") && y.matches("Super (U) [!].nes"));
  CHECK(!y.matches("a.nes.bak"));
  CHECK(Exts({"", "."}).empty());
}

void test_tree() {
  std::string root = make_tree("t", {"a.nes", "B.NES", "c.txt", "noext", ".hid.nes", ".hdir/x.nes", "s1/d.nes", "s1/s2/e.nes", "s1/s2/s3/f.nes", "s1/z.nes"});
  Exts x({".nes"});
  // depth 2: root, s1, s2 are read; s3 is not
  CHECK_EQ(scan_count_games(root, x), size_t(5));
  CHECK(scan_has_game(root, x));
  CHECK(!scan_has_game(root, Exts({"sfc"})));
  CHECK(!scan_has_game(root + "/nope", x));
  auto l = rel(scan_list_games(root, x), root);
  CHECK((l == std::vector<std::string>{"B.NES", "a.nes", "s1/d.nes", "s1/s2/e.nes", "s1/z.nes"}));
  GameList g = GameList::scan(root, x);
  CHECK_EQ(g.size(), size_t(5));
  CHECK_EQ(g.folders(), size_t(3));  // root, s1, s1/s2: one string per folder even though s1 has files before and after s2
  std::vector<std::string> gp;
  for (size_t i = 0; i < g.size(); ++i) gp.push_back(g.path(i));
  CHECK((rel(gp, root) == l));
  for (size_t i = 0; i < g.size(); ++i) CHECK(g.path(i) == g.dir(i) + "/" + std::string(g.file_name(i)));
  auto m = scan_count_many({{root, Exts({"nes"})}, {root, Exts({"txt"})}, {root + "/nope", x}}, 4);
  CHECK((m == std::vector<size_t>{5, 1, 0}));
  CHECK(scan_count_many({}, 2).empty());
  // trailing slash on the root gives clean paths
  GameList g2 = GameList::scan(root + "/", x);
  CHECK_EQ(g2.size(), size_t(5));
  for (size_t i = 0; i < g2.size(); ++i) CHECK(g2.path(i).find("//") == std::string::npos);
  cleanup(root);
}

void test_symlinks() {
  std::string root = make_tree("l", {"real/a.nes", "plain.nes"});
  CHECK(::symlink((root + "/real").c_str(), (root + "/link").c_str()) == 0);
  CHECK(::symlink((root + "/missing").c_str(), (root + "/broken").c_str()) == 0);
  CHECK(::symlink((root + "/plain.nes").c_str(), (root + "/ln.nes").c_str()) == 0);
  Exts x({"nes"});
  CHECK_EQ(scan_count_games(root, x), size_t(4));  // real/a, link/a (followed), plain, ln.nes (file symlink)
  cleanup(root);
}

void test_worker() {
  std::string root = make_tree("w", {"a/1.nes", "a/2.nes", "b/1.sfc", "c/x.txt", "d/s/1.nes"});
  std::vector<CountJob> jobs;
  for (const char* s : {"a", "b", "c", "d"}) jobs.push_back({root + "/" + s, Exts({"nes", "sfc"})});
  std::atomic<int> wakes{0};
  CountWorker w(jobs, 2, 2);
  w.set_wake([&] { ++wakes; });
  std::vector<std::pair<size_t, size_t>> got, tmp;
  for (int i = 0; i < 400 && got.size() < 4; ++i) {
    if (w.drain(tmp)) got.insert(got.end(), tmp.begin(), tmp.end()), tmp.clear();
    else std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK_EQ(got.size(), size_t(4));
  std::sort(got.begin(), got.end());
  CHECK((got == std::vector<std::pair<size_t, size_t>>{{0, 2}, {1, 1}, {2, 0}, {3, 1}}));
  CHECK(w.done());
  // destroying a worker that has not finished must not hang or crash
  std::vector<CountJob> many(2000, jobs[0]);
  { CountWorker w2(many, 2); }
  { CountWorker w3({}, 2); }
  cleanup(root);
}

void test_lazy_systems() {
  std::string root = make_tree("s", {"nes/a.nes", "nes/b.nes", "empty/readme.txt", "gb/x.gb"});
  std::string xml =
      "<systemList>"
      "<system><name>nes</name><fullname>NES</fullname><path>/storage/roms/nes</path><extension>.nes</extension><command>c</command></system>"
      "<system><name>empty</name><fullname>E</fullname><path>/storage/roms/empty</path><extension>.nes</extension><command>c</command></system>"
      "<system><name>gb</name><fullname>GB</fullname><path>/storage/roms/gb</path><extension>.gb .gbc</extension><command>c</command></system>"
      "</systemList>";
  std::vector<System> s;
  std::string err;
  CHECK(load_systems_text_lazy(xml, root, s, err));
  CHECK_EQ(s.size(), size_t(2));  // empty system hidden without counting
  CHECK_EQ(s[0].count, size_t(kCountPending));
  std::vector<System> c;
  CHECK(load_systems_text(xml, root, false, c, err));
  CHECK_EQ(c.size(), size_t(2));
  CHECK_EQ(c[0].count, size_t(2));
  // opening still works from the compact list
  s[0].open();
  CHECK_EQ(s[0].games.size(), size_t(2));
  CHECK_EQ(s[0].count, size_t(2));
  CHECK(s[0].games[0].path.find(root + "/nes/") == 0);
  cleanup(root);
}

// ---- benchmarks

size_t rss_kb() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line))
    if (line.compare(0, 6, "VmRSS:") == 0) return size_t(std::atol(line.c_str() + 6));
  return 0;
}

using Clk = std::chrono::steady_clock;
double ms_since(Clk::time_point t) { return std::chrono::duration<double, std::milli>(Clk::now() - t).count(); }

std::string zlib() {
  const char* e = std::getenv("ZLIB");
  return e ? e : "/dev/shm/zlib";
}

std::vector<System> load_index(const char* mode) {
  std::vector<System> s;
  std::string err, cfg = zlib() + "/es_systems.cfg", roms = zlib() + "/roms";
  bool ok = !std::strcmp(mode, "eager") ? load_systems(cfg, roms, s, err) : !std::strcmp(mode, "lazy") ? load_systems_lazy(cfg, roms, s, err) : load_systems_index(cfg, roms, s, err);
  if (!ok) std::fprintf(stderr, "load: %s\n", err.c_str());
  return s;
}

std::vector<CountJob> jobs_of(const std::vector<System>& s) {
  std::vector<CountJob> j;
  for (const auto& x : s) j.push_back({x.rom_dir, Exts(x.exts)});
  return j;
}

int bench(const char* what, const char* arg) {
  std::string w = what;
  if (w == "--bench-load") {
    const char* mode = arg ? arg : "eager";
    size_t r0 = rss_kb();
    auto t = Clk::now();
    auto s = load_index(mode);
    double ms = ms_since(t);
    size_t g = 0, c = 0;
    for (auto& x : s) g += x.games.size(), c += x.count == kCountPending ? 0 : x.count;
    std::printf("[BENCH] load_systems(%s): %.1f ms systems=%zu games_loaded=%zu counted=%zu rss+%zu KB\n", mode, ms, s.size(), g, c, rss_kb() - r0);
    return 0;
  }
  auto s = load_index("index");
  auto jobs = jobs_of(s);
  if (w == "--bench-has") {
    double best = 1e9, first = 0;
    for (int i = 0; i < 5; ++i) {
      auto t = Clk::now();
      size_t n = 0;
      for (auto& j : jobs) n += scan_has_game(j.dir, j.exts);
      double ms = ms_since(t);
      if (!i) first = ms;
      best = std::min(best, ms);
      if (i == 4) std::printf("[BENCH] has_game x%zu systems (%zu with games): first %.2f ms, best of 5 %.2f ms\n", jobs.size(), n, first, best);
    }
  } else if (w == "--bench-count") {
    for (int th : {1, 2, 3, 4}) {
      double best = 1e9, first = 0;
      size_t tot = 0;
      for (int i = 0; i < 5; ++i) {
        auto t = Clk::now();
        auto r = scan_count_many(jobs, th);
        double ms = ms_since(t);
        tot = 0;
        for (size_t n : r) tot += n;
        if (!i) first = ms;
        best = std::min(best, ms);
      }
      std::printf("[BENCH] count_many threads=%d: first %.2f ms, best of 5 %.2f ms total=%zu\n", th, first, best, tot);
    }
  } else if (w == "--bench-list") {
    bool compact = !arg || std::strcmp(arg, "vector") != 0;
    size_t r0 = rss_kb(), n = 0, bytes = 0;
    auto t = Clk::now();
    std::vector<GameList> keep_g;
    std::vector<std::vector<std::string>> keep_v;
    for (auto& j : jobs) {
      if (compact) {
        keep_g.push_back(GameList::scan(j.dir, j.exts));
        n += keep_g.back().size();
        bytes += keep_g.back().heap_bytes();
      } else {
        keep_v.push_back(scan_list_games(j.dir, j.exts));
        n += keep_v.back().size();
      }
    }
    std::printf("[BENCH] list (%s) of all systems: %.1f ms games=%zu rss+%zu KB%s\n", compact ? "GameList" : "vector<string>", ms_since(t), n, rss_kb() - r0,
                compact ? (" heap=" + std::to_string(bytes / 1024) + " KB").c_str() : "");
    // biggest single system, the case that matters when the user opens it
    size_t bi = 0, bn = 0;
    for (size_t i = 0; i < s.size(); ++i)
      if (s[i].count > bn) bn = s[i].count, bi = i;
    auto t2 = Clk::now();
    if (compact) {
      GameList g = GameList::scan(jobs[bi].dir, jobs[bi].exts);
      std::printf("[BENCH] open biggest system '%s' (%zu games): GameList %.2f ms\n", s[bi].name.c_str(), g.size(), ms_since(t2));
    } else {
      auto v = scan_list_games(jobs[bi].dir, jobs[bi].exts);
      std::printf("[BENCH] open biggest system '%s' (%zu games): vector<string> %.2f ms\n", s[bi].name.c_str(), v.size(), ms_since(t2));
    }
  } else if (w == "--bench-worker") {
    // time to the last count with the real worker (2 threads), and how long the main thread is occupied
    auto t = Clk::now();
    CountWorker cw(jobs, 2, 0);
    double spawn = ms_since(t);
    std::vector<std::pair<size_t, size_t>> got, tmp;
    double first_result = -1;
    while (got.size() < jobs.size()) {
      if (cw.drain(tmp)) {
        if (first_result < 0) first_result = ms_since(t);
        got.insert(got.end(), tmp.begin(), tmp.end()), tmp.clear();
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    }
    std::printf("[BENCH] CountWorker 2 threads: spawn %.3f ms, first result %.2f ms, all %zu systems %.2f ms\n", spawn, first_result, got.size(), ms_since(t));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && !std::strncmp(argv[1], "--bench", 7)) return bench(argv[1], argc > 2 ? argv[2] : nullptr);
  test_exts();
  test_tree();
  test_symlinks();
  test_worker();
  test_lazy_systems();
  return zt::done();
}
