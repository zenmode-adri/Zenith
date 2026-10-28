#include "util.hpp"

#include <atomic>
#include <charconv>
#include <cstdio>
#include <sys/stat.h>

namespace zf {

static std::atomic<bool> g_writes_blocked{false};
bool writes_blocked() { return g_writes_blocked.load(std::memory_order_relaxed); }
void set_writes_blocked(bool blocked) { g_writes_blocked.store(blocked, std::memory_order_relaxed); }

bool read_file(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  // fopen("rb") also opens a directory, and ftell on one gives LONG_MAX: resize() then aborts the process (the build has
  // no exceptions). Only regular files of a sane size are read; /sys and /proc attributes are regular files too.
  struct stat st;
  if (::fstat(::fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
    std::fclose(f);
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.clear();
  if (n > static_cast<long>(kMaxReadFile)) {
    std::fclose(f);
    return false;
  }
  if (n > 0) {
    out.resize(static_cast<size_t>(n));
    size_t got = std::fread(out.data(), 1, out.size(), f);
    out.resize(got);
  }
  std::fclose(f);
  return true;
}

bool read_file_range(const std::string& path, uint64_t offset, size_t size, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  bool ok = std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0;
  out.resize(size);
  if (ok) ok = std::fread(out.data(), 1, size, f) == size;
  std::fclose(f);
  return ok;
}

bool file_exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

bool is_dir(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool parse_float(std::string_view s, float& out) {
  s = trim(s);
  if (s.empty()) return false;
  if (s.front() == '+') s.remove_prefix(1);
  float v = 0;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
  out = v;
  return true;
}

bool parse_int(std::string_view s, long& out) {
  s = trim(s);
  if (s.empty()) return false;
  if (s.front() == '+') s.remove_prefix(1);
  long v = 0;
  auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return false;
  out = v;
  return true;
}

std::string_view trim(std::string_view s) {
  auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
  while (!s.empty() && sp(s.front())) s.remove_prefix(1);
  while (!s.empty() && sp(s.back())) s.remove_suffix(1);
  return s;
}

bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool ends_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }

uint32_t utf8_next(std::string_view s, size_t& i) {
  auto b = [&](size_t k) { return static_cast<uint8_t>(s[k]); };
  uint8_t c = b(i);
  if (c < 0x80) { i += 1; return c; }
  auto cont = [&](size_t k) { return k < s.size() && (b(k) & 0xC0) == 0x80; };
  if ((c & 0xE0) == 0xC0 && cont(i + 1)) {
    uint32_t cp = (uint32_t(c & 0x1F) << 6) | (b(i + 1) & 0x3F);
    i += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0 && cont(i + 1) && cont(i + 2)) {
    uint32_t cp = (uint32_t(c & 0x0F) << 12) | (uint32_t(b(i + 1) & 0x3F) << 6) | (b(i + 2) & 0x3F);
    i += 3;
    return cp;
  }
  if ((c & 0xF8) == 0xF0 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
    uint32_t cp = (uint32_t(c & 0x07) << 18) | (uint32_t(b(i + 1) & 0x3F) << 12) | (uint32_t(b(i + 2) & 0x3F) << 6) | (b(i + 3) & 0x3F);
    i += 4;
    return cp;
  }
  i += 1;
  return 0xFFFD;
}

void utf8_append(std::string& o, uint32_t cp) {
  if (cp < 0x80) {
    o.push_back(char(cp));
  } else if (cp < 0x800) {
    o.push_back(char(0xC0 | (cp >> 6)));
    o.push_back(char(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    o.push_back(char(0xE0 | (cp >> 12)));
    o.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    o.push_back(char(0x80 | (cp & 0x3F)));
  } else {
    o.push_back(char(0xF0 | (cp >> 18)));
    o.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    o.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    o.push_back(char(0x80 | (cp & 0x3F)));
  }
}

static uint32_t upper_cp(uint32_t c) {
  if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
  if (c == 0xB5) return 0x39C;
  if (c == 0xFF) return 0x178;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : c;
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
  if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
  if (c >= 0x430 && c <= 0x44F) return c - 32;
  if (c >= 0x450 && c <= 0x45F) return c - 80;
  return c;
}

std::string utf8_upper(std::string_view s) {
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    uint32_t c = utf8_next(s, i);
    if (c == 0xDF) {  // sharp s upper-cases to "SS"
      o += "SS";
      continue;
    }
    utf8_append(o, upper_cp(c));
  }
  return o;
}

std::string utf8_cap_first(std::string_view s) {
  size_t i = 0;
  if (s.empty()) return std::string();
  utf8_next(s, i);
  std::string o = utf8_upper(s.substr(0, i));
  o.append(s.substr(i));
  return o;
}

size_t utf8_len(std::string_view s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size(); ++n) utf8_next(s, i);
  return n;
}

std::string uptime_text() {
  char buf[64];  // /proc files report size 0: read_file would return nothing
  FILE* f = std::fopen("/proc/uptime", "r");
  if (!f) return "?";
  size_t n = std::fread(buf, 1, sizeof buf - 1, f);
  std::fclose(f);
  std::string t(buf, n);
  return t.substr(0, t.find(' '));
}

std::string path_join(std::string_view a, std::string_view b) {
  std::string r(a);
  if (!r.empty() && r.back() != '/') r.push_back('/');
  r.append(b);
  return r;
}

std::string path_dir(std::string_view p) {
  size_t i = p.rfind('/');
  if (i == std::string_view::npos) return std::string();
  if (i == 0) return "/";
  return std::string(p.substr(0, i));
}

std::string path_filename(std::string_view p) {
  size_t i = p.rfind('/');
  return std::string(i == std::string_view::npos ? p : p.substr(i + 1));
}

std::string path_stem(std::string_view p) {
  std::string n = path_filename(p);
  size_t d = n.rfind('.');
  if (d == std::string::npos || d == 0) return n;
  return n.substr(0, d);
}

std::string path_ext_lower(std::string_view p) {
  std::string n = path_filename(p);
  size_t d = n.rfind('.');
  if (d == std::string::npos || d == 0) return std::string();
  std::string e = n.substr(d);
  for (char& c : e)
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return e;
}

}  // namespace zf
