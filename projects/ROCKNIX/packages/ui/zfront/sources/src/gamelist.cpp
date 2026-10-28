#include "gamelist.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "util.hpp"

namespace zf {

// ---------------------------------------------------------------------------------------------------------------
// small public helpers

std::string GameMeta::lastplayed_iso() const {
  if (lastplayed == 0) return std::string();
  char b[40];
  std::snprintf(b, sizeof b, "%08llu" "T" "%06llu", (unsigned long long)(lastplayed / 1000000ULL), (unsigned long long)(lastplayed % 1000000ULL));
  return b;
}

std::string_view gamelist_intern(std::string_view s) {
  static std::deque<std::string> store;  // deque: elements never move, SSO strings keep their address
  static std::unordered_set<std::string_view> index;
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  auto it = index.find(s);
  if (it != index.end()) return *it;
  store.emplace_back(s);
  index.insert(store.back());
  return store.back();
}

std::string resolve_rel_path(std::string_view p, const std::string& rom_dir) {
  p = trim(p);
  if (starts_with(p, "./")) return path_join(rom_dir, p.substr(2));
  if (starts_with(p, "~/")) {
    const char* home = std::getenv("HOME");
    return path_join(home ? home : "", p.substr(2));
  }
  if (!p.empty() && p[0] == '/') return std::string(p);
  return path_join(rom_dir, p);
}

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Tokenizer primitives over a window [p, end). `More` = the construct is not complete inside the window (read
// more or, at end of file, the XML is truncated); `Bad` = not XML we understand.

enum class St { Ok, More, Bad };

inline const char* find_ch(const char* p, const char* end, char c) { return p < end ? static_cast<const char*>(std::memchr(p, c, size_t(end - p))) : nullptr; }

const char* find_str(const char* p, const char* end, std::string_view n) {
  while (p < end) {
    const char* q = find_ch(p, end, n[0]);
    if (!q || size_t(end - q) < n.size()) return nullptr;
    if (std::memcmp(q, n.data(), n.size()) == 0) return q;
    p = q + 1;
  }
  return nullptr;
}

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// p at '<' followed by '?' or '!': comment, CDATA, processing instruction, DOCTYPE. Advances p past it.
St skip_markup(const char*& p, const char* end) {
  if (end - p < 2) return St::More;
  const char* q;
  if (p[1] == '?') {
    q = find_str(p + 2, end, "?>");
    if (!q) return St::More;
    p = q + 2;
    return St::Ok;
  }
  if (end - p < 9) return St::More;
  if (std::memcmp(p, "<!--", 4) == 0) {
    q = find_str(p + 4, end, "-->");
    if (!q) return St::More;
    p = q + 3;
  } else if (std::memcmp(p, "<![CDATA[", 9) == 0) {
    q = find_str(p + 9, end, "]]>");
    if (!q) return St::More;
    p = q + 3;
  } else {
    q = find_ch(p, end, '>');
    if (!q) return St::More;
    p = q + 1;
  }
  return St::Ok;
}

// p at '<' of an opening tag. Reads the name and skips the attributes (a '>' inside quotes does not end the tag).
St open_tag(const char*& p, const char* end, std::string_view& name, bool& self) {
  const char* s = p + 1;
  const char* q = s;
  while (q < end && !is_space(*q) && *q != '/' && *q != '>') ++q;
  if (q >= end) return St::More;
  name = std::string_view(s, size_t(q - s));
  if (name.empty()) return St::Bad;
  char quote = 0;
  for (; q < end; ++q) {
    char c = *q;
    if (quote) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      self = q[-1] == '/';
      p = q + 1;
      return St::Ok;
    }
  }
  return St::More;
}

// p at '<' of an opening tag: skips the whole element (nested children included).
St skip_element(const char*& p, const char* end) {
  int depth = 0;
  for (;;) {
    if (p + 1 >= end) return St::More;
    char c = p[1];
    if (c == '/') {
      const char* q = find_ch(p, end, '>');
      if (!q) return St::More;
      p = q + 1;
      if (--depth <= 0) return St::Ok;
    } else if (c == '!' || c == '?') {
      St r = skip_markup(p, end);
      if (r != St::Ok) return r;
    } else {
      std::string_view nm;
      bool self = false;
      St r = open_tag(p, end, nm, self);
      if (r != St::Ok) return r;
      if (self) {
        if (depth == 0) return St::Ok;
      } else {
        ++depth;
      }
    }
    const char* q = find_ch(p, end, '<');
    if (!q) return St::More;
    p = q;
  }
}

// p right after the opening tag of a leaf: finds its raw content [rb, re) and moves p past the closing tag.
// `nested` is set when it contains child elements (then the text is not a plain value).
St read_content(const char*& p, const char* end, const char*& rb, const char*& re, bool& nested) {
  rb = p;
  nested = false;
  for (;;) {
    const char* q = find_ch(p, end, '<');
    if (!q) return St::More;
    p = q;
    if (p + 1 >= end) return St::More;
    char c = p[1];
    if (c == '/') {
      re = p;
      q = find_ch(p, end, '>');
      if (!q) return St::More;
      p = q + 1;
      return St::Ok;
    }
    if (c == '!' || c == '?') {
      St r = skip_markup(p, end);
      if (r != St::Ok) return r;
      continue;
    }
    nested = true;
    St r = skip_element(p, end);
    if (r != St::Ok) return r;
  }
}

// p right after `<game ...>`: visits the DEPTH-1 children (`vis(tag, raw, nested, elem_begin, elem_end)`) up to and
// including `</game>`.
template <class V>
St game_body(const char*& p, const char* end, V&& vis) {
  for (;;) {
    const char* q = find_ch(p, end, '<');
    if (!q) return St::More;
    p = q;
    if (p + 1 >= end) return St::More;
    char c = p[1];
    if (c == '/') {
      q = find_ch(p, end, '>');
      if (!q) return St::More;
      p = q + 1;
      return St::Ok;
    }
    if (c == '!' || c == '?') {
      St r = skip_markup(p, end);
      if (r != St::Ok) return r;
      continue;
    }
    const char* eb = p;
    std::string_view name;
    bool self = false;
    St r = open_tag(p, end, name, self);
    if (r != St::Ok) return r;
    if (self) {
      vis(name, std::string_view(), false, eb, p);
      continue;
    }
    const char *rb, *re;
    bool nested;
    r = read_content(p, end, rb, re, nested);
    if (r != St::Ok) return r;
    vis(name, std::string_view(rb, size_t(re - rb)), nested, eb, p);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Text values

void decode(std::string_view raw, std::string& out) {
  out.clear();
  size_t i = 0, n = raw.size();
  while (i < n) {
    char c = raw[i];
    if (c == '&') {
      size_t semi = raw.find(';', i + 1);
      if (semi != std::string_view::npos && semi - i <= 10) {
        std::string_view e = raw.substr(i + 1, semi - i - 1);
        bool done = true;
        if (e == "amp") out.push_back('&');
        else if (e == "lt") out.push_back('<');
        else if (e == "gt") out.push_back('>');
        else if (e == "quot") out.push_back('"');
        else if (e == "apos") out.push_back('\'');
        else if (e.size() >= 2 && e[0] == '#') {
          uint32_t cp = 0;
          bool hex = e[1] == 'x' || e[1] == 'X';
          bool ok = e.size() > (hex ? 2u : 1u);
          for (size_t k = hex ? 2 : 1; k < e.size() && ok; ++k) {
            char d = e[k];
            int v = (d >= '0' && d <= '9') ? d - '0' : (hex && d >= 'a' && d <= 'f') ? d - 'a' + 10 : (hex && d >= 'A' && d <= 'F') ? d - 'A' + 10 : -1;
            if (v < 0) ok = false;
            else cp = cp * (hex ? 16u : 10u) + uint32_t(v);
            if (cp > 0x10FFFF) ok = false;
          }
          if (ok && cp != 0) utf8_append(out, cp);
          else done = false;
        } else {
          done = false;
        }
        if (done) {
          i = semi + 1;
          continue;
        }
      }
      out.push_back('&');
      ++i;
    } else if (c == '<') {  // CDATA verbatim, comments dropped
      if (raw.compare(i, 9, "<![CDATA[") == 0) {
        size_t e = raw.find("]]>", i + 9);
        if (e == std::string_view::npos) e = n;
        out.append(raw.substr(i + 9, e - i - 9));
        i = e + 3;
      } else if (raw.compare(i, 4, "<!--") == 0) {
        size_t e = raw.find("-->", i + 4);
        i = e == std::string_view::npos ? n : e + 3;
      } else {
        out.push_back(c);
        ++i;
      }
    } else {
      size_t j = i;
      while (j < n && raw[j] != '&' && raw[j] != '<') ++j;
      out.append(raw.substr(i, j - i));
      i = j;
    }
  }
}

// Trimmed text of a raw value; decodes into `scratch` only when it has entities / CDATA (the common case copies nothing).
std::string_view text_of(std::string_view raw, std::string& scratch) {
  raw = trim(raw);
  if (raw.find_first_of("&<") == std::string_view::npos) return raw;
  decode(raw, scratch);
  return trim(scratch);
}

bool ieq_true(std::string_view s) {
  s = trim(s);
  return s.size() == 4 && (s[0] | 32) == 't' && (s[1] | 32) == 'r' && (s[2] | 32) == 'u' && (s[3] | 32) == 'e';
}

int32_t year_of(std::string_view d) {
  d = trim(d);
  if (d.size() < 4) return -1;
  int y = 0;
  for (int i = 0; i < 4; ++i) {
    if (d[size_t(i)] < '0' || d[size_t(i)] > '9') return -1;
    y = y * 10 + (d[size_t(i)] - '0');
  }
  return y > 0 ? y : -1;
}

// `YYYYMMDDTHHMMSS` -> number YYYYMMDDHHMMSS; 0 if not exactly 14 digits ("0", "not-a-date-time").
uint64_t pack_iso(std::string_view s) {
  uint64_t v = 0;
  int n = 0;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      v = v * 10 + uint64_t(c - '0');
      if (++n > 14) return 0;
    }
  }
  return n == 14 ? v : 0;
}

// Raw (undecoded) values of one game while its children are visited. Last occurrence of a tag wins.
struct RawGame {
  std::string_view path, name, sortname, genre, players, rating, date, fav, hid, pc, lp, gt, desc, image, thumb;
  bool has_path = false;
};

inline void take_field(RawGame& g, std::string_view tag, std::string_view raw, bool nested, bool want_heavy) {
  if (nested) return;  // a value with child elements is not a plain value
  switch (tag.size()) {
    case 4:
      if (tag == "path") {
        g.path = raw;
        g.has_path = true;
      } else if (tag == "name") {
        g.name = raw;
      } else if (want_heavy && tag == "desc") {
        g.desc = raw;
      }
      break;
    case 5:
      if (tag == "genre") g.genre = raw;
      else if (want_heavy && tag == "image") g.image = raw;
      break;
    case 6:
      if (tag == "rating") g.rating = raw;
      else if (tag == "hidden") g.hid = raw;
      break;
    case 7:
      if (tag == "players") g.players = raw;
      break;
    case 8:
      if (tag == "sortname") g.sortname = raw;
      else if (tag == "favorite") g.fav = raw;
      else if (tag == "gametime") g.gt = raw;
      break;
    case 9:
      if (tag == "playcount") g.pc = raw;
      else if (want_heavy && tag == "thumbnail") g.thumb = raw;
      break;
    case 10:
      if (tag == "lastplayed") g.lp = raw;
      break;
    case 11:
      if (tag == "releasedate") g.date = raw;
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Window over a file (read in blocks) or over memory

class Window {
 public:
  explicit Window(std::string_view mem) : data_(mem.data()), len_(mem.size()), eof_(true), mem_(true) {}
  explicit Window(int fd) : fd_(fd) {
    buf_ = static_cast<char*>(std::malloc(cap_));
    data_ = buf_;
  }
  ~Window() { std::free(buf_); }
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;
  const char* data() const { return data_; }
  size_t len() const { return len_; }
  uint64_t base() const { return base_; }
  /// Drops [0, keep) and reads more. False when nothing more can be read (end of file / memory / size cap).
  bool refill(size_t keep) {
    if (mem_ || eof_ || !buf_) return false;
    if (keep > 0) {
      std::memmove(buf_, buf_ + keep, len_ - keep);
      len_ -= keep;
      base_ += keep;
    } else if (len_ == cap_) {
      if (cap_ >= kMaxWindow) return false;
      cap_ *= 2;
      char* nb = static_cast<char*>(std::realloc(buf_, cap_));
      if (!nb) return false;
      buf_ = nb;
      data_ = buf_;
    }
    ssize_t n;
    do n = ::read(fd_, buf_ + len_, cap_ - len_);
    while (n < 0 && errno == EINTR);
    if (n <= 0) {
      eof_ = true;
      return false;
    }
    len_ += size_t(n);
    return true;
  }
  /// True when the window is full at its size cap: the element being read does not fit.
  bool at_cap() const { return !mem_ && !eof_ && buf_ && len_ == cap_ && cap_ >= kMaxWindow; }
  /// Throws away everything up to and including the next `needle`, reading on as needed (memory stays bounded).
  /// False when the file ends first.
  bool discard_past(std::string_view needle) {
    for (;;) {
      size_t at = std::string_view(data_, len_).find(needle);
      if (at != std::string_view::npos) {
        size_t cut = at + needle.size();
        std::memmove(buf_, buf_ + cut, len_ - cut);
        len_ -= cut;
        base_ += cut;
        return true;
      }
      size_t keep = len_ >= needle.size() ? len_ - (needle.size() - 1) : 0;  // the tail may hold half a needle
      if (!refill(keep)) return false;
    }
  }

 private:
  static constexpr size_t kMaxWindow = 16u << 20;  // one element bigger than this is treated as malformed
  const char* data_ = nullptr;
  char* buf_ = nullptr;
  size_t len_ = 0, cap_ = 128u << 10;
  uint64_t base_ = 0;
  int fd_ = -1;
  bool eof_ = false, mem_ = false;
};

class Scanner {
 public:
  GamelistInfo run(Window& w, EntryFn cb) {
    GamelistInfo info;
    info.opened = true;
    enum { Prolog, Root, Done } st = Prolog;
    size_t pos = 0;
    bool in_game = false;  // the element being read is a <game>
    while (st != Done) {
      const char* d = w.data();
      const char* end = d + w.len();
      const char* q = find_ch(d + pos, end, '<');
      if (!q) {  // no markup in the rest of the window: drop it and read on
        if (!w.refill(w.len())) break;
        pos = 0;
        continue;
      }
      pos = size_t(q - d);
      const char* p = q;
      St r;
      if (p + 1 >= end) {
        r = St::More;
      } else {
        info.saw_markup = true;
        char c = p[1];
        if (c == '?' || c == '!') {
          r = skip_markup(p, end);
        } else if (c == '/') {
          const char* gt = find_ch(p, end, '>');
          if (st != Root) {
            r = St::Bad;
          } else if (!gt) {
            r = St::More;
          } else {
            info.root_close_begin = w.base() + pos;
            info.ok = true;
            st = Done;
            r = St::Ok;
          }
        } else if (st == Prolog) {
          std::string_view name;
          bool self = false;
          r = open_tag(p, end, name, self);
          if (r == St::Ok) {
            if (name != "gameList") {
              r = St::Bad;
            } else {
              info.root_begin = w.base() + pos;
              info.root_end = w.base() + size_t(p - d);
              if (self) {
                info.root_selfclosing = true;
                info.ok = true;
                st = Done;
              } else {
                st = Root;
              }
            }
          }
        } else {  // child of <gameList>
          const char* eb = p;
          std::string_view name;
          bool self = false;
          r = open_tag(p, end, name, self);
          if (r == St::Ok) {
            in_game = name == "game" && !self;
            if (in_game) {
              RawGame g;
              r = game_body(p, end, [&](std::string_view tag, std::string_view raw, bool nested, const char*, const char*) { take_field(g, tag, raw, nested, false); });
              if (r == St::Ok && g.has_path) deliver(g, w.base() + pos, w.base() + size_t(p - d), cb, info);
            } else if (!self) {
              p = eb;
              r = skip_element(p, end);
            }
          }
        }
      }
      if (r == St::Ok) {
        pos = size_t(p - d);
        in_game = false;
      } else if (r == St::More) {
        if (!w.refill(pos)) {
          // a <game> bigger than the window cap (a huge <desc>): skip just that one and go on with the rest of the list
          if (!(in_game && pos == 0 && w.at_cap() && w.discard_past("</game>"))) break;  // else: truncated file
          ++info.skipped;
        }
        pos = 0;
      } else {
        break;
      }
    }
    return info;
  }

 private:
  void deliver(const RawGame& g, uint64_t b, uint64_t e, EntryFn cb, GamelistInfo& info) {
    GameEntry en;
    en.path = text_of(g.path, s_path_);
    if (en.path.empty()) return;
    en.name = text_of(g.name, s_name_);
    en.sortname = text_of(g.sortname, s_sort_);
    GameMeta& m = en.meta;
    std::string_view gen = text_of(g.genre, s_genre_), pl = text_of(g.players, s_pl_);
    if (!gen.empty()) m.genre = gamelist_intern(gen);
    if (!pl.empty()) m.players = gamelist_intern(pl);
    m.release_year = year_of(g.date);
    float r = 0;
    if (parse_float(g.rating, r) && std::isfinite(r)) m.rating_pct = uint8_t(r <= 0 ? 0 : (r >= 1 ? 100 : int(r * 100.0f + 0.5f)));
    m.favorite = ieq_true(g.fav);
    m.hidden = ieq_true(g.hid);
    long pc = 0;
    if (parse_int(g.pc, pc) && pc > 0) m.playcount = pc > 0x7fffffffL ? 0x7fffffffu : uint32_t(pc);
    m.lastplayed = pack_iso(g.lp);
    long gt = 0;
    if (parse_int(g.gt, gt) && gt > 0) m.gametime = gt > 0x7fffffffL ? 0x7fffffffu : uint32_t(gt);
    en.begin = b;
    en.end = e;
    if (e <= 0xFFFFFFFFull) {
      m.span_begin = uint32_t(b);
      m.span_end = uint32_t(e);
    }
    ++info.games;
    cb.fn(cb.ctx, en);
  }

  std::string s_path_, s_name_, s_sort_, s_genre_, s_pl_;
};

struct Fd {
  int fd;
  explicit Fd(int f) : fd(f) {}
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};

// A complete `<game>` element in memory: raw fields (heavy ones included). False if it is not a game element.
bool parse_one(std::string_view el, RawGame& g) {
  const char* p = el.data();
  const char* end = p + el.size();
  if (el.size() < 6 || el[0] != '<') return false;
  std::string_view name;
  bool self = false;
  if (open_tag(p, end, name, self) != St::Ok || name != "game" || self) return false;
  return game_body(p, end, [&](std::string_view tag, std::string_view raw, bool nested, const char*, const char*) { take_field(g, tag, raw, nested, true); }) == St::Ok;
}

}  // namespace

GamelistInfo gamelist_scan_text(std::string_view xml, EntryFn cb) {
  Window w(xml);
  Scanner s;
  return s.run(w, cb);
}

GamelistInfo gamelist_scan_file(const std::string& path, EntryFn cb) {
  Fd f(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (f.fd < 0) return GamelistInfo();
  (void)!::posix_fadvise(f.fd, 0, 0, POSIX_FADV_SEQUENTIAL);
  Window w(f.fd);
  Scanner s;
  return s.run(w, cb);
}

// ---------------------------------------------------------------------------------------------------------------
// details

namespace {

GameDetails details_from(std::string_view el, const std::string& rom_dir, const std::string& rom, bool* matched) {
  GameDetails d;
  *matched = false;
  RawGame g;
  if (!parse_one(el, g)) return d;
  std::string sp, sd;
  std::string_view path = text_of(g.path, sp);
  if (path.empty() || resolve_rel_path(path, rom_dir) != rom) return d;
  *matched = true;
  d.desc = std::string(text_of(g.desc, sd));
  std::string_view img = text_of(g.image, sp), th = text_of(g.thumb, sd);
  if (!img.empty()) d.image = resolve_rel_path(img, rom_dir);
  if (!th.empty()) d.thumbnail = resolve_rel_path(th, rom_dir);
  return d;
}

}  // namespace

GameDetails gamelist_details(const std::string& xml_path, const std::string& rom_dir, const std::string& rom, const GameMeta& meta) {
  uint32_t s = meta.span_begin, e = meta.span_end;
  if (e == 0) return GameDetails();  // not in the gamelist (no span): nothing to read, never scan the file for it
  if (e > s && e - s <= (1u << 20)) {
    std::string buf;
    if (read_file_range(xml_path, s, e - s, buf)) {
      bool ok = false;
      GameDetails d = details_from(buf, rom_dir, rom, &ok);
      if (ok) return d;
    }
  }
  // stale span (the file changed since it was loaded): one streaming search for this ROM
  uint64_t fb = 0, fe = 0;
  bool found = false;
  auto cb = [&](const GameEntry& en) {
    if (resolve_rel_path(en.path, rom_dir) == rom) {
      found = true;
      fb = en.begin;
      fe = en.end;
    }
  };
  gamelist_scan_file(xml_path, EntryFn::of(cb));
  if (!found || fe <= fb || fe - fb > (1u << 20)) return GameDetails();
  std::string buf;
  if (!read_file_range(xml_path, fb, size_t(fe - fb), buf)) return GameDetails();
  bool ok = false;
  return details_from(buf, rom_dir, rom, &ok);
}

// ---------------------------------------------------------------------------------------------------------------
// updates

namespace {

std::string esc(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o.push_back(c);
  }
  return o;
}

using Field = std::pair<std::string, std::string>;  // (tag, value)

std::string rel_of(const std::string& rom, const std::string& rom_dir) {
  std::string pre = rom_dir;
  if (!pre.empty() && pre.back() != '/') pre.push_back('/');
  if (!pre.empty() && starts_with(rom, pre)) return "./" + rom.substr(pre.size());
  return rom;
}

std::string new_block(const std::string& rel, const std::vector<Field>& f) {
  std::string b = "\t<game>\n\t\t<path>" + esc(rel) + "</path>\n";
  for (const auto& kv : f) b += "\t\t<" + kv.first + ">" + esc(kv.second) + "</" + kv.first + ">\n";
  return b + "\t</game>\n";
}

// New text of one `<game>` element with `fields` set: existing children replaced (the last occurrence, the one the
// reader uses), missing ones appended after the last child. Empty string if `el` is not a plain game element.
std::string edit_element(std::string_view el, const std::vector<Field>& fields) {
  if (!ends_with(el, "</game>")) return std::string();
  const char* d = el.data();
  const char* p = d;
  const char* end = d + el.size();
  std::string_view name;
  bool self = false;
  if (open_tag(p, end, name, self) != St::Ok || name != "game" || self) return std::string();
  size_t open_end = size_t(p - d), last_child_end = open_end;
  struct Rng {
    size_t b = 0, e = 0;
    bool have = false;
  };
  std::vector<Rng> rng(fields.size());
  St r = game_body(p, end, [&](std::string_view tag, std::string_view, bool, const char* eb, const char* ee) {
    last_child_end = size_t(ee - d);
    for (size_t i = 0; i < fields.size(); ++i)
      if (tag == fields[i].first) rng[i] = {size_t(eb - d), size_t(ee - d), true};
  });
  if (r != St::Ok) return std::string();
  struct Ed {
    size_t b, e;
    std::string r;
  };
  std::vector<Ed> eds;
  std::string missing;
  for (size_t i = 0; i < fields.size(); ++i) {
    std::string txt = "<" + fields[i].first + ">" + esc(fields[i].second) + "</" + fields[i].first + ">";
    if (rng[i].have) eds.push_back({rng[i].b, rng[i].e, txt});
    else missing += "\n\t\t" + txt;
  }
  if (!missing.empty()) eds.push_back({last_child_end, last_child_end, missing});
  // back to front
  for (size_t a = 0; a < eds.size(); ++a)
    for (size_t b = a + 1; b < eds.size(); ++b)
      if (eds[b].b > eds[a].b) std::swap(eds[a], eds[b]);
  std::string out(el);
  for (auto& e : eds) out.replace(e.b, e.e - e.b, e.r);
  return out;
}

bool write_all(int fd, const char* p, size_t n) {
  while (n) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= size_t(w);
  }
  return true;
}

bool copy_range(int in, int out, uint64_t from, uint64_t to /* exclusive; UINT64_MAX = EOF */) {
  std::vector<char> bv(64 * 1024);
  char* buf = bv.data();
  uint64_t off = from;
  while (off < to) {
    size_t want = bv.size();
    if (to != UINT64_MAX && to - off < want) want = size_t(to - off);
    ssize_t n = ::pread(in, buf, want, off_t(off));
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return to == UINT64_MAX;
    if (!write_all(out, buf, size_t(n))) return false;
    off += uint64_t(n);
  }
  return true;
}

// Rewrites `path`: [0,begin) + repl + [begin+old_len, EOF) of the open original `in` (-1: no original) into a
// sibling temporary file (fsync), then renames it over `path`.
bool splice_file(const std::string& path, int in, uint64_t begin, uint64_t old_len, const std::string& repl, std::string& err) {
  if (writes_blocked()) return true;  // preview / dry-run: the file is never touched
  std::string tmp = path + ".zfront-tmp";
  int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    err = tmp + ": cannot create";
    return false;
  }
  bool ok = true;
  struct stat st;
  if (in >= 0 && ::fstat(in, &st) == 0) (void)!::fchmod(out, st.st_mode & 07777);
  if (in >= 0) ok = copy_range(in, out, 0, begin);
  ok = ok && write_all(out, repl.data(), repl.size());
  if (ok && in >= 0) ok = copy_range(in, out, begin + old_len, UINT64_MAX);
  ok = ok && ::fsync(out) == 0;
  ok = (::close(out) == 0) && ok;
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    err = path + ": cannot write";
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

uint32_t add_capped(uint32_t cur, uint64_t add) { return uint32_t(std::min<uint64_t>(uint64_t(cur) + add, 0x7fffffffu)); }

// `bump`: playcount + 1; `add_secs`: gametime + add_secs (both are read from the file, not from memory)
bool apply(const std::string& xml_path, const std::string& rom_dir, const std::string& rom, std::vector<Field> fields, bool bump, uint64_t add_secs, std::string& err,
           GamelistEdit* edit) {
  std::string rel = rel_of(rom, rom_dir);
  auto new_doc = [&]() {
    std::vector<Field> f = fields;
    if (bump) f.push_back({"playcount", "1"});
    if (add_secs) f.push_back({"gametime", std::to_string(add_capped(0, add_secs))});
    return "<?xml version=\"1.0\"?>\n<gameList>\n" + new_block(rel, f) + "</gameList>\n";
  };
  Fd f(::open(xml_path.c_str(), O_RDONLY | O_CLOEXEC));
  if (f.fd < 0) {
    if (errno != ENOENT) {
      err = xml_path + ": cannot open";
      return false;
    }
    std::string doc = new_doc();
    if (edit) *edit = {0, 0, doc.size()};
    return splice_file(xml_path, -1, 0, 0, doc, err);
  }
  bool found = false;
  uint64_t fb = 0, fe = 0;
  uint32_t pc = 0, gt = 0;
  auto cb = [&](const GameEntry& en) {
    if (resolve_rel_path(en.path, rom_dir) == rom) {
      found = true;
      fb = en.begin;
      fe = en.end;
      pc = en.meta.playcount;
      gt = en.meta.gametime;
    }
  };
  Window w(f.fd);
  Scanner sc;
  GamelistInfo info = sc.run(w, EntryFn::of(cb));
  struct stat st;
  if (!info.saw_markup && ::fstat(f.fd, &st) == 0 && st.st_size < 4096) {
    std::string t;
    if (read_file_range(xml_path, 0, size_t(st.st_size), t) && trim(t).empty()) {  // empty / blank file: start a document
      std::string doc = new_doc();
      if (edit) *edit = {0, uint64_t(st.st_size), doc.size()};
      return splice_file(xml_path, -1, 0, 0, doc, err);
    }
  }
  if (!info.ok || info.skipped) {  // skipped: an oversized <game> was not read, so it may be this very ROM
    err = xml_path + (info.ok ? ": has an oversized <game> element, not modified" : ": malformed gamelist.xml, not modified");
    return false;
  }
  uint64_t begin, old_len;
  std::string repl;
  if (found) {
    if (fe <= fb || fe - fb > (8u << 20)) {
      err = xml_path + ": unsupported <game> element";
      return false;
    }
    std::string el;
    if (!read_file_range(xml_path, fb, size_t(fe - fb), el)) {
      err = xml_path + ": cannot read";
      return false;
    }
    if (bump) fields.push_back({"playcount", std::to_string(pc == 0x7fffffffu ? pc : pc + 1)});
    if (add_secs) fields.push_back({"gametime", std::to_string(add_capped(gt, add_secs))});
    repl = edit_element(el, fields);
    if (repl.empty()) {
      err = xml_path + ": unsupported <game> element";
      return false;
    }
    begin = fb;
    old_len = fe - fb;
  } else {
    if (bump) fields.push_back({"playcount", "1"});
    if (add_secs) fields.push_back({"gametime", std::to_string(add_capped(0, add_secs))});
    std::string block = new_block(rel, fields);
    if (info.root_selfclosing) {
      begin = info.root_begin;
      old_len = info.root_end - info.root_begin;
      repl = "<gameList>\n" + block + "</gameList>";
    } else {
      begin = info.root_close_begin;
      old_len = 0;
      char prev = 0;
      std::string pb;
      if (begin > 0 && read_file_range(xml_path, begin - 1, 1, pb)) prev = pb[0];
      repl = (prev == '\n' ? "" : "\n") + block;
    }
  }
  if (!splice_file(xml_path, f.fd, begin, old_len, repl, err)) return false;
  if (edit) *edit = {begin, old_len, repl.size()};
  return true;
}

}  // namespace

bool gamelist_update_after_play(const std::string& xml_path, const std::string& rom_dir, const std::string& rom, const std::string& now_iso, std::string& err, GamelistEdit* edit,
                                uint64_t add_secs) {
  return apply(xml_path, rom_dir, rom, {{"lastplayed", now_iso}}, true, add_secs, err, edit);
}

bool gamelist_set_favorite(const std::string& xml_path, const std::string& rom_dir, const std::string& rom, bool fav, std::string& err, GamelistEdit* edit) {
  return apply(xml_path, rom_dir, rom, {{"favorite", fav ? "true" : "false"}}, false, 0, err, edit);
}

// ---------------------------------------------------------------------------------------------------------------
// batch edit (scraper): many games, ONE rewrite of the file

namespace {

struct Rep {
  uint64_t begin = 0, old_len = 0;
  std::string text;
};

// Like `splice_file` for several non-overlapping replacements sorted by `begin`.
bool splice_multi(const std::string& path, int in, const std::vector<Rep>& reps, std::string& err) {
  std::string tmp = path + ".zfront-tmp";
  int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    err = tmp + ": cannot create";
    return false;
  }
  struct stat st;
  if (in >= 0 && ::fstat(in, &st) == 0) (void)!::fchmod(out, st.st_mode & 07777);
  bool ok = true;
  uint64_t pos = 0;
  for (const Rep& r : reps) {
    ok = ok && copy_range(in, out, pos, r.begin) && write_all(out, r.text.data(), r.text.size());
    pos = r.begin + r.old_len;
  }
  ok = ok && copy_range(in, out, pos, UINT64_MAX);
  ok = ok && ::fsync(out) == 0;
  ok = (::close(out) == 0) && ok;
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    err = path + ": cannot write";
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

std::string scrap_text(const std::string& name, const std::string& date) { return "<scrap name=\"" + esc(name) + "\" date=\"" + esc(date) + "\"/>"; }

// A complete `<game>` element with `<scrap name=".." date=".."/>` set: the child of that scraper is replaced, any other
// `<scrap>` stays. Empty string if `el` is not a plain game element.
std::string upsert_scrap(std::string_view el, const std::string& name, const std::string& date) {
  if (!ends_with(el, "</game>")) return std::string();
  const char* d = el.data();
  const char* p = d;
  const char* end = d + el.size();
  std::string_view tag;
  bool self = false;
  if (open_tag(p, end, tag, self) != St::Ok || tag != "game" || self) return std::string();
  size_t last_child_end = size_t(p - d), b = 0, e = 0;
  bool have = false;
  const std::string needle1 = "name=\"" + name + "\"", needle2 = "name='" + name + "'";
  St r = game_body(p, end, [&](std::string_view t, std::string_view, bool, const char* eb, const char* ee) {
    last_child_end = size_t(ee - d);
    if (t != "scrap") return;
    std::string_view whole(eb, size_t(ee - eb));
    size_t gt = whole.find('>');
    std::string_view open = whole.substr(0, gt == std::string_view::npos ? whole.size() : gt);
    if (open.find(needle1) != std::string_view::npos || open.find(needle2) != std::string_view::npos) {
      b = size_t(eb - d);
      e = size_t(ee - d);
      have = true;
    }
  });
  if (r != St::Ok) return std::string();
  std::string out(el);
  if (have) out.replace(b, e - b, scrap_text(name, date));
  else out.insert(last_child_end, "\n\t\t" + scrap_text(name, date));
  return out;
}

std::string new_block_scrap(const std::string& rel, const std::vector<Field>& f, const std::string& scraper, const std::string& date) {
  std::string b = new_block(rel, f);
  if (!scraper.empty()) {
    size_t at = b.rfind("\t</game>");
    if (at != std::string::npos) b.insert(at, "\t\t" + scrap_text(scraper, date) + "\n");
  }
  return b;
}

}  // namespace

bool gamelist_edit_fields(const std::string& xml_path, const std::string& rom_dir, const std::vector<GameFieldEdit>& edits, std::string& err, GamelistBatchResult* res) {
  GamelistBatchResult local;
  GamelistBatchResult& R = res ? *res : local;
  R = GamelistBatchResult();
  if (edits.empty()) return true;
  std::unordered_map<std::string, size_t> want;  // ROM path -> index of its edit (the last one wins)
  for (size_t i = 0; i < edits.size(); ++i) want[edits[i].rom] = i;
  struct Span {
    uint64_t b = 0, e = 0;
    bool have = false;
  };
  std::vector<Span> spans(edits.size());

  auto fresh_doc = [&]() {
    std::string body;
    for (size_t i = 0; i < edits.size(); ++i)
      if (want[edits[i].rom] == i) body += new_block_scrap(rel_of(edits[i].rom, rom_dir), edits[i].fields, edits[i].scraper, edits[i].scrape_date), ++R.created;
    return "<?xml version=\"1.0\"?>\n<gameList>\n" + body + "</gameList>\n";
  };

  Fd f(::open(xml_path.c_str(), O_RDONLY | O_CLOEXEC));
  if (f.fd < 0) {
    if (errno != ENOENT) {
      err = xml_path + ": cannot open";
      return false;
    }
    return splice_file(xml_path, -1, 0, 0, fresh_doc(), err);
  }
  auto cb = [&](const GameEntry& en) {
    auto it = want.find(resolve_rel_path(en.path, rom_dir));
    if (it != want.end()) spans[it->second] = {en.begin, en.end, true};
  };
  Window w(f.fd);
  Scanner sc;
  GamelistInfo info = sc.run(w, EntryFn::of(cb));
  struct stat st;
  if (!info.saw_markup && ::fstat(f.fd, &st) == 0 && st.st_size < 4096) {
    std::string t;
    if (read_file_range(xml_path, 0, size_t(st.st_size), t) && trim(t).empty()) {  // empty / blank file: start a document
      R = GamelistBatchResult();
      return splice_file(xml_path, -1, 0, 0, fresh_doc(), err);
    }
  }
  if (!info.ok || info.skipped) {  // skipped: an oversized <game> was not read, so it may be this very ROM
    err = xml_path + (info.ok ? ": has an oversized <game> element, not modified" : ": malformed gamelist.xml, not modified");
    return false;
  }

  std::vector<Rep> reps;
  std::string created;
  for (size_t i = 0; i < edits.size(); ++i) {
    if (want[edits[i].rom] != i) continue;  // superseded by a later edit of the same ROM
    const GameFieldEdit& ed = edits[i];
    if (spans[i].have) {
      const Span& s = spans[i];
      std::string el, repl;
      if (s.e > s.b && s.e - s.b <= (8u << 20) && read_file_range(xml_path, s.b, size_t(s.e - s.b), el)) {
        repl = ed.fields.empty() ? el : edit_element(el, ed.fields);
        if (!repl.empty() && !ed.scraper.empty()) repl = upsert_scrap(repl, ed.scraper, ed.scrape_date);
      }
      if (repl.empty()) {
        ++R.failed;  // unsupported element: left as it was
        continue;
      }
      if (repl != el) reps.push_back({s.b, s.e - s.b, std::move(repl)});
      ++R.edited;
    } else {
      created += new_block_scrap(rel_of(ed.rom, rom_dir), ed.fields, ed.scraper, ed.scrape_date);
      ++R.created;
    }
  }
  if (!created.empty()) {
    Rep c;
    if (info.root_selfclosing) {
      c.begin = info.root_begin;
      c.old_len = info.root_end - info.root_begin;
      c.text = "<gameList>\n" + created + "</gameList>";
    } else {
      c.begin = info.root_close_begin;
      char prev = 0;
      std::string pb;
      if (c.begin > 0 && read_file_range(xml_path, c.begin - 1, 1, pb)) prev = pb[0];
      c.text = (prev == '\n' ? "" : "\n") + created;
    }
    reps.push_back(std::move(c));
  }
  if (reps.empty()) return true;
  std::sort(reps.begin(), reps.end(), [](const Rep& a, const Rep& b) { return a.begin < b.begin; });
  return splice_multi(xml_path, f.fd, reps, err);
}

}  // namespace zf
