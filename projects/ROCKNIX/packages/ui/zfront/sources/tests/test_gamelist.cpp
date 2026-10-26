// Tests of the streaming gamelist module. `ZF_BENCH=1 ./test_gamelist` also runs the 5,000 / 20,000 game benchmark
// (time, heap and RSS); without it only the functional tests run.
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "check.hpp"
#include "gamelist.hpp"
#include "scan.hpp"
#include "systems.hpp"
#include "util.hpp"

// ---- heap accounting for the benchmark (real chunk sizes)
static std::atomic<size_t> g_live{0}, g_peak{0};
static void account(void* p) {
  size_t n = g_live.fetch_add(malloc_usable_size(p)) + malloc_usable_size(p);
  size_t pk = g_peak.load();
  while (n > pk && !g_peak.compare_exchange_weak(pk, n)) {
  }
}
void* operator new(size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  account(p);
  return p;
}
void* operator new[](size_t n) { return operator new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept {
  void* p = std::malloc(n ? n : 1);
  if (p) account(p);
  return p;
}
void* operator new[](size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }
void operator delete(void* p, const std::nothrow_t&) noexcept;
void operator delete[](void* p, const std::nothrow_t&) noexcept;
void operator delete(void* p) noexcept {
  if (p) g_live -= malloc_usable_size(p);
  std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { operator delete(p); }

using namespace zf;

namespace {

struct Ent {
  std::string path, name, sortname;
  GameMeta m;
  uint64_t b = 0, e = 0;
};

std::vector<Ent> collect_text(const std::string& xml, GamelistInfo* info = nullptr) {
  std::vector<Ent> v;
  auto cb = [&](const GameEntry& g) { v.push_back({std::string(g.path), std::string(g.name), std::string(g.sortname), g.meta, g.begin, g.end}); };
  GamelistInfo i = gamelist_scan_text(xml, EntryFn::of(cb));
  if (info) *info = i;
  return v;
}

std::string slurp(const std::string& p) {
  std::string s;
  read_file(p, s);
  return s;
}

void spit(const std::string& p, const std::string& t) {
  FILE* f = std::fopen(p.c_str(), "wb");
  if (f) {
    std::fwrite(t.data(), 1, t.size(), f);
    std::fclose(f);
  }
}

std::string tmpdir(const char* tag) {
  std::string d = "/tmp/zf_gl_" + std::to_string(getpid()) + "_" + tag;
  std::string c = "rm -rf '" + d + "' && mkdir -p '" + d + "'";
  (void)!std::system(c.c_str());
  return d;
}

const Ent* find(const std::vector<Ent>& v, const std::string& path) {
  for (auto& e : v)
    if (e.path == path) return &e;
  return nullptr;
}

std::string sample() { return slurp(std::string(ZF_TEST_DATA) + "/gamelist-sample.xml"); }

void test_fields() {
  std::string xml = sample();
  GamelistInfo info;
  auto v = collect_text(xml, &info);
  CHECK(info.ok);
  CHECK_EQ(v.size(), size_t(4));  // <folder> ignored
  const Ent* g = find(v, "./Tom & Jerry (Europe).gbc");
  CHECK(g != nullptr);
  if (!g) return;
  CHECK_EQ(g->name, std::string("Tom & Jerry: Frantic Antics!"));
  CHECK_EQ(g->sortname, std::string("Tom and Jerry"));
  CHECK_EQ(g->m.release_year, 1993);
  CHECK_EQ(std::string(g->m.genre), std::string("Platform"));
  CHECK_EQ(std::string(g->m.players), std::string("1-2"));
  CHECK_EQ(int(g->m.rating_pct), 80);
  CHECK(g->m.favorite && !g->m.hidden);
  CHECK_EQ(g->m.playcount, 3u);
  CHECK_EQ(g->m.lastplayed_iso(), std::string("20261020T213000"));
  CHECK_EQ(xml.substr(g->b, 5), std::string("<game"));
  CHECK_EQ(xml.substr(g->e - 7, 7), std::string("</game>"));
  // interned: the same genre in two games is the same storage
  CHECK_EQ(gamelist_intern("Platform").data(), g->m.genre.data());

  CHECK(find(v, "./sub dir/Hidden Game.gbc")->m.hidden);
  const Ent* mn = find(v, "./Minimal.gbc");
  CHECK_EQ(mn->name, std::string());
  CHECK(mn->m.release_year == -1 && mn->m.lastplayed == 0);
  CHECK(mn->m.rating_pct == 0 && mn->m.playcount == 0 && !mn->m.favorite);
  const Ent* w = find(v, "/abs/Weird.gbc");
  CHECK_EQ(w->m.release_year, -1);
  CHECK_EQ(int(w->m.rating_pct), 100);
  CHECK_EQ(w->m.lastplayed, uint64_t(0));
}

void test_tricky_markup() {
  // names that merely start like others, '>' inside an attribute, self-closing, comments, PI, CDATA, nested unknown
  std::string xml =
      "<?xml version=\"1.0\"?><!-- c --><!DOCTYPE x>\n<gameList>\n"
      "<gameListX><game><path>./no.gb</path></game></gameListX>\n"
      "<provider><System>x</System><game><path>./inprovider.gb</path></game></provider>\n"
      "<game id=\"a>b\" note='x > y'><path>./a.gb</path><names>no</names><name>A</name>"
      "<extra><name>NOT</name><path>./evil.gb</path></extra><!-- <name>c</name> --><genre><![CDATA[Fish & <Chips>]]></genre>"
      "<players/><favorite>TRUE</favorite><hidden> True </hidden></game>\n"
      "<game/><game><path/></game>\n"
      "<game><path>./b&#x20;c&#233;.gb</path><name>B &lt;&amp;&gt; &quot;q&quot; &apos;a&apos; &bogus; &#xZZ;</name></game>\n"
      "<folder><path>./sub</path><name>Sub</name></folder>\n"
      "</gameList>trailing";
  GamelistInfo info;
  auto v = collect_text(xml, &info);
  CHECK(info.ok);
  CHECK_EQ(v.size(), size_t(2));
  if (v.size() != 2) return;
  CHECK_EQ(v[0].path, std::string("./a.gb"));
  CHECK_EQ(v[0].name, std::string("A"));  // depth-1 only: <extra><name> does not override
  CHECK_EQ(std::string(v[0].m.genre), std::string("Fish & <Chips>"));
  CHECK(v[0].m.favorite && v[0].m.hidden);
  CHECK_EQ(v[1].path, std::string("./b c\xc3\xa9.gb"));
  CHECK_EQ(v[1].name, std::string("B <&> \"q\" 'a' &bogus; &#xZZ;"));
}

void test_tolerant_and_truncation() {
  CHECK(collect_text("").empty());
  CHECK(collect_text("<gameList><game><path>x").empty());
  CHECK(collect_text("garbage \x01").empty());
  CHECK(collect_text("<other><game><path>./a.gb</path></game></other>").empty());
  CHECK(collect_text("<gameList><game/><game><path/></game></gameList>").empty());
  GamelistInfo info;
  auto v = collect_text("<gameList><game><path>./a.gb</path><rating>abc</rating><playcount>-4</playcount><releasedate>zz</releasedate></game></gameList>", &info);
  CHECK(info.ok && v.size() == 1);
  CHECK(v[0].m.rating_pct == 0 && v[0].m.playcount == 0 && v[0].m.release_year == -1);
  v = collect_text("<gameList><game><path>./a.gb</path><rating>nan</rating><playcount>99999999999</playcount></game></gameList>");
  CHECK(v.size() == 1 && v[0].m.rating_pct == 0 && v[0].m.playcount == 0x7fffffffu);
  CHECK(collect_text("<gameList/>", &info).empty() && info.ok && info.root_selfclosing);
  // every prefix of the sample parses without crashing; games before the cut are still delivered
  std::string xml = sample();
  size_t prev = 0;
  for (size_t n = 0; n <= xml.size(); ++n) {
    auto r = collect_text(xml.substr(0, n), &info);
    CHECK(r.size() >= prev);
    prev = r.size();
    if (n + 1 < xml.size()) CHECK(!info.ok);  // only the final newline may be missing
  }
  CHECK_EQ(prev, size_t(4));
}

void test_file_blocks() {
  // > 128 KB window: elements straddle block boundaries, one element bigger than a block (grows the window)
  std::string d = tmpdir("blocks");
  std::string x = d + "/gamelist.xml";
  std::string xml = "<?xml version=\"1.0\"?>\n<gameList>\n";
  const int N = 3000;
  for (int i = 0; i < N; ++i) {
    xml += "\t<game id=\"" + std::to_string(i) + "\"><path>./g" + std::to_string(i) + ".gb</path><name>Game " + std::to_string(i) + "</name><desc>";
    xml += std::string(size_t(i % 7) * 100 + 50, 'x');
    if (i == 1500) xml += std::string(300 * 1024, 'y');  // bigger than the initial window
    xml += "</desc><rating>0.5</rating></game>\n";
  }
  xml += "</gameList>\n";
  spit(x, xml);
  size_t n = 0, last = 0;
  bool order = true;
  auto cb = [&](const GameEntry& e) {
    order = order && e.path == "./g" + std::to_string(n) + ".gb" && e.name == "Game " + std::to_string(n) && xml.compare(e.begin, 5, "<game") == 0 && e.meta.rating_pct == 50;
    ++n;
    last = size_t(e.end);
  };
  GamelistInfo info = gamelist_scan_file(x, EntryFn::of(cb));
  CHECK(info.ok);
  CHECK(order);
  CHECK_EQ(n, size_t(N));
  CHECK_EQ(xml.substr(last), std::string("\n</gameList>\n"));
  // truncated in the middle of the big element
  spit(x, xml.substr(0, 700 * 1024));
  n = 0;
  info = gamelist_scan_file(x, EntryFn::of(cb));
  CHECK(!info.ok && n > 1000 && n < size_t(N));
  CHECK(!gamelist_scan_file(d + "/missing.xml", EntryFn::of(cb)).opened);
}

void test_details() {
  std::string d = tmpdir("details");
  std::string x = d + "/gamelist.xml";
  std::string xml = sample();
  spit(x, xml);
  std::string rom = d + "/Tom & Jerry (Europe).gbc";
  GameMeta meta;
  std::string rom_rel = "./Tom & Jerry (Europe).gbc";
  for (auto& e : collect_text(xml))
    if (e.path == rom_rel) meta = e.m;
  GameDetails det = gamelist_details(x, d, rom, meta);
  CHECK_EQ(det.image, d + "/media/images/Tom & Jerry.png");
  CHECK_EQ(det.thumbnail, d + "/media/thumbs/tj.png");
  CHECK(det.desc.find("<cat>") != std::string::npos && det.desc.find('\n') != std::string::npos && det.desc.find("caf\xc3\xa9") != std::string::npos);
  CHECK(det.desc.find("Cat & mouse") == 0);
  // game without desc
  GameMeta mm;
  for (auto& e : collect_text(xml))
    if (e.path == "./Minimal.gbc") mm = e.m;
  GameDetails dm = gamelist_details(x, d, d + "/Minimal.gbc", mm);
  CHECK(dm.desc.empty() && dm.image.empty() && dm.thumbnail.empty());
  // wrong span (another game's): falls back to the search and still answers right
  GameDetails dw = gamelist_details(x, d, rom, mm);
  CHECK_EQ(dw.thumbnail, d + "/media/thumbs/tj.png");
  // span pointing into the middle of an element
  GameMeta bad = meta;
  bad.span_begin += 3;
  CHECK_EQ(gamelist_details(x, d, rom, bad).image, d + "/media/images/Tom & Jerry.png");
  // no span (ROM not in the gamelist): empty at once, no file scan
  GameMeta none;
  CHECK(gamelist_details(x, d, rom, none).image.empty());
  // stale but non-zero span (file edited since loading): recovered by one search
  GameMeta stale = meta;
  stale.span_begin += 5000;
  stale.span_end += 5000;
  CHECK_EQ(gamelist_details(x, d, rom, stale).image, d + "/media/images/Tom & Jerry.png");
  CHECK(gamelist_details(x, d, d + "/nope.gbc", none).desc.empty());
  CHECK(gamelist_details(d + "/missing.xml", d, rom, meta).desc.empty());
}

void test_update_existing() {
  std::string d = tmpdir("play");
  std::string x = d + "/gamelist.xml";
  std::string xml = sample();
  spit(x, xml);
  std::string rom = d + "/Tom & Jerry (Europe).gbc", err;
  GamelistEdit ed;
  CHECK(gamelist_update_after_play(x, d, rom, "20261025T101500", err, &ed));
  std::string t = slurp(x);
  CHECK(t.find("<playcount>4</playcount>") != std::string::npos && t.find("<lastplayed>20261025T101500</lastplayed>") != std::string::npos);
  std::string back = t;
  auto rep = [&](const std::string& a, const std::string& b) {
    size_t p = back.find(a);
    if (p != std::string::npos) back.replace(p, a.size(), b);
  };
  rep("<playcount>4</playcount>", "<playcount>3</playcount>");
  rep("20261025T101500", "20261020T213000");
  CHECK_EQ(back, xml);  // everything else byte for byte
  CHECK_EQ(ed.new_len - ed.old_len, t.size() - xml.size());
  CHECK(!file_exists(x + ".zfront-tmp"));
  CHECK_EQ(find(collect_text(t), "./Tom & Jerry (Europe).gbc")->m.playcount, 4u);
  // favorite off then on keeps a single element
  CHECK(gamelist_set_favorite(x, d, rom, false, err));
  CHECK(!find(collect_text(slurp(x)), "./Tom & Jerry (Europe).gbc")->m.favorite);
  t = slurp(x);
  CHECK_EQ(t.find("<favorite>", t.find("<favorite>") + 1), std::string::npos);
}

void test_update_missing() {
  std::string d = tmpdir("new");
  std::string x = d + "/gamelist.xml";
  spit(x, sample());
  std::string err;
  CHECK(gamelist_update_after_play(x, d, d + "/Minimal.gbc", "20261025T000001", err));  // game without playcount / lastplayed
  std::string r2 = d + "/New & Shiny <game>.gbc";
  CHECK(gamelist_update_after_play(x, d, r2, "20261025T000002", err));  // not in the file
  CHECK(gamelist_update_after_play(x, d, r2, "20261025T000003", err));
  CHECK(gamelist_update_after_play(x, d, d + "/sub/in folder.gb", "20261025T000004", err));
  GamelistInfo info;
  auto v = collect_text(slurp(x), &info);
  CHECK(info.ok);
  CHECK_EQ(v.size(), size_t(6));
  const Ent* a = find(v, "./Minimal.gbc");
  CHECK(a && a->m.playcount == 1 && a->m.lastplayed_iso() == "20261025T000001");
  const Ent* b = find(v, "./New & Shiny <game>.gbc");
  CHECK(b && b->m.playcount == 2 && b->m.lastplayed_iso() == "20261025T000003");
  CHECK(find(v, "./sub/in folder.gb") != nullptr);
  CHECK_EQ(find(v, "./Tom & Jerry (Europe).gbc")->m.playcount, 3u);
  // a ROM outside the folder is stored with its absolute path
  CHECK(gamelist_update_after_play(x, d, "/other/place/z.gb", "20261025T000005", err));
  CHECK(find(collect_text(slurp(x)), "/other/place/z.gb") != nullptr);
}

void test_create_blank_selfclosing() {
  std::string d = tmpdir("create");
  std::string x = d + "/gamelist.xml", err;
  std::string rom = d + "/sub/a b.gb";
  CHECK(gamelist_set_favorite(x, d, rom, true, err));  // file does not exist
  CHECK(find(collect_text(slurp(x)), "./sub/a b.gb")->m.favorite);
  CHECK(gamelist_set_favorite(x, d, rom, false, err));
  CHECK(gamelist_update_after_play(x, d, rom, "20261025T000000", err));
  auto vg = collect_text(slurp(x));
  const Ent* g = find(vg, "./sub/a b.gb");
  CHECK(!g->m.favorite && g->m.playcount == 1);
  spit(x, "");  // empty
  CHECK(gamelist_set_favorite(x, d, rom, true, err));
  CHECK(find(collect_text(slurp(x)), "./sub/a b.gb")->m.favorite);
  spit(x, " \n\t ");  // blank
  CHECK(gamelist_set_favorite(x, d, rom, true, err));
  CHECK(find(collect_text(slurp(x)), "./sub/a b.gb")->m.favorite);
  spit(x, "<gameList/>");
  CHECK(gamelist_set_favorite(x, d, d + "/a.gb", true, err));
  CHECK(find(collect_text(slurp(x)), "./a.gb")->m.favorite);
  spit(x, "<?xml version=\"1.0\"?>\n<gameList></gameList>");  // no newline before the closing tag
  CHECK(gamelist_set_favorite(x, d, d + "/a.gb", true, err));
  GamelistInfo info;
  auto v = collect_text(slurp(x), &info);
  CHECK(info.ok && v.size() == 1);
}

void test_broken_untouched() {
  std::string d = tmpdir("broken");
  std::string x = d + "/gamelist.xml", err;
  const char* bad[] = {"<gameList><game>", "<gameList><game><path>./a.gb</path></game>", "hello world", "<other/>", "<gameList><game id=\"x></game></gameList>"};
  for (const char* b : bad) {
    spit(x, b);
    err.clear();
    bool ok = gamelist_update_after_play(x, d, d + "/a.gb", "20261025T000000", err);
    CHECK(!ok);
    CHECK_EQ(slurp(x), std::string(b));
    CHECK(!err.empty());
    CHECK(!file_exists(x + ".zfront-tmp"));
  }
}

void test_apply_edit_spans() {
  // after an update the spans of the loaded games are shifted and details still work
  std::string d = tmpdir("shift");
  std::string x = d + "/gamelist.xml";
  std::string xml = "<gameList>\n";
  for (int i = 0; i < 5; ++i) xml += "<game><path>./g" + std::to_string(i) + ".gb</path><name>G" + std::to_string(i) + "</name><desc>d" + std::to_string(i) + "</desc></game>\n";
  xml += "</gameList>\n";
  spit(x, xml);
  for (int i = 0; i < 5; ++i) spit(d + "/g" + std::to_string(i) + ".gb", "x");
  System s;
  s.rom_dir = d;
  s.gamelist = x;
  s.exts = {".gb"};
  s.open();
  CHECK_EQ(s.games.size(), size_t(5));
  CHECK(s.loaded);
  std::string err;
  GamelistEdit ed;
  CHECK(gamelist_update_after_play(x, d, d + "/g1.gb", "20261025T101500", err, &ed));
  s.apply_edit(ed);
  for (auto& g : s.games) {
    std::string want = "d" + g.name.substr(1);
    GameMeta m = g.meta;
    std::string t = slurp(x);
    CHECK_EQ(t.compare(m.span_begin, 5, "<game"), 0);
    CHECK_EQ(t.substr(m.span_end - 7, 7), std::string("</game>"));
    CHECK_EQ(gamelist_details(x, d, g.path, g.meta).desc, want);
  }
}

void test_load_games_order() {
  // ES order: sortname or name, upper case, leading article dropped; hidden out; names from the file or the stem
  std::string d = tmpdir("order");
  for (const char* n : {"The Zelda.gb", "A Bug.gb", "an Apple.gb", "Mario.gb", "Hidden.gb", "zeta.gb", "\xc3\xa9" "clair.gb", "Theme.gb", "other.txt"}) spit(d + "/" + n, "x");
  spit(d + "/gamelist.xml",
       "<gameList><game><path>./Hidden.gb</path><hidden>true</hidden></game>"
       "<game><path>./Mario.gb</path><name>Mario</name><sortname>Aardvark Mario</sortname></game>"
       "<game><path>./zeta.gb</path><name>Zeta &amp; Co</name></game></gameList>");
  auto g = load_games(d, {".gb"}, d + "/gamelist.xml");
  std::vector<std::string> names;
  for (auto& x : g) names.push_back(x.name);
  // keys: APPLE, BUG, MARIO->AARDVARK MARIO, THEME, ZELDA, ZETA & CO, \xc3\x89CLAIR
  std::vector<std::string> want = {"Mario", "an Apple", "A Bug", "Theme", "The Zelda", "Zeta & Co", "\xc3\xa9" "clair"};
  // sort keys: "AARDVARK MARIO" < "APPLE" < "BUG" < "THEME" < "ZELDA" < "ZETA & CO" < "\xc3\x89CLAIR"
  CHECK_EQ(names.size(), want.size());
  for (size_t i = 0; i < names.size() && i < want.size(); ++i) CHECK_EQ(names[i], want[i]);
  // a system whose games are all hidden is not reloaded by every open()
  System s;
  s.rom_dir = d;
  s.gamelist = d + "/gamelist.xml";
  s.exts = {".gb"};
  s.open();
  CHECK(s.loaded);
}

// ---------------------------------------------------------------------------------------------------------------
// benchmark

long status_kb(const char* key) {
  FILE* f = std::fopen("/proc/self/status", "r");
  char line[256];
  long v = -1;
  size_t kl = std::strlen(key);
  while (f && std::fgets(line, sizeof line, f))
    if (std::strncmp(line, key, kl) == 0) v = std::atol(line + kl);
  if (f) std::fclose(f);
  return v;
}

void reset_hwm() {
  FILE* f = std::fopen("/proc/self/clear_refs", "w");
  if (f) {
    std::fputs("5", f);
    std::fclose(f);
  }
}

double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void make_synthetic(const std::string& dir, int n) {
  std::string cmd = "rm -rf '" + dir + "' && mkdir -p '" + dir + "'";
  (void)!std::system(cmd.c_str());
  FILE* f = std::fopen((dir + "/gamelist.xml").c_str(), "wb");
  std::fputs("<?xml version=\"1.0\"?>\n<gameList>\n", f);
  std::string lorem;
  while (lorem.size() < 400) lorem += "Lorem ipsum dolor sit amet. ";
  for (int i = 0; i < n; ++i) {
    std::fprintf(f,
                 "\t<game id=\"%d\" source=\"ScreenScraper.fr\">\n\t\t<path>./Game number %d (Europe) (Rev 1).gbc</path>\n\t\t<name>Game number %d: The Quest &amp; More</name>\n"
                 "\t\t<desc>%s</desc>\n\t\t<image>./media/images/Game number %d.png</image>\n\t\t<thumbnail>./media/thumbs/Game number %d.png</thumbnail>\n"
                 "\t\t<marquee>./media/marquee/Game number %d.png</marquee>\n\t\t<video>./media/video/Game number %d.mp4</video>\n"
                 "\t\t<rating>0.6</rating>\n\t\t<releasedate>19950101T000000</releasedate>\n\t\t<developer>Dev %d</developer>\n\t\t<publisher>Pub</publisher>\n"
                 "\t\t<genre>Platform</genre>\n\t\t<players>1-2</players>\n\t\t<playcount>2</playcount>\n\t\t<lastplayed>20261020T213000</lastplayed>\n\t</game>\n",
                 i, i, i, lorem.c_str(), i, i, i, i, i % 50);
    std::string rom = dir + "/Game number " + std::to_string(i) + " (Europe) (Rev 1).gbc";
    FILE* r = std::fopen(rom.c_str(), "wb");
    if (r) std::fclose(r);
  }
  std::fputs("</gameList>\n", f);
  std::fclose(f);
}

// ZF_FAKELIB=<dir made by mkfakelib.sh>: index load + open() of every system, slowest first
void bench_fakelib(const char* root) {
  std::vector<System> v;
  std::string err;
  double t = now_ms();
  bool ok = load_systems_index(std::string(root) + "/es_systems.cfg", std::string(root) + "/roms", v, err);
  std::printf("== fakelib %s: load_systems_index %.1f ms ok=%d systems=%zu\n", root, now_ms() - t, int(ok), v.size());
  std::vector<std::pair<double, std::string>> r;
  size_t total = 0, withgl = 0;
  reset_hwm();
  long rss0 = status_kb("VmRSS:");
  for (auto& s : v) {
    withgl += file_exists(s.gamelist);
    double t0 = now_ms();
    s.open();
    r.push_back({now_ms() - t0, s.name + " (" + std::to_string(s.games.size()) + " games" + (file_exists(s.gamelist) ? ", gamelist" : "") + ")"});
    total += s.games.size();
  }
  std::sort(r.rbegin(), r.rend());
  for (size_t i = 0; i < r.size() && i < 4; ++i) std::printf("   open() %.1f ms  %s\n", r[i].first, r[i].second.c_str());
  std::printf("   all systems opened: %zu games, %zu gamelists, VmHWM-VmRSS0 +%.2f MB\n", total, withgl, double(status_kb("VmHWM:") - rss0) / 1024.0);
}

void bench() {
  const char* base = std::getenv("ZF_BENCH_DIR");
  const char* only = std::getenv("ZF_BENCH_N");  // run one size per process for clean RSS numbers
  for (int n : {5000, 20000}) {
    if (only && std::atoi(only) != n) continue;
    std::string dir = std::string(base ? base : "/dev/shm") + "/zf_gl_bench_" + std::to_string(n);
    make_synthetic(dir, n);
    std::string x = dir + "/gamelist.xml";
    struct stat st;
    stat(x.c_str(), &st);
    std::printf("== N=%d  xml=%.1f MB\n", n, double(st.st_size) / 1048576.0);

    {  // 0. cold: first load in this process, RSS as the kernel sees it
      reset_hwm();
      long rss0 = status_kb("VmRSS:"), anon0 = status_kb("RssAnon:");
      size_t live0 = g_live.load();
      g_peak = live0;
      auto g = load_games(dir, {".gbc"}, x);
      std::printf("cold load_games: heap peak +%.2f MB, resident heap %.2f MB; RssAnon +%.2f MB, VmHWM-VmRSS0 +%.2f MB (n=%zu)\n", double(g_peak.load() - live0) / 1048576.0,
                  double(g_live.load() - live0) / 1048576.0, double(status_kb("RssAnon:") - anon0) / 1024.0, double(status_kb("VmHWM:") - rss0) / 1024.0, g.size());
    }
    // 1. streaming parse alone (callback does nothing)
    size_t cnt = 0;
    auto cb = [&](const GameEntry&) { ++cnt; };
    double best = 1e9;
    for (int r = 0; r < 5; ++r) {
      cnt = 0;
      double t = now_ms();
      gamelist_scan_file(x, EntryFn::of(cb));
      best = std::min(best, now_ms() - t);
    }
    reset_hwm();
    size_t live0 = g_live.load();
    g_peak = live0;
    long rss0 = status_kb("VmRSS:");
    cnt = 0;
    gamelist_scan_file(x, EntryFn::of(cb));
    std::printf("scan only      : %.1f ms (best of 5)  heap peak +%.2f MB  (VmHWM-VmRSS0 %.2f MB)  games=%zu\n", best, double(g_peak.load() - live0) / 1048576.0,
                double(status_kb("VmHWM:") - rss0) / 1024.0, cnt);

    // 2. load_games: scan of ROM files + gamelist + sort
    std::vector<double> ts;
    for (int r = 0; r < 5; ++r) {
      double t = now_ms();
      auto g = load_games(dir, {".gbc"}, x);
      ts.push_back(now_ms() - t);
      CHECK_EQ(g.size(), size_t(n));
    }
    std::sort(ts.begin(), ts.end());
    reset_hwm();
    live0 = g_live.load();
    g_peak = live0;
    rss0 = status_kb("VmRSS:");
    long anon0 = status_kb("RssAnon:");
    auto g = load_games(dir, {".gbc"}, x);
    size_t kept = g_live.load() - live0;
    std::printf("load_games     : %.1f ms median  heap peak +%.2f MB  residual %.2f MB = %zu B/game (sizeof Game=%zu, GameMeta=%zu)  RssAnon +%.2f MB  VmHWM-VmRSS0 %.2f MB\n",
                ts[ts.size() / 2], double(g_peak.load() - live0) / 1048576.0, double(kept) / 1048576.0, kept / size_t(n), sizeof(Game), sizeof(GameMeta),
                double(status_kb("RssAnon:") - anon0) / 1024.0, double(status_kb("VmHWM:") - rss0) / 1024.0);
    size_t name_heap = 0;
    for (auto& e : g) name_heap += e.name.capacity() > 15 ? e.name.capacity() + 1 : 0;
    std::printf("                 of which gamelist-attributable: GameMeta %zu B inline + names heap %zu B/game\n", sizeof(GameMeta), name_heap / size_t(n));

    // 3. one details() and one update
    ts.clear();
    for (int r = 0; r < 9; ++r) {
      const Game& m = g[size_t(n / 2 + r)];
      double t = now_ms();
      GameDetails d = gamelist_details(x, dir, m.path, m.meta);
      ts.push_back(now_ms() - t);
      CHECK(!d.desc.empty());
    }
    std::sort(ts.begin(), ts.end());
    std::printf("details()      : %.3f ms median\n", ts[ts.size() / 2]);
    ts.clear();
    std::string err;
    for (int r = 0; r < 5; ++r) {
      double t = now_ms();
      GamelistEdit ed;
      CHECK(gamelist_update_after_play(x, dir, g[size_t(n / 3)].path, "20261025T101500", err, &ed));
      ts.push_back(now_ms() - t);
    }
    std::sort(ts.begin(), ts.end());
    std::printf("update_after_play: %.1f ms median (rewrites %.1f MB, fsync included)\n", ts[ts.size() / 2], double(st.st_size) / 1048576.0);
    std::string c = "rm -rf '" + dir + "'";
    (void)!std::system(c.c_str());
  }
}

}  // namespace

int main() {
  test_fields();
  test_tricky_markup();
  test_tolerant_and_truncation();
  test_file_blocks();
  test_details();
  test_update_existing();
  test_update_missing();
  test_create_blank_selfclosing();
  test_broken_untouched();
  test_apply_edit_spans();
  test_load_games_order();
  if (std::getenv("ZF_BENCH")) bench();
  if (const char* fl = std::getenv("ZF_FAKELIB")) bench_fakelib(fl);
  std::string c = "rm -rf /tmp/zf_gl_" + std::to_string(getpid()) + "_*";
  (void)!std::system(c.c_str());
  return zt::done();
}
