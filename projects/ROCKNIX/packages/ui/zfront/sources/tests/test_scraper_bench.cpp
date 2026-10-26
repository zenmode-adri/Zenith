// CPU and memory of a scrape of 2000 games on the fake-curl rig. Runs only when $ZF_SCR_BENCH is set (nothing under ctest).
// The process' own CPU is what the engine costs; the fake `curl` (a bash script) is accounted as children.
//   ZF_SCR_BENCH=2000 ./test_scraper_bench
#include <sys/resource.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app.hpp"
#include "check.hpp"
#include "scraper_fake.hpp"
#include "scraper_run.hpp"

using namespace zf;
using namespace zf::scraper;
using namespace scrfake;

static double secs(const timeval& t) { return double(t.tv_sec) + double(t.tv_usec) / 1e6; }

int main() {
  const char* e = std::getenv("ZF_SCR_BENCH");
  if (!e) return 0;
  int n = std::max(1, std::atoi(e));
  Rig t;
  put(t.fake + "/game/default.xml", slurp(std::string(ZF_TEST_DATA) + "/scraper/jeu_sfc.xml"));
  std::string gl = "<?xml version=\"1.0\"?>\n<gameList>\n";
  for (int i = 0; i < n; ++i) {
    std::string name = "rom" + std::to_string(i) + ".sfc";
    put(t.roms() + "/" + name, std::string(32768, char('a' + i % 26)));
    gl += "\t<game>\n\t\t<path>./" + name + "</path>\n\t\t<name>Rom " + std::to_string(i) + "</name>\n\t\t<playcount>1</playcount>\n\t</game>\n";
  }
  put(t.roms() + "/gamelist.xml", gl + "</gameList>\n");
  t.systems3();
  Options o = t.opts();
  o.systems = {0};
  o.flush_every = 50;
  o.min_interval_ms = 0;
  auto r = Run::create(t.cx, o);
  rusage a{}, b{}, ca{}, cb{};
  getrusage(RUSAGE_SELF, &a);
  getrusage(RUSAGE_CHILDREN, &ca);
  auto t0 = std::chrono::steady_clock::now();
  r->start();
  size_t polls = 0;
  bool ok = t.pump(*r, [&] { ++polls; return r->finished(); }, 3600 * 1000);
  double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  getrusage(RUSAGE_SELF, &b);
  getrusage(RUSAGE_CHILDREN, &cb);
  CHECK(ok);
  CHECK_EQ(r->stats().found, size_t(n));
  double self = secs(b.ru_utime) + secs(b.ru_stime) - secs(a.ru_utime) - secs(a.ru_stime);
  double kids = secs(cb.ru_utime) + secs(cb.ru_stime) - secs(ca.ru_utime) - secs(ca.ru_stime);
  std::printf("bench: games=%d found=%zu media=%zu wall=%.1fs engine_cpu=%.2fs (%.1f ms/game) fake_curl_cpu=%.1fs maxrss=%ld KB main_loop_polls=%zu\n", n,
              r->stats().found, r->stats().media, wall, self, 1000.0 * self / n, kids, b.ru_maxrss, polls);
  std::printf("bench: gamelist=%ld bytes\n", long(fs::file_size(t.roms() + "/gamelist.xml")));
  return zt::done();
}
