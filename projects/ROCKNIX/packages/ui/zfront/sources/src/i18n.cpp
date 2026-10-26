#include "i18n.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "services.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace zf {
namespace i18n {

// ---- Plural-Forms ---------------------------------------------------------------------------------------------------
namespace {

struct PluralParser {
  std::string_view s;
  size_t i = 0;
  bool ok = true;

  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  bool eat(const char* t) {
    ws();
    size_t n = std::strlen(t);
    if (s.compare(i, n, t) == 0) {
      i += n;
      return true;
    }
    return false;
  }
};

}  // namespace

bool Plural::parse(std::string_view v) {
  nodes_.clear();
  root_ = -1;
  nplurals_ = 2;
  auto fallback = [this]() {
    nodes_.clear();
    nodes_.push_back({'n', 0, -1, -1, -1});
    nodes_.push_back({'k', 1, -1, -1, -1});
    nodes_.push_back({'~', 0, 0, 1, -1});
    root_ = 2;
    nplurals_ = 2;
    return false;
  };

  PluralParser p;
  p.s = v;
  if (!p.eat("nplurals")) return fallback();
  if (!p.eat("=")) return fallback();
  p.ws();
  unsigned np = 0;
  size_t digits = 0;
  while (p.i < v.size() && v[p.i] >= '0' && v[p.i] <= '9' && digits < 3) np = np * 10 + unsigned(v[p.i++] - '0'), ++digits;
  if (digits == 0 || np < 1 || np > 32) return fallback();
  if (!p.eat(";")) return fallback();
  if (!p.eat("plural")) return fallback();
  if (!p.eat("=")) return fallback();

  // recursive descent over the C operators (no assignment / comma), lowest precedence first; nodes go to `nodes_`
  struct R {
    PluralParser& p;
    std::vector<Node>& nodes;
    int mk(char op, unsigned long k, int a, int b, int c) {
      if (nodes.size() >= 256) {
        p.ok = false;
        return 0;
      }
      nodes.push_back({op, k, a, b, c});
      return int(nodes.size()) - 1;
    }
    int ternary(int depth) {
      int cnd = orx(depth);
      if (p.eat("?")) {
        if (depth > 32) return p.ok = false, 0;
        int a = ternary(depth + 1);
        if (!p.eat(":")) return p.ok = false, 0;
        int b = ternary(depth + 1);
        return mk('?', 0, cnd, a, b);
      }
      return cnd;
    }
    int orx(int d) {
      int l = andx(d);
      while (p.ok && p.eat("||")) l = mk('|', 0, l, andx(d), -1);
      return l;
    }
    int andx(int d) {
      int l = eqx(d);
      while (p.ok && p.eat("&&")) l = mk('&', 0, l, eqx(d), -1);
      return l;
    }
    int eqx(int d) {
      int l = relx(d);
      for (;;) {
        if (!p.ok) return l;
        if (p.eat("==")) l = mk('=', 0, l, relx(d), -1);
        else if (p.eat("!=")) l = mk('~', 0, l, relx(d), -1);
        else return l;
      }
    }
    int relx(int d) {
      int l = addx(d);
      for (;;) {
        if (!p.ok) return l;
        if (p.eat("<=")) l = mk('l', 0, l, addx(d), -1);
        else if (p.eat(">=")) l = mk('g', 0, l, addx(d), -1);
        else if (p.eat("<")) l = mk('<', 0, l, addx(d), -1);
        else if (p.eat(">")) l = mk('>', 0, l, addx(d), -1);
        else return l;
      }
    }
    int addx(int d) {
      int l = mulx(d);
      for (;;) {
        if (!p.ok) return l;
        if (p.eat("+")) l = mk('+', 0, l, mulx(d), -1);
        else if (p.eat("-")) l = mk('-', 0, l, mulx(d), -1);
        else return l;
      }
    }
    int mulx(int d) {
      int l = unary(d);
      for (;;) {
        if (!p.ok) return l;
        if (p.eat("*")) l = mk('*', 0, l, unary(d), -1);
        else if (p.eat("/")) l = mk('/', 0, l, unary(d), -1);
        else if (p.eat("%")) l = mk('%', 0, l, unary(d), -1);
        else return l;
      }
    }
    int unary(int d) {
      p.ws();
      if (p.i < p.s.size() && p.s[p.i] == '!' && !(p.i + 1 < p.s.size() && p.s[p.i + 1] == '=')) {
        ++p.i;
        if (d > 32) return p.ok = false, 0;
        return mk('!', 0, unary(d + 1), -1, -1);
      }
      return primary(d);
    }
    int primary(int d) {
      p.ws();
      if (p.i >= p.s.size()) return p.ok = false, 0;
      char c = p.s[p.i];
      if (c == 'n') {
        ++p.i;
        return mk('n', 0, -1, -1, -1);
      }
      if (c >= '0' && c <= '9') {
        unsigned long k = 0;
        size_t nd = 0;
        while (p.i < p.s.size() && p.s[p.i] >= '0' && p.s[p.i] <= '9' && nd < 9) k = k * 10 + unsigned(p.s[p.i++] - '0'), ++nd;
        return mk('k', k, -1, -1, -1);
      }
      if (c == '(') {
        ++p.i;
        if (d > 32) return p.ok = false, 0;
        int e = ternary(d + 1);
        if (!p.eat(")")) return p.ok = false, 0;
        return e;
      }
      return p.ok = false, 0;
    }
  };
  R r{p, nodes_};
  int root = r.ternary(0);
  p.eat(";");
  p.ws();
  // anything but whitespace after the expression is a parse error
  if (!p.ok || root < 0 || p.i < v.size()) return fallback();
  root_ = root;
  nplurals_ = np;
  return true;
}

unsigned long Plural::run(int node, unsigned long n, int depth) const {
  if (node < 0 || depth > 64) return 0;
  const Node& x = nodes_[size_t(node)];
  switch (x.op) {
    case 'n': return n;
    case 'k': return x.k;
    case '!': return !run(x.a, n, depth + 1);
    case '?': return run(x.a, n, depth + 1) ? run(x.b, n, depth + 1) : run(x.c, n, depth + 1);
    case '|': return run(x.a, n, depth + 1) || run(x.b, n, depth + 1);
    case '&': return run(x.a, n, depth + 1) && run(x.b, n, depth + 1);
    default: break;
  }
  unsigned long a = run(x.a, n, depth + 1), b = run(x.b, n, depth + 1);
  switch (x.op) {
    case '=': return a == b;
    case '~': return a != b;
    case '<': return a < b;
    case '>': return a > b;
    case 'l': return a <= b;
    case 'g': return a >= b;
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b ? a / b : 0;
    case '%': return b ? a % b : 0;
    default: return 0;
  }
}

unsigned Plural::eval(unsigned long n) const {
  if (root_ < 0) return n != 1;
  unsigned long v = run(root_, n, 0);
  return v >= nplurals_ ? nplurals_ - 1 : unsigned(v);
}

// ---- .mo catalog ----------------------------------------------------------------------------------------------------
uint32_t Catalog::rd(const uint8_t* p) const {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return swap_ ? __builtin_bswap32(v) : v;
}

bool Catalog::read_entry(const uint8_t* table, size_t i, uint32_t& len, uint32_t& off) const {
  len = rd(table + 8 * i);
  off = rd(table + 8 * i + 4);
  return uint64_t(off) + len < file_size_ && base_[off + len] == 0;  // the NUL terminator is inside the file
}

bool Catalog::load_header() {
  if (file_size_ < 28) return false;
  uint32_t magic;
  std::memcpy(&magic, base_, 4);
  if (magic == 0x950412deu) swap_ = false;
  else if (magic == 0xde120495u) swap_ = true;
  else return false;
  uint32_t rev = rd(base_ + 4);
  if ((rev >> 16) > 1) return false;
  count_ = rd(base_ + 8);
  orig_off_ = rd(base_ + 12);
  trans_off_ = rd(base_ + 16);
  if (count_ > (1u << 22)) return false;
  if (uint64_t(orig_off_) + 8ull * count_ > file_size_ || uint64_t(trans_off_) + 8ull * count_ > file_size_) return false;
  // originals: inside the file, NUL terminated and strictly sorted (what the binary search relies on). Translations
  // are only checked when used, so a lookup does not fault in every page of the file at load.
  const char* prev = nullptr;
  for (size_t i = 0; i < count_; ++i) {
    uint32_t ol, oo;
    if (!read_entry(base_ + orig_off_, i, ol, oo)) return false;
    const char* cur = reinterpret_cast<const char*>(base_) + oo;
    if (prev && std::strcmp(prev, cur) >= 0) return false;
    prev = cur;
  }
  // a file cut short loses its tail, which is where the last translations live: check the ends of the table
  if (count_) {
    uint32_t tl, to;
    if (!read_entry(base_ + trans_off_, 0, tl, to) || !read_entry(base_ + trans_off_, count_ - 1, tl, to)) return false;
  }
  // plural rule from the header entry (msgid "")
  plural_.parse("nplurals=2; plural=n != 1;");
  if (const char* hdr = find("")) {
    const char* pf = std::strstr(hdr, "Plural-Forms:");
    if (pf) {
      pf += 13;
      const char* end = std::strchr(pf, '\n');
      std::string_view v(pf, end ? size_t(end - pf) : std::strlen(pf));
      plural_.parse(trim(v));
    }
  }
  return true;
}

bool Catalog::open_memory(const void* data, size_t size, std::string* err) {
  *this = Catalog();
  base_ = static_cast<const uint8_t*>(data);
  file_size_ = size;
  bytes_ = size;
  if (!load_header()) {
    *this = Catalog();
    if (err) *err = "not a valid sorted .mo catalog";
    return false;
  }
  return true;
}

bool Catalog::open(const std::string& path, std::string* err) {
  *this = Catalog();
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (err) *err = path + ": cannot open";
    return false;
  }
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 28 || st.st_size > (64 << 20)) {
    ::close(fd);
    if (err) *err = path + ": not a catalog";
    return false;
  }
  void* m = ::mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (m == MAP_FAILED) {
    if (err) *err = path + ": mmap failed";
    return false;
  }
  if (!open_memory(m, size_t(st.st_size), err)) {
    ::munmap(m, size_t(st.st_size));
    if (err) *err = path + ": " + *err;
    return false;
  }
  return true;  // the mapping is kept for the life of the process on purpose (see i18n.hpp)
}

const char* Catalog::find_len(const char* key, uint32_t* total_len) const {
  uint32_t lo = 0, hi = count_;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    uint32_t oo = rd(base_ + orig_off_ + 8ull * mid + 4);
    int c = std::strcmp(key, reinterpret_cast<const char*>(base_) + oo);
    if (c == 0) {
      uint32_t tl = rd(base_ + trans_off_ + 8ull * mid);
      uint32_t to = rd(base_ + trans_off_ + 8ull * mid + 4);
      if (tl == 0 || uint64_t(to) + tl >= file_size_ || base_[to + tl] != 0) return nullptr;  // empty or corrupt
      const char* t = reinterpret_cast<const char*>(base_) + to;
      if (*t == 0) return nullptr;  // empty msgstr = untranslated
      if (total_len) *total_len = tl;
      return t;
    }
    if (c < 0) hi = mid;
    else lo = mid + 1;
  }
  return nullptr;
}

const char* Catalog::find_form(const char* key, unsigned form) const {
  uint32_t tl = 0;
  const char* t = find_len(key, &tl);
  if (!t) return nullptr;
  const char* p = t;
  for (unsigned k = 0; k < form; ++k) {
    p += std::strlen(p) + 1;
    if (size_t(p - t) > tl) return nullptr;
  }
  return *p ? p : nullptr;
}

const char* Catalog::original(size_t i) const {
  if (i >= count_) return nullptr;
  return reinterpret_cast<const char*>(base_) + rd(base_ + orig_off_ + 8 * i + 4);
}
const char* Catalog::translation(size_t i) const {
  if (i >= count_) return nullptr;
  uint32_t tl = rd(base_ + trans_off_ + 8 * i), to = rd(base_ + trans_off_ + 8 * i + 4);
  if (uint64_t(to) + tl >= file_size_ || base_[to + tl] != 0) return "";
  return reinterpret_cast<const char*>(base_) + to;
}

// ---- language selection ---------------------------------------------------------------------------------------------
namespace {

struct Bundle {
  std::vector<Catalog> cats;
  std::string lang;  // normalised code requested ("" = English)
  std::string sig;   // language + options, to make init() idempotent
};
// Bundles are never freed: strings returned by tr() point into their mappings.
Bundle* g_cur = nullptr;
unsigned g_generation = 0;

// tr(): the translation (or the English key) in sentence case when it comes in all capitals; mixed case stays as written.
const char* lookup(const char* s) {
  if (!g_cur || !s || !*s) return nullptr;
  for (const Catalog& c : g_cur->cats)
    if (const char* t = c.find(s)) return display_text(t);  // catalogs of ES also use capitals ("AJUSTES")
  return display_text(s);
}

std::string normalise(std::string_view lang) {
  std::string l(trim(lang));
  size_t cut = l.find_first_of(".@");
  if (cut != std::string::npos) l.resize(cut);
  std::replace(l.begin(), l.end(), '-', '_');
  return l;
}

std::string mo_path(const std::string& dir, const std::string& lang, const std::string& domain) {
  return dir + "/" + lang + "/LC_MESSAGES/" + domain + ".mo";
}

/// First existing `<dir>/<cand>/LC_MESSAGES/<domain>.mo`; as a last resort `xx` also finds `xx_YY` (fi -> fi_FI).
std::string locate(const std::string& dir, const std::vector<std::string>& cands, const std::string& domain) {
  for (const auto& c : cands) {
    std::string p = mo_path(dir, c, domain);
    if (file_exists(p)) return p;
  }
  if (cands.empty()) return std::string();
  const std::string& shortc = cands.back();
  if (shortc.find('_') != std::string::npos) return std::string();
  std::vector<std::string> alt;
  if (DIR* d = ::opendir(dir.c_str())) {
    std::string prefix = shortc + "_";
    while (dirent* e = ::readdir(d))
      if (std::strncmp(e->d_name, prefix.c_str(), prefix.size()) == 0 && !std::strchr(e->d_name + prefix.size(), '.')) alt.push_back(e->d_name);
    ::closedir(d);
  }
  std::sort(alt.begin(), alt.end());
  for (const auto& a : alt) {
    std::string p = mo_path(dir, a, domain);
    if (file_exists(p)) return p;
  }
  return std::string();
}

std::string signature(const std::string& lang, const Options& o) {
  std::string s = lang;
  s += '|';
  for (const auto& d : o.dirs) s += d + ':';
  s += '|';
  for (const auto& d : o.domains) s += d + ':';
  return s;
}

Options g_last_opts;

}  // namespace

std::vector<std::string> candidates(std::string_view lang) {
  std::string l = normalise(lang);
  std::vector<std::string> v;
  if (l.empty() || l == "C" || l == "POSIX" || l == "en_US" || l == "en") return v;
  v.push_back(l);
  size_t us = l.find('_');
  if (us != std::string::npos && us > 0) v.push_back(l.substr(0, us));
  return v;
}

Options default_options() {
  Options o;
  if (const char* e = std::getenv("ZF_LOCALE_DIR")) {
    std::string s = e;
    size_t a = 0;
    while (a <= s.size()) {
      size_t b = s.find(':', a);
      if (b == std::string::npos) b = s.size();
      if (b > a) o.dirs.push_back(s.substr(a, b - a));
      a = b + 1;
    }
  }
  // ES binds /usr/share/locale (-> /storage/.config/locale, a copy of /usr/config/locale made at install)
  for (const char* d : {"/usr/share/locale", "/storage/.config/locale", "/usr/config/locale", "/usr/share/zfront/locale"}) o.dirs.push_back(d);
  o.domains = {"zfront", "emulationstation2"};
  return o;
}

std::vector<std::string> available_languages(const Options& opts) {
  std::string domain = opts.domains.empty() ? "emulationstation2" : opts.domains.back();
  std::vector<std::string> out;
  for (const auto& dir : opts.dirs) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) continue;
    while (dirent* e = ::readdir(d)) {
      if (e->d_name[0] == '.') continue;
      if (file_exists(mo_path(dir, e->d_name, domain))) out.emplace_back(e->d_name);
    }
    ::closedir(d);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool set_language(std::string_view lang, const Options& opts) {
  std::vector<std::string> cands = candidates(lang);
  std::string code = cands.empty() ? std::string() : normalise(lang);
  std::string sig = signature(code, opts);
  if (g_cur && g_cur->sig == sig) return cands.empty() || !g_cur->cats.empty();
  g_last_opts = opts;

  Bundle* b = new Bundle();
  b->lang = code;
  b->sig = sig;
  for (const auto& domain : opts.domains) {
    for (const auto& dir : opts.dirs) {
      std::string p = locate(dir, cands, domain);
      if (p.empty()) continue;
      Catalog c;
      if (c.open(p)) {
        b->cats.push_back(c);
        break;
      }
    }
  }
  // The previous bundle stays alive on purpose (a screen may still hold a pointer into its mapping); keeping it in a
  // list, not just dropping the pointer, makes that visible to leak checkers.
  static auto* retired = new std::vector<Bundle*>();  // never destroyed: still reachable when the leak check runs at exit
  if (g_cur) retired->push_back(g_cur);
  g_cur = b;
  ++g_generation;
  set_translator(&lookup);  // also without catalogs: English keys still get their sentence-case form
  return cands.empty() || !b->cats.empty();
}

bool init(const SystemConf& conf, const Options& opts) { return set_language(conf.get_or("system.language", ""), opts); }

unsigned generation() { return g_generation; }

const std::string& language() {
  static const std::string none;
  return g_cur ? g_cur->lang : none;
}
size_t loaded_catalogs() { return g_cur ? g_cur->cats.size() : 0; }
size_t mapped_bytes() {
  size_t n = 0;
  if (g_cur)
    for (const Catalog& c : g_cur->cats) n += c.mapped_bytes();
  return n;
}
bool is_rtl() {
  const std::string& l = language();
  return l == "ar" || l == "he" || starts_with(l, "ar_") || starts_with(l, "he_");
}

const char* ntr(const char* singular, const char* plural, unsigned long n) {
  if (g_cur)
    for (const Catalog& c : g_cur->cats)
      if (const char* t = c.find_form(singular, c.plural().eval(n))) return t;
  return display_text(n != 1 ? plural : singular);
}

const char* ctr(const char* context, const char* text) {
  if (!g_cur) return text;
  char buf[256];
  size_t cl = std::strlen(context), tl = std::strlen(text);
  std::string big;
  const char* key;
  if (cl + 1 + tl + 1 <= sizeof buf) {
    std::memcpy(buf, context, cl);
    buf[cl] = '\4';
    std::memcpy(buf + cl + 1, text, tl + 1);
    key = buf;
  } else {
    big.reserve(cl + tl + 2);
    big.append(context, cl).push_back('\4');
    big.append(text, tl);
    key = big.c_str();
  }
  for (const Catalog& c : g_cur->cats)
    if (const char* t = c.find(key)) return t;
  return display_text(text);
}

}  // namespace i18n
}  // namespace zf
