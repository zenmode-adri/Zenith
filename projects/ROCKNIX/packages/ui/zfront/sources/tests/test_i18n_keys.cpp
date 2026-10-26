// Every text the code can show must have a Spanish translation: either in zfront's own catalog (locale/es/LC_MESSAGES/zfront.po,
// built to <build>/locale/es/LC_MESSAGES/zfront.mo) or in ES's (the keys listed in locale/es-inherited.txt). The keys are found by scanning
// src/ for tr("..."), ntr("..", ".."), ctr("ctx", "..") and the markers N_("..") / NC_("ctx", "..") (what tools/extract-strings.sh sees),
// so a new tr("NEW TEXT") without a translation fails here. Also: no placeholder is lost in a translation, zfront.po does not repeat
// what ES already translates and has no entry the code no longer uses (the two checks that need ES's real catalogs are skipped without them),
// the help bars of the carousel and the list follow the system language, and tr() with both catalogs loaded does not allocate.
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "alloc_nothrow.hpp"
#include "check.hpp"
#include "i18n.hpp"
#include "scene.hpp"
#include "services.hpp"
#include "util.hpp"

using namespace zf;

static size_t g_allocs = 0;
void* operator new(size_t n) {
  ++g_allocs;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

using Key = std::pair<std::string, std::string>;  // (context, msgid)

// ---- scan the sources for translatable literals ---------------------------------------------------------------------------
namespace {
struct Scan {
  const std::string& s;
  size_t i = 0;
  explicit Scan(const std::string& text) : s(text) {}

  bool at(const char* lit) const { return i <= s.size() && s.compare(i, std::strlen(lit), lit) == 0; }
  bool ident_char(char c) const { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
  void skip_ws() {
    for (;;) {
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
      if (at("//")) {
        while (i < s.size() && s[i] != '\n') ++i;
      } else if (at("/*")) {
        size_t e = s.find("*/", i + 2);
        i = e == std::string::npos ? s.size() : e + 2;
      } else {
        return;
      }
    }
  }
  // one string literal or several adjacent ones ("a" "b"); false when the next token is not a literal
  bool literal(std::string& out) {
    skip_ws();
    if (i >= s.size() || s[i] != '"') return false;
    out.clear();
    while (i < s.size() && s[i] == '"') {
      ++i;
      while (i < s.size() && s[i] != '"') {
        char c = s[i++];
        if (c != '\\') {
          out += c;
          continue;
        }
        char e = s[i++];
        switch (e) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'x': {
            int v = 0, n = 0;
            while (n < 2 && i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i]))) {
              char h = s[i++];
              v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
              ++n;
            }
            out += char(v);
            break;
          }
          default: out += e; break;  // \" \\ \'
        }
      }
      ++i;  // closing quote
      skip_ws();
    }
    return true;
  }
  bool comma() {
    skip_ws();
    if (i < s.size() && s[i] == ',') {
      ++i;
      return true;
    }
    return false;
  }
};

void scan_text(const std::string& text, std::set<Key>& keys) {
  Scan sc(text);
  const std::string& s = text;
  while (sc.i < s.size()) {
    char c = s[sc.i];
    if (c == '/' && (sc.at("//") || sc.at("/*"))) {
      sc.skip_ws();
      continue;
    }
    if (c == '"') {  // a literal that is not an argument of a marker: skip it whole
      std::string tmp;
      sc.literal(tmp);
      continue;
    }
    if (c == '\'') {  // char literal
      ++sc.i;
      while (sc.i < s.size() && s[sc.i] != '\'') sc.i += s[sc.i] == '\\' ? 2 : 1;
      ++sc.i;
      continue;
    }
    if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_')) {
      ++sc.i;
      continue;
    }
    size_t b = sc.i;
    while (sc.i < s.size() && sc.ident_char(s[sc.i])) ++sc.i;
    std::string id = s.substr(b, sc.i - b);
    bool simple = id == "tr" || id == "N_" || id == "ntr", ctx = id == "ctr" || id == "NC_";
    if (!simple && !ctx) continue;
    sc.skip_ws();
    if (sc.i >= s.size() || s[sc.i] != '(') continue;
    ++sc.i;
    std::string a, b2;
    if (!sc.literal(a)) continue;  // a definition or a call with a variable
    if (ctx) {
      if (sc.comma() && sc.literal(b2)) keys.insert({a, b2});
    } else {
      keys.insert({"", a});
    }
  }
}

std::string join_key(const Key& k) { return k.first.empty() ? k.second : k.first + "\x04" + k.second; }

std::vector<std::string> list_dir(const std::string& dir) {
  std::vector<std::string> v;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d)) v.push_back(e->d_name);
    ::closedir(d);
  }
  std::sort(v.begin(), v.end());
  return v;
}

std::string unescape_key(const std::string& line) {  // es-inherited.txt: `ctx|msgid`, `\n` and `\\` escaped
  std::string o;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size()) {
      ++i;
      o += line[i] == 'n' ? '\n' : line[i];
    } else {
      o += line[i];
    }
  }
  return o;
}

std::vector<std::string> placeholders(const char* s) {
  std::vector<std::string> v;
  for (const char* p = s; *p; ++p)
    if (*p == '%' && p[1]) {
      v.push_back(std::string(p, 2));
      ++p;
    }
  return v;
}

std::string find_real_locale() {
  std::vector<std::string> c;
  if (const char* e = std::getenv("ZF_TEST_REAL_LOCALE")) c.push_back(e);
  c.push_back("/home/epsil/verify31/root/usr/config/locale");
  c.push_back("/usr/share/locale");
  c.push_back("/usr/config/locale");
  for (const std::string& d : c)
    if (file_exists(d + "/es/LC_MESSAGES/emulationstation2.mo")) return d;
  return std::string();
}

double now_ns() { return double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
}  // namespace

int main() {
  const std::string src = std::string(ZF_SRC_DIR) + "/src";
  const std::string build = ZF_LOCALE_BUILD;
  std::string real = find_real_locale();

  // ---- keys of the code
  std::set<Key> keys;
  size_t files = 0;
  for (const std::string& f : list_dir(src)) {
    if (!ends_with(f, ".cpp") && !ends_with(f, ".hpp")) continue;
    std::string text;
    CHECK(read_file(src + "/" + f, text));
    scan_text(text, keys);
    ++files;
  }
  std::printf("test_i18n_keys: %zu keys in %zu source files\n", keys.size(), files);
  CHECK(files > 50);
  CHECK(keys.size() > 500);
  CHECK(keys.count({"", "QUIT"}));
  CHECK(keys.count({"joystick", "D-PAD UP"}));        // NC_
  CHECK(keys.count({"", "CHOOSE"}));                  // N_ in a Prompt table
  CHECK(keys.count({"", "ARE YOU SURE YOU WANT TO FORGET '%s' ?"}));  // N_ passed to a helper
  CHECK(keys.count({"", "%i GAME"}));                 // ntr

  // ---- Spanish: zfront's own catalog + what ES translates
  i18n::Catalog zf;
  std::string err;
  CHECK(zf.open(build + "/es/LC_MESSAGES/zfront.mo", &err));
  std::set<std::string> inherited;
  {
    std::string text;
    CHECK(read_file(std::string(ZF_SRC_DIR) + "/locale/es-inherited.txt", text));
    size_t a = 0;
    while (a < text.size()) {
      size_t b = text.find('\n', a);
      if (b == std::string::npos) b = text.size();
      if (b > a) {
        std::string l = text.substr(a, b - a), ctx;
        size_t bar = l.find('|');  // a context is lower-case letters and underscores (joystick, game_options)
        if (bar != std::string::npos && bar > 0 && l.find_first_not_of("abcdefghijklmnopqrstuvwxyz_") == bar) {
          ctx = l.substr(0, bar);
          l = l.substr(bar + 1);
        }
        inherited.insert(join_key({ctx, unescape_key(l)}));
      }
      a = b + 1;
    }
  }
  std::vector<std::string> untranslated;
  for (const Key& k : keys) {
    std::string j = join_key(k);
    if (!zf.find(j.c_str()) && !inherited.count(j)) untranslated.push_back(k.first + (k.first.empty() ? "" : "|") + k.second);
  }
  for (const std::string& u : untranslated) std::fprintf(stderr, "test_i18n_keys: no Spanish translation for \"%s\" (run tools/update-locale.py)\n", u.c_str());
  CHECK(untranslated.empty());

  // zfront.po: placeholders kept, no empty text, every entry still used by the code
  size_t unused = 0;
  std::set<std::string> used;
  for (const Key& k : keys) used.insert(join_key(k));
  for (size_t n = 0; n < zf.size(); ++n) {
    const char* o = zf.original(n);
    const char* t = zf.translation(n);
    if (!*o) continue;  // header
    if (!*t) {
      std::fprintf(stderr, "test_i18n_keys: empty translation of \"%s\"\n", o);
      CHECK(false);
      continue;
    }
    const char* key = std::strchr(o, '\x04') ? std::strchr(o, '\x04') + 1 : o;
    if (placeholders(key) != placeholders(t)) {
      std::fprintf(stderr, "test_i18n_keys: placeholders of \"%s\" differ from \"%s\"\n", o, t);
      CHECK(false);
    }
    if (std::count(key, key + std::strlen(key), '\n') != std::count(t, t + std::strlen(t), '\n')) {
      std::fprintf(stderr, "test_i18n_keys: line breaks of \"%s\" differ from the translation\n", o);
      CHECK(false);
    }
    if (!used.count(o)) {
      std::fprintf(stderr, "test_i18n_keys: zfront.po entry \"%s\" is not used by the code any more\n", o);
      ++unused;
    }
  }
  CHECK_EQ(unused, size_t(0));

  // ---- consistency with ES's real Spanish catalog
  i18n::Options both;
  both.dirs = {build};
  if (!real.empty()) both.dirs.push_back(real);
  both.domains = {"zfront", "emulationstation2"};
  if (real.empty()) {
    std::printf("test_i18n_keys: ES catalogs not found: inheritance checks skipped\n");
  } else {
    i18n::Catalog es;
    CHECK(es.open(real + "/es/LC_MESSAGES/emulationstation2.mo", &err));
    size_t stale = 0, shadow = 0;
    for (const std::string& j : inherited)
      if (!es.find(j.c_str())) {
        std::fprintf(stderr, "test_i18n_keys: es-inherited.txt lists \"%s\" but ES does not translate it (run tools/update-locale.py)\n", j.c_str());
        ++stale;
      }
    for (size_t n = 0; n < zf.size(); ++n)
      if (*zf.original(n) && es.find(zf.original(n))) {
        std::fprintf(stderr, "test_i18n_keys: zfront.po repeats \"%s\", which ES already translates\n", zf.original(n));
        ++shadow;
      }
    CHECK_EQ(stale, size_t(0));
    CHECK_EQ(shadow, size_t(0));
  }

  // ---- behaviour with the catalogs loaded
  unsigned g0 = i18n::generation();
  CHECK(i18n::set_language("es_ES", both));
  CHECK(i18n::generation() != g0);
  CHECK_EQ(i18n::loaded_catalogs(), real.empty() ? size_t(1) : size_t(2));
  CHECK_EQ(std::string(tr("Could not save the setting")), std::string("No se pudo guardar el ajuste"));
  CHECK_EQ(std::string(tr("Action")), std::string("Acci\xc3\xb3n"));
  CHECK_EQ(std::string(tr("ENABLE SSH")), std::string("Activar SSH"));
  CHECK_EQ(std::string(i18n::ctr("joystick", "LEFT THUMB")), std::string("Stick izquierdo (pulsar)"));
  CHECK_EQ(std::string(i18n::ctr("joystick", "UNKNOWN BUTTON")), std::string("Unknown button"));
  CHECK(i18n::set_language("es_MX.UTF-8", both));  // es_MX: ES has its own catalog, zfront's one is the `es` folder
  CHECK_EQ(std::string(tr("Could not save the setting")), std::string("No se pudo guardar el ajuste"));
  CHECK(i18n::set_language("es_ES", both));
  if (!real.empty()) {
    CHECK_EQ(std::string(tr("QUIT")), std::string("Salir"));
    CHECK_EQ(std::string(tr("all games")), std::string("todos los juegos"));
    CHECK_EQ(std::string(i18n::ctr("joystick", "D-PAD UP")), std::string("D-PAD ARRIBA"));
    CHECK_EQ(std::string(i18n::ntr("%i GAME", "%i GAMES", 1)), std::string("%i JUEGO"));
    CHECK_EQ(std::string(i18n::ntr("%i GAME", "%i GAMES", 14)), std::string("%i JUEGOS"));
    // the carousel and list help bars (capitals, as before) follow the language and come back when it changes
    const HelpPrompts& sh = system_help();
    CHECK_EQ(sh.size(), size_t(5));
    CHECK_EQ(std::string(sh[0].second), std::string("MEN\xc3\x9a"));
    CHECK_EQ(std::string(sh[1].second), std::string("BARRA DE NAVEGACI\xc3\x93N"));
    CHECK_EQ(std::string(sh[2].second), std::string("ALEATORIO"));
    const HelpPrompts& lh = list_help();
    CHECK_EQ(lh.size(), size_t(6));
    CHECK_EQ(std::string(lh[4].second), std::string("OPCIONES DE JUEGO"));
    CHECK_EQ(std::string(lh[5].second), std::string("INICIAR"));
  }
  CHECK(i18n::set_language("en_US", both));
  {
    const HelpPrompts& sh = system_help();
    CHECK_EQ(std::string(sh[0].second), std::string("MENU"));
    CHECK_EQ(std::string(sh[1].second), std::string("NAVIGATION BAR"));
    CHECK_EQ(std::string(sh[4].second), std::string("CHOOSE"));
    CHECK_EQ(std::string(list_help()[5].second), std::string("LAUNCH"));
  }
  // another language: zfront has no catalog for it, its keys show in English (sentence case), ES's still translate
  if (!real.empty() && i18n::set_language("fr", both)) {
    CHECK_EQ(std::string(tr("ENABLE SSH")), std::string("Enable SSH"));
    CHECK(std::string(tr("QUIT")) != "Quit");
  }

  // ---- cost: tr() with zfront's catalog first, then ES's: 0 allocations, time next to the ES-only lookup
  const char* mix[] = {"YES", "SETTINGS", "Could not save the setting", "QUIT", "ENABLE SSH", "GAME SETTINGS", "NO SUCH KEY", "WI-FI NETWORKS", "Wi-Fi is on", "SOUND SETTINGS"};
  auto bench = [&](const i18n::Options& o, const char* what) {
    CHECK(i18n::set_language("es_ES", o));
    volatile const char* sink = nullptr;
    for (int w = 0; w < 1000; ++w) sink = tr(mix[w % 10]);
    const size_t a0 = g_allocs;
    const int N = 1000000;
    double t0 = now_ns();
    for (int i = 0; i < N; ++i) sink = tr(mix[i % 10]);
    double ns = (now_ns() - t0) / N;
    size_t allocs = g_allocs - a0;
    (void)sink;
    std::printf("test_i18n_keys: %s: tr() %.0f ns/lookup, %zu allocations in %d calls\n", what, ns, allocs, N);
    CHECK_EQ(allocs, size_t(0));
    CHECK(ns < 5000);
    return ns;
  };
  if (!real.empty()) {
    i18n::Options esonly;
    esonly.dirs = {real};
    esonly.domains = {"emulationstation2"};
    double a = bench(esonly, "ES catalog only      ");
    double b = bench(both, "zfront + ES catalogs ");
    std::printf("test_i18n_keys: cost of the second catalog: %+.0f ns per lookup\n", b - a);
  } else {
    bench(both, "zfront catalog       ");
  }
  i18n::set_language("en_US", both);
  return zt::done();
}
