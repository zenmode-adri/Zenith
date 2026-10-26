// i18n: .mo reader (both endians, plural forms, contexts, corrupt files), language fall-back, the real ES catalogs when
// they exist on this machine, allocation-free tr(), and the lazy fallback fonts of text.cpp.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "check.hpp"
#include "alloc_nothrow.hpp"
#include "i18n.hpp"
#include "services.hpp"
#include "settings.hpp"
#include "text.hpp"
#include "util.hpp"

using namespace zf;

// ---- allocation counter (tr() must not allocate) --------------------------------------------------------------------
static size_t g_allocs = 0;
void* operator new(size_t n) {
  ++g_allocs;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static const std::string kData = std::string(ZF_TEST_DATA) + "/i18n";

// ---- tiny .mo writer (for cases msgfmt cannot produce: unsorted, empty msgstr, truncated) ---------------------------
struct Ent {
  std::string k, v;
};
static std::string make_mo(std::vector<Ent> e, bool big, bool sort = true) {
  if (sort) std::sort(e.begin(), e.end(), [](const Ent& a, const Ent& b) { return std::strcmp(a.k.c_str(), b.k.c_str()) < 0; });
  auto put = [&](std::string& s, uint32_t v) {
    unsigned char b[4];
    if (big) b[0] = v >> 24, b[1] = v >> 16, b[2] = v >> 8, b[3] = v;
    else b[3] = v >> 24, b[2] = v >> 16, b[1] = v >> 8, b[0] = v;
    s.append(reinterpret_cast<char*>(b), 4);
  };
  uint32_t n = uint32_t(e.size());
  std::string out;
  put(out, 0x950412deu);  // written in the chosen byte order: reads as 0xde120495 on the other side
  put(out, 0);
  put(out, n);
  put(out, 28);
  put(out, 28 + 8 * n);
  put(out, 0);
  put(out, 0);
  std::string strs;
  uint32_t base = 28 + 16 * n;
  std::string ot, tt;
  for (auto& x : e) {
    put(ot, uint32_t(x.k.size()));
    put(ot, base + uint32_t(strs.size()));
    strs += x.k;
    strs.push_back('\0');
  }
  for (auto& x : e) {
    put(tt, uint32_t(x.v.size()));
    put(tt, base + uint32_t(strs.size()));
    strs += x.v;
    strs.push_back('\0');
  }
  return out + ot + tt + strs;
}

static bool write_file(const std::string& p, const std::string& s) {
  FILE* f = std::fopen(p.c_str(), "wb");
  if (!f) return false;
  std::fwrite(s.data(), 1, s.size(), f);
  std::fclose(f);
  return true;
}

static size_t rss_kb() {
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  long a = 0, b = 0;
  if (std::fscanf(f, "%ld %ld", &a, &b) != 2) b = 0;
  std::fclose(f);
  return size_t(b) * size_t(sysconf(_SC_PAGESIZE)) / 1024;
}

static double now_ns() {
  return double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void test_plural_expr() {
  i18n::Plural p;
  CHECK(p.parse("nplurals=2; plural=(n != 1);"));
  CHECK_EQ(p.nplurals(), 2u);
  CHECK_EQ(p.eval(0), 1u);
  CHECK_EQ(p.eval(1), 0u);
  CHECK_EQ(p.eval(2), 1u);

  CHECK(p.parse("nplurals=1; plural=0;"));
  CHECK_EQ(p.eval(7), 0u);

  CHECK(p.parse("nplurals=2; plural=n>1;"));  // French
  CHECK_EQ(p.eval(0), 0u);
  CHECK_EQ(p.eval(1), 0u);
  CHECK_EQ(p.eval(2), 1u);

  // Russian: 1, 2, 5, 11, 21, 22 -> 0 1 2 2 0 1
  CHECK(p.parse("nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);"));
  CHECK_EQ(p.nplurals(), 3u);
  const unsigned long ns[] = {1, 2, 5, 11, 21, 22, 0, 100, 101, 111, 112};
  const unsigned want[] = {0, 1, 2, 2, 0, 1, 2, 2, 0, 2, 2};
  for (size_t i = 0; i < sizeof ns / sizeof *ns; ++i) CHECK_EQ(p.eval(ns[i]), want[i]);

  // Polish and Arabic (6 forms)
  CHECK(p.parse("nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);"));
  CHECK_EQ(p.eval(1), 0u);
  CHECK_EQ(p.eval(3), 1u);
  CHECK_EQ(p.eval(12), 2u);
  CHECK(p.parse("nplurals=6; plural=n==0 ? 0 : n==1 ? 1 : n==2 ? 2 : n%100>=3 && n%100<=10 ? 3 : n%100>=11 ? 4 : 5;"));
  CHECK_EQ(p.nplurals(), 6u);
  CHECK_EQ(p.eval(0), 0u);
  CHECK_EQ(p.eval(2), 2u);
  CHECK_EQ(p.eval(5), 3u);
  CHECK_EQ(p.eval(11), 4u);
  CHECK_EQ(p.eval(100), 5u);
  CHECK_EQ(p.eval(103), 3u);
  // result is clamped below nplurals
  CHECK(p.parse("nplurals=2; plural=n;"));
  CHECK_EQ(p.eval(9), 1u);
  // garbage falls back to n != 1
  CHECK(!p.parse("nplurals=2; plural=(n +;"));
  CHECK_EQ(p.eval(1), 0u);
  CHECK_EQ(p.eval(3), 1u);
  CHECK(!p.parse(""));
  CHECK(!p.parse("nplurals=2; plural=n != 1; trailing"));
  CHECK(!p.parse("nplurals=0; plural=0;"));
  CHECK(p.parse("nplurals=2; plural=(n % 0) + n/0 != 1;"));  // division by zero is 0, not a crash
}

static void test_candidates() {
  using V = std::vector<std::string>;
  CHECK(i18n::candidates("es_ES.UTF-8") == (V{"es_ES", "es"}));
  CHECK(i18n::candidates("zh_CN") == (V{"zh_CN", "zh"}));
  CHECK(i18n::candidates("pt-BR") == (V{"pt_BR", "pt"}));
  CHECK(i18n::candidates("ca@valencia") == (V{"ca"}));
  CHECK(i18n::candidates("ko") == (V{"ko"}));
  CHECK(i18n::candidates("en_GB") == (V{"en_GB", "en"}));
  CHECK(i18n::candidates("en_US").empty());
  CHECK(i18n::candidates("en_US.UTF-8").empty());
  CHECK(i18n::candidates("en").empty());
  CHECK(i18n::candidates("").empty());
  CHECK(i18n::candidates("C").empty());
  CHECK(i18n::candidates("POSIX").empty());
}

static void test_mo_files() {
  // msgfmt little endian
  i18n::Catalog le, be;
  std::string err;
  CHECK(le.open(kData + "/es/LC_MESSAGES/zfront.mo", &err));
  CHECK(be.open(kData + "/es-be.mo", &err));
  CHECK_EQ(le.size(), be.size());
  CHECK(le.size() >= 8);
  for (const i18n::Catalog* c : {&le, &be}) {
    CHECK_EQ(std::string(c->find("YES")), std::string("S\xc3\x8d"));
    CHECK_EQ(std::string(c->find("QUIT")), std::string("SALIR"));
    CHECK_EQ(std::string(c->find("Not available yet")), std::string("A\xc3\xban no disponible"));
    CHECK_EQ(std::string(c->find("Line\nbreak \"quoted\"")), std::string("Salto\nde l\xc3\xadnea \"entre comillas\""));
    CHECK(c->find("NOT TRANSLATED") == nullptr);
    CHECK(c->find("never heard of it") == nullptr);
    CHECK(c->find("") != nullptr);  // header
    // plural: key is the singular only, forms are NUL separated
    CHECK_EQ(std::string(c->find_form("%d day ago", 0)), std::string("hace %d d\xc3\xad" "a"));
    CHECK_EQ(std::string(c->find_form("%d day ago", 1)), std::string("hace %d d\xc3\xad" "as"));
    CHECK(c->find_form("%d day ago", 2) == nullptr);
    CHECK(c->find("%d days ago") == nullptr);  // the plural msgid is not a key
    // context
    CHECK_EQ(std::string(c->find("game_options\x04" "DELETE")), std::string("BORRAR JUEGO"));
    CHECK_EQ(std::string(c->find("other\x04" "DELETE")), std::string("ELIMINAR"));
    CHECK_EQ(std::string(c->find("DELETE")), std::string("BORRAR"));
    CHECK_EQ(c->plural().nplurals(), 2u);
  }
  // identical content in both byte orders
  for (size_t i = 0; i < le.size(); ++i) {
    CHECK_EQ(std::string(le.original(i)), std::string(be.original(i)));
    CHECK_EQ(std::string(le.translation(i)), std::string(be.translation(i)));
  }

  // hand-built files: the writer must agree with msgfmt, and an empty msgstr is "no translation"
  std::vector<Ent> ents = {{"", "Plural-Forms: nplurals=2; plural=(n != 1);\n"}, {"A", "a"}, {"B", ""}, {"C", "c1" + std::string(1, '\0') + "c2"}};
  for (bool big : {false, true}) {
    std::string m = make_mo(ents, big);
    i18n::Catalog c;
    CHECK(c.open_memory(m.data(), m.size(), &err));
    CHECK_EQ(std::string(c.find("A")), std::string("a"));
    CHECK(c.find("B") == nullptr);
    CHECK_EQ(std::string(c.find_form("C", 1)), std::string("c2"));
  }
  // corrupt files are rejected, never crash
  {
    std::string good = make_mo(ents, false);
    i18n::Catalog c;
    std::vector<Ent> rev = {{"B", "b"}, {"A", "a"}};
    std::string uns = make_mo(rev, false, false);
    CHECK(!c.open_memory(uns.data(), uns.size(), &err));  // unsorted originals
    CHECK(!c.valid());
    for (size_t cut : {size_t(0), size_t(10), size_t(27), size_t(28), good.size() / 2, good.size() - 1}) {
      std::string t = good.substr(0, cut);
      CHECK(!c.open_memory(t.data(), t.size(), &err));
    }
    std::string badmagic = good;
    badmagic[0] ^= 0x55;
    CHECK(!c.open_memory(badmagic.data(), badmagic.size(), &err));
    std::string hugecount = good;
    hugecount[8] = char(0xff), hugecount[9] = char(0xff);  // N = 0x....ffff entries: tables would not fit
    CHECK(!c.open_memory(hugecount.data(), hugecount.size(), &err));
    std::string badoff = good;
    badoff[28 + 4] = char(0xff), badoff[28 + 5] = char(0xff);  // first original points far outside
    CHECK(!c.open_memory(badoff.data(), badoff.size(), &err));
    // truncated on disk (mmap path): size is taken from fstat, so it is just invalid
    char dir[] = "/tmp/zf-i18n-XXXXXX";
    CHECK(::mkdtemp(dir) != nullptr);
    std::string p = std::string(dir) + "/trunc.mo";
    CHECK(write_file(p, good.substr(0, good.size() - 3)));
    CHECK(!c.open(p, &err));
    CHECK(write_file(p, std::string("not a catalog at all, just text........")));
    CHECK(!c.open(p, &err));
    CHECK(!c.open(std::string(dir) + "/missing.mo", &err));
    std::remove(p.c_str());
    ::rmdir(dir);
  }
}

static void test_language_selection() {
  i18n::Options o;
  o.dirs = {kData};
  o.domains = {"zfront", "emulationstation2"};

  CHECK_EQ(std::string(tr("YES")), std::string("YES"));  // identity before any catalog
  // es_ES.UTF-8 -> es
  CHECK(i18n::set_language("es_ES.UTF-8", o));
  CHECK_EQ(i18n::loaded_catalogs(), size_t(1));
  CHECK_EQ(i18n::language(), std::string("es_ES"));
  CHECK_EQ(std::string(tr("YES")), std::string("S\xc3\xad"));
  CHECK_EQ(std::string(tr("Not available yet")), std::string("A\xc3\xban no disponible"));
  const char* unknown = "something nobody translated";
  CHECK(tr(unknown) == unknown);  // the very same pointer back
  CHECK(tr("") != nullptr);
  CHECK_EQ(std::string(tr("")), std::string(""));
  CHECK_EQ(std::string(i18n::ctr("game_options", "DELETE")), std::string("BORRAR JUEGO"));
  CHECK_EQ(std::string(i18n::ctr("nope", "DELETE")), std::string("Delete"));  // no translation: sentence case
  CHECK_EQ(std::string(i18n::ntr("%d day ago", "%d days ago", 1)), std::string("hace %d d\xc3\xad" "a"));
  CHECK_EQ(std::string(i18n::ntr("%d day ago", "%d days ago", 3)), std::string("hace %d d\xc3\xad" "as"));
  CHECK_EQ(std::string(i18n::ntr("%d hour ago", "%d hours ago", 1)), std::string("%d hour ago"));
  CHECK_EQ(std::string(i18n::ntr("%d hour ago", "%d hours ago", 0)), std::string("%d hours ago"));
  CHECK(!i18n::is_rtl());

  // calling again with the same language reloads nothing: same pointers
  const char* y1 = tr("YES");
  CHECK(i18n::set_language("es_ES", o));
  CHECK(tr("YES") == y1);

  // Russian: three forms
  CHECK(i18n::set_language("ru_RU", o));
  CHECK_EQ(std::string(i18n::ntr("%d day ago", "%d days ago", 1)), std::string("%d \xd0\xb4\xd0\xb5\xd0\xbd\xd1\x8c \xd0\xbd\xd0\xb0\xd0\xb7\xd0\xb0\xd0\xb4"));
  CHECK(std::strstr(i18n::ntr("%d day ago", "%d days ago", 5), "\xd0\xb4\xd0\xbd\xd0\xb5\xd0\xb9"));   // "дней"
  CHECK(std::strstr(i18n::ntr("%d day ago", "%d days ago", 22), "\xd0\xb4\xd0\xbd\xd1\x8f"));                  // "дня"
  CHECK(std::strstr(i18n::ntr("%d day ago", "%d days ago", 21), "\xd0\xb4\xd0\xb5\xd0\xbd\xd1\x8c"));          // "день"
  // y1 (from the old catalog) is still readable: mappings are never released
  CHECK_EQ(std::string(y1), std::string("S\xc3\xad"));

  // Chinese: one form; the territory fall-back also works for a bare "zh"
  CHECK(i18n::set_language("zh_CN", o));
  CHECK_EQ(std::string(tr("SETTINGS")), std::string("\xe8\xae\xbe\xe7\xbd\xae"));
  CHECK_EQ(std::string(i18n::ntr("%d day ago", "%d days ago", 9)), std::string("%d \xe5\xa4\xa9\xe5\x89\x8d"));
  CHECK(i18n::set_language("zh", o));
  CHECK_EQ(std::string(tr("YES")), std::string("\xe6\x98\xaf"));
  CHECK_EQ(i18n::language(), std::string("zh"));

  // no catalog for this language: English and a false return
  CHECK(!i18n::set_language("xx_YY", o));
  CHECK_EQ(i18n::loaded_catalogs(), size_t(0));
  CHECK_EQ(std::string(tr("YES")), std::string("Yes"));  // untranslated all-caps keys show in sentence case (i18n_case.cpp)
  CHECK_EQ(std::string(i18n::ntr("%d day ago", "%d days ago", 2)), std::string("%d days ago"));
  // English is always fine
  CHECK(i18n::set_language("en_US", o));
  CHECK(tr("YES") != nullptr && std::string(tr("YES")) == "Yes");
  CHECK_EQ(i18n::language(), std::string(""));

  // init() reads system.language
  SystemConf conf = SystemConf::from_text("foo=bar\nsystem.language=es_ES\n");
  CHECK(i18n::init(conf, o));
  CHECK_EQ(std::string(tr("QUIT")), std::string("Salir"));
  CHECK(i18n::init(SystemConf::from_text("system.language=en_US\n"), o));
  CHECK_EQ(std::string(tr("QUIT")), std::string("Quit"));
  CHECK(i18n::init(SystemConf::from_text("nothing=here\n"), o));  // no key: English

  // RTL
  CHECK(!i18n::set_language("he", o));  // no catalog in the fixtures, but the direction is known from the code
  CHECK(i18n::is_rtl());
  CHECK(i18n::set_language("es", o));
  CHECK(!i18n::is_rtl());

  // two search directories: the first one that has the file wins; an unreadable dir is skipped
  i18n::Options two;
  two.dirs = {"/nonexistent-zf-dir", kData};
  two.domains = {"zfront"};
  CHECK(i18n::set_language("ru_RU", two));
  CHECK_EQ(std::string(tr("YES")), std::string("\xd0\x94\xd0\x90"));

  // a bare "fi" finds "fi_FI" (alias directory made of a symlink to the es fixture)
  char dir[] = "/tmp/zf-i18n-XXXXXX";
  CHECK(::mkdtemp(dir) != nullptr);
  CHECK(::symlink((kData + "/es").c_str(), (std::string(dir) + "/fi_FI").c_str()) == 0);
  i18n::Options alias;
  alias.dirs = {dir};
  alias.domains = {"zfront"};
  CHECK(i18n::set_language("fi", alias));
  CHECK_EQ(std::string(tr("QUIT")), std::string("Salir"));
  CHECK(i18n::available_languages(alias) == std::vector<std::string>{"fi_FI"});
  std::remove((std::string(dir) + "/fi_FI").c_str());
  ::rmdir(dir);

  CHECK(i18n::available_languages(i18n::Options{{kData}, {"zfront"}}) == (std::vector<std::string>{"es", "ru_RU", "zh_CN"}));
  i18n::set_language("en_US", o);
}

static std::string find_real_locale() {
  std::vector<std::string> c;
  if (const char* e = std::getenv("ZF_TEST_REAL_LOCALE")) c.push_back(e);
  c.push_back("/home/epsil/verify31/root/usr/config/locale");
  c.push_back("/usr/share/locale");
  for (const auto& d : c)
    if (file_exists(d + "/es/LC_MESSAGES/emulationstation2.mo") && file_exists(d + "/zh_CN/LC_MESSAGES/emulationstation2.mo")) return d;
  return std::string();
}

static void test_real_catalogs() {
  std::string dir = find_real_locale();
  if (dir.empty()) {
    std::printf("test_i18n: real ES catalogs not found, skipped\n");
    return;
  }
  std::printf("test_i18n: real catalogs in %s\n", dir.c_str());
  // every catalog: valid, and every entry resolves to itself (also proves the table is sorted)
  size_t files = 0, entries = 0;
  for (const auto& lang : i18n::available_languages(i18n::Options{{dir}, {"emulationstation2"}})) {
    i18n::Catalog c;
    std::string err;
    std::string path = dir + "/" + lang + "/LC_MESSAGES/emulationstation2.mo";
    if (!c.open(path, &err)) {
      std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
      CHECK(false);
      continue;
    }
    ++files;
    for (size_t i = 0; i < c.size(); ++i) {
      ++entries;
      const char* t = c.translation(i);
      if (!*t) continue;
      CHECK(c.find(c.original(i)) == t);
    }
  }
  std::printf("test_i18n: %zu real catalogs, %zu entries self-checked\n", files, entries);
  CHECK(files >= 30);

  i18n::Options o;
  o.dirs = {dir};
  o.domains = {"emulationstation2"};
  CHECK(i18n::set_language("es_ES", o));
  CHECK_EQ(std::string(tr("YES")), std::string("S\xc3\xad"));
  CHECK_EQ(std::string(tr("SETTINGS")), std::string("Ajustes"));
  CHECK_EQ(std::string(tr("QUIT")), std::string("Salir"));
  CHECK_EQ(std::string(tr("OK")), std::string("Aceptar"));
  CHECK_EQ(std::string(tr("RESTART SYSTEM")), std::string("Reiniciar sistema"));
  CHECK_EQ(std::string(i18n::ntr("%d day ago", "%d days ago", 1)), std::string("hace %d d\xc3\xad" "a"));
  CHECK_EQ(std::string(i18n::ntr("%d day ago", "%d days ago", 2)), std::string("hace %d d\xc3\xad" "as"));
  CHECK(i18n::set_language("zh_CN", o));
  CHECK_EQ(std::string(tr("YES")), std::string("\xe6\x98\xaf"));
  CHECK_EQ(std::string(tr("SETTINGS")), std::string("\xe8\xae\xbe\xe7\xbd\xae"));
  CHECK_EQ(std::string(tr("SHUTDOWN SYSTEM")), std::string("\xe5\x85\xb3\xe6\x9c\xba"));
  CHECK(i18n::set_language("ru_RU", o));
  CHECK_EQ(std::string(tr("YES")), std::string("\xd0\x94\xd0\x90"));
  const unsigned long ns[] = {1, 2, 5, 11, 21, 22};
  const char* ru_forms[] = {"\xd0\xb4\xd0\xb5\xd0\xbd\xd1\x8c", "\xd0\xb4\xd0\xbd\xd1\x8f", "\xd0\xb4\xd0\xbd\xd0\xb5\xd0\xb9", "\xd0\xb4\xd0\xbd\xd0\xb5\xd0\xb9", "\xd0\xb4\xd0\xb5\xd0\xbd\xd1\x8c", "\xd0\xb4\xd0\xbd\xd1\x8f"};
  for (size_t i = 0; i < 6; ++i) CHECK(std::strstr(i18n::ntr("%d day ago", "%d days ago", ns[i]), ru_forms[i]) != nullptr);
  // Arabic is right to left
  if (i18n::set_language("ar", o)) CHECK(i18n::is_rtl());
  CHECK(i18n::set_language("ko", o));
  CHECK(std::string(tr("SETTINGS")) != "SETTINGS");
  CHECK(i18n::set_language("pt_BR", o));
  CHECK(std::string(tr("SETTINGS")) != "SETTINGS");
  // the German catalog exists as "de": "de_DE.UTF-8" must find it
  CHECK(i18n::set_language("de_DE.UTF-8", o));
  CHECK(std::string(tr("SETTINGS")) != "SETTINGS");

  // ---- cost: load time, lookups, memory --------------------------------------------------------------------------
  const size_t rss0 = rss_kb();
  double t0 = now_ns();
  CHECK(i18n::set_language("zh_CN", o));
  double load_us = (now_ns() - t0) / 1000.0;
  const char* keys[] = {"YES", "NO", "OK", "SETTINGS", "QUIT", "RESTART SYSTEM", "SHUTDOWN SYSTEM", "GAME SETTINGS", "SOMETHING MISSING", "UI SETTINGS"};
  volatile const char* sink = nullptr;
  for (int w = 0; w < 1000; ++w) sink = tr(keys[w % 10]);  // warm: fault in the pages
  const size_t a0 = g_allocs;
  const int N = 1000000;
  t0 = now_ns();
  for (int i = 0; i < N; ++i) sink = tr(keys[i % 10]);
  double ns_per = (now_ns() - t0) / N;
  const size_t allocs = g_allocs - a0;
  (void)sink;
  const size_t rss1 = rss_kb();
  std::printf("test_i18n: zh_CN load %.0f us (%zu catalog(s), %zu bytes mapped), tr() %.0f ns/lookup, %zu allocations in %d calls, RSS +%zu kB\n", load_us,
              i18n::loaded_catalogs(), i18n::mapped_bytes(), ns_per, allocs, N, rss1 > rss0 ? rss1 - rss0 : 0);
  CHECK_EQ(allocs, size_t(0));
  CHECK(ns_per < 5000);
  i18n::set_language("en_US", o);
}

static void test_fonts() {
  const char* base = "/home/epsil/verify31/root/usr/config/emulationstation/resources";
  std::string rubik = std::string(base) + "/Rubik-Regular.ttf";
  if (const char* e = std::getenv("ZF_TEST_FONT")) rubik = e;
  if (!file_exists(rubik) || !file_exists(path_join(path_dir(rubik), "DroidSansFallbackFull.ttf"))) {
    std::printf("test_i18n: Rubik/DroidSansFallbackFull not found, font checks skipped\n");
    return;
  }
  Font f;
  std::string err;
  CHECK(f.load(rubik, err));
  // Latin text never opens a fallback
  Img a = f.render("Hello, Zenith ñ é", 24, {255, 255, 255, 255});
  CHECK(a.w > 10);
  CHECK_EQ(f.fallbacks_open(), 0);
  float wl = f.measure("Hello", 24);
  CHECK(wl > 20);
  // Chinese: Rubik has no glyph -> the fallback is opened on demand and measure() agrees with render()
  const std::string zh = "\xe8\xae\xbe\xe7\xbd\xae";  // 设置
  float w = f.measure(zh, 24);
  CHECK(f.fallbacks_open() >= 1);
  CHECK(w > 30);
  Img r = f.render(zh, 24, {255, 255, 255, 255});
  CHECK_EQ(r.w, uint32_t(std::ceil(w)) + 1);
  size_t lit = 0;
  for (size_t i = 3; i < r.rgba.size(); i += 4) lit += r.rgba[i] > 0;
  CHECK(lit > 100);  // real glyphs, not an empty box
  // Hangul (needs PyeojinGothic) and mixed text
  const std::string ko = "\xec\x84\xa4\xec\xa0\x95";  // 설정
  Img k = f.render(ko, 24, {255, 255, 255, 255});
  CHECK(k.w > 30);
  Img mix = f.render("OK " + zh, 24, {255, 255, 255, 255});
  CHECK(mix.w > k.w / 2);
  CHECK_EQ(mix.h, a.h);  // the line box stays the Latin one
  std::printf("test_i18n: fallback fonts opened: %d\n", f.fallbacks_open());
}

int main() {
  test_plural_expr();
  test_candidates();
  test_mo_files();
  test_language_selection();
  test_real_catalogs();
  test_fonts();
  return zt::done();
}
