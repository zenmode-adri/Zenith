#include "webbios.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "httpd.hpp"
#include "oscontract.hpp"
#include "scraper_hash.hpp"
#include "webcore.hpp"
#include "webtransfer.hpp"

namespace zf {
namespace webbios {

using std::string;

// ---- the table of the script -------------------------------------------------------------------------------------------

namespace {

/// A Python literal reduced to what the table uses: strings, {..}, [..]; anything else (True, 1...) is kept as a bare word.
struct Val {
  char kind = 'x';  // 's' string, 'o' object, 'a' array, 'x' other
  string s;
  std::vector<std::pair<string, Val>> o;
  std::vector<Val> a;
  const Val* get(const char* key) const {
    for (const auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
};

struct Parser {
  const string& t;
  size_t i;
  bool ok = true;
  void skip() {
    while (i < t.size()) {
      if (t[i] == '#') {
        while (i < t.size() && t[i] != '\n') ++i;
      } else if (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n') {
        ++i;
      } else {
        break;
      }
    }
  }
  string str() {
    const char q = t[i++];
    string out;
    while (i < t.size() && t[i] != q) {
      if (t[i] == '\n') {
        ok = false;
        return out;
      }
      if (t[i] == '\\' && i + 1 < t.size()) ++i;
      out += t[i++];
    }
    if (i >= t.size()) ok = false;
    else ++i;
    return out;
  }
  Val value(int depth) {
    Val v;
    skip();
    if (i >= t.size() || depth > 8) {
      ok = false;
      return v;
    }
    const char c = t[i];
    if (c == '"' || c == '\'') {
      v.kind = 's';
      v.s = str();
    } else if (c == '{') {
      v.kind = 'o';
      ++i;
      for (;;) {
        skip();
        if (i >= t.size()) {
          ok = false;
          return v;
        }
        if (t[i] == '}') {
          ++i;
          break;
        }
        Val k = value(depth + 1);
        skip();
        if (!ok || i >= t.size() || t[i] != ':') {
          ok = false;
          return v;
        }
        ++i;
        Val x = value(depth + 1);
        if (!ok) return v;
        v.o.emplace_back(k.s, std::move(x));
        skip();
        if (i < t.size() && t[i] == ',') ++i;
      }
    } else if (c == '[') {
      v.kind = 'a';
      ++i;
      for (;;) {
        skip();
        if (i >= t.size()) {
          ok = false;
          return v;
        }
        if (t[i] == ']') {
          ++i;
          break;
        }
        Val x = value(depth + 1);
        if (!ok) return v;
        v.a.push_back(std::move(x));
        skip();
        if (i < t.size() && t[i] == ',') ++i;
      }
    } else {
      const size_t from = i;
      while (i < t.size() && !std::strchr(",:{}[]# \t\r\n", t[i])) ++i;
      if (i == from) ok = false;
      v.s = t.substr(from, i - from);
    }
    return v;
  }
};

string lower_hex(string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'F') c = char(c + 32);
  return s;
}

}  // namespace

std::vector<BiosSystem> parse_systems(const string& script) {
  std::vector<BiosSystem> out;
  size_t at = 0;  // `systems = {` at the start of a line
  for (;;) {
    at = script.find("systems", at);
    if (at == string::npos) return out;
    if (at == 0 || script[at - 1] == '\n') {
      size_t k = at + 7;
      while (k < script.size() && (script[k] == ' ' || script[k] == '\t')) ++k;
      if (k < script.size() && script[k] == '=') {
        ++k;
        while (k < script.size() && (script[k] == ' ' || script[k] == '\t')) ++k;
        if (k < script.size() && script[k] == '{') {
          at = k;
          break;
        }
      }
    }
    at += 7;
  }
  Parser p{script, at};
  Val top = p.value(0);
  if (!p.ok || top.kind != 'o') return out;
  for (const auto& kv : top.o) {
    const Val& sv = kv.second;
    if (sv.kind != 'o') continue;
    BiosSystem sys;
    sys.key = kv.first;
    if (const Val* n = sv.get("name"); n && n->kind == 's') sys.name = n->s;
    const Val* files = sv.get("biosFiles");
    if (!files || files->kind != 'a') continue;
    for (const Val& e : files->a) {
      const Val* f = e.get("file");
      if (e.kind != 'o' || !f || f->kind != 's' || f->s.empty()) continue;
      ++sys.entries;
      auto it = std::find_if(sys.files.begin(), sys.files.end(), [&](const BiosFile& b) { return b.file == f->s; });
      if (it == sys.files.end()) {
        sys.files.push_back(BiosFile());
        it = sys.files.end() - 1;
        it->file = f->s;
      }
      if (const Val* z = e.get("zippedFile"); z && z->kind == 's') it->zipped = true;
      for (const char* key : {"md5", "altmd5"}) {
        const Val* m = e.get(key);
        if (!m || m->kind != 's') continue;
        if (m->s.empty()) {
          it->any = true;
          continue;
        }
        const string h = lower_hex(m->s);
        if (std::find(it->md5s.begin(), it->md5s.end(), h) == it->md5s.end()) it->md5s.push_back(h);
      }
    }
    // a key written twice: Python keeps the first position and the last value
    auto dup = std::find_if(out.begin(), out.end(), [&](const BiosSystem& b) { return b.key == sys.key; });
    if (dup != out.end()) *dup = std::move(sys);
    else out.push_back(std::move(sys));
  }
  return out;
}

// ---- files below the root ----------------------------------------------------------------------------------------------

namespace {

std::atomic<size_t> g_hashes{0};

std::vector<string> split_path(const string& rel) {
  std::vector<string> v;
  size_t from = 0;
  for (;;) {
    size_t s = rel.find('/', from);
    v.push_back(rel.substr(from, s == string::npos ? string::npos : s - from));
    if (s == string::npos) break;
    from = s + 1;
  }
  return v;
}

/// Every component is a plain name (webtransfer::valid_name): no "..", no empty part, no leading dot.
bool safe_rel(const string& rel, std::vector<string>& comps) {
  comps = split_path(rel);
  for (const string& c : comps)
    if (!webtransfer::valid_name(c)) return false;
  return !comps.empty();
}

/// Folder holding comps[0..n) below `root_fd`, one O_NOFOLLOW step at a time (a symlink anywhere fails). `create`: missing
/// folders are made; `first_made` gets the index of the first one made (-1 none). Returns a new descriptor, or -1.
int walk(int root_fd, const std::vector<string>& comps, size_t n, bool create, int* first_made) {
  int cur = ::dup(root_fd);
  for (size_t k = 0; k < n && cur >= 0; ++k) {
    int nx = ::openat(cur, comps[k].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (nx < 0 && errno == ENOENT && create && ::mkdirat(cur, comps[k].c_str(), 0755) == 0) {
      if (first_made && *first_made < 0) *first_made = int(k);
      nx = ::openat(cur, comps[k].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    }
    ::close(cur);
    cur = nx;
  }
  return cur;
}

/// Takes back the folders `walk` made (comps[first..n-1], deepest first); a folder that is not empty stays.
void unmake(int root_fd, const std::vector<string>& comps, int first, size_t n) {
  if (first < 0) return;
  for (size_t k = n; k > size_t(first); --k) {
    int d = walk(root_fd, comps, k - 1, false, nullptr);
    if (d < 0) continue;
    ::unlinkat(d, comps[k - 1].c_str(), AT_REMOVEDIR);
    ::close(d);
  }
}

string root_dir() {
  const char* e = std::getenv("ZF_BIOS_ROOT");
  return e && *e ? e : "/storage/roms";
}

string script_path() {
  const char* e = std::getenv("ZF_ROCKNIX_SYSTEMS");
  return e && *e ? e : "/usr/bin/rocknix-systems";
}

bool slurp(const string& p, string& out) {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  out.clear();
  char b[16384];
  ssize_t n;
  while ((n = ::read(fd, b, sizeof b)) > 0) out.append(b, size_t(n));
  ::close(fd);
  return n == 0;
}

long long mtime_ns(const struct stat& sb) { return (long long)sb.st_mtim.tv_sec * 1000000000LL + sb.st_mtim.tv_nsec; }

/// The parsed script, re-read only when the file changed.
std::vector<BiosSystem> list_systems() {
  static std::mutex m;
  static string cached_path;
  static long long cached_size = -1, cached_mtime = 0;
  static std::vector<BiosSystem> cached;
  const string path = script_path();
  struct stat sb;
  if (::stat(path.c_str(), &sb) != 0) return {};
  std::lock_guard<std::mutex> lk(m);
  if (path != cached_path || cached_size != (long long)sb.st_size || cached_mtime != mtime_ns(sb)) {
    string text;
    if (!slurp(path, text)) return {};
    cached = parse_systems(text);
    cached_path = path;
    cached_size = (long long)sb.st_size;
    cached_mtime = mtime_ns(sb);
  }
  return cached;
}

struct Seen {
  long long size = -1, mtime = 0;
  string md5;
};
std::mutex g_cache_m;
std::map<string, Seen>& cache() {
  static std::map<string, Seen> c;
  return c;
}

void remember(const string& path, const struct stat& sb, const string& md5) {
  std::lock_guard<std::mutex> lk(g_cache_m);
  cache()[path] = Seen{(long long)sb.st_size, mtime_ns(sb), md5};
}

enum class State { Missing, Ok, Wrong, Unchecked };

struct Probe {
  State st = State::Missing;
  string md5;  // of the file
};

/// State of one list entry below `root_fd`; hashes the file unless the cache knows it (path, size, mtime).
Probe probe(int root_fd, const string& root, const BiosFile& f) {
  Probe r;
  std::vector<string> comps;
  if (!safe_rel(f.file, comps)) return r;
  int dir = walk(root_fd, comps, comps.size() - 1, false, nullptr);
  if (dir < 0) return r;
  struct stat sb;
  if (::fstatat(dir, comps.back().c_str(), &sb, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(sb.st_mode)) {
    ::close(dir);
    return r;
  }
  if (f.zipped) {
    ::close(dir);
    r.st = State::Unchecked;
    return r;
  }
  const string full = root + "/" + f.file;
  {
    std::lock_guard<std::mutex> lk(g_cache_m);
    auto it = cache().find(full);
    if (it != cache().end() && it->second.size == (long long)sb.st_size && it->second.mtime == mtime_ns(sb)) r.md5 = it->second.md5;
  }
  if (r.md5.empty()) {
    int fd = ::openat(dir, comps.back().c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
      struct stat fb;
      if (::fstat(fd, &fb) == 0 && S_ISREG(fb.st_mode)) {
        scraper::Md5 h;
        char buf[64 * 1024];
        ssize_t n;
        while ((n = ::read(fd, buf, sizeof buf)) > 0) h.update(buf, size_t(n));
        if (n == 0) {
          ++g_hashes;
          r.md5 = h.hex();
          remember(full, fb, r.md5);
        }
      }
      ::close(fd);
    }
  }
  ::close(dir);
  if (r.md5.empty()) return r;  // unreadable: as good as missing
  r.st = f.any || std::find(f.md5s.begin(), f.md5s.end(), r.md5) != f.md5s.end() ? State::Ok : State::Wrong;
  return r;
}

const char* state_name(State s) {
  switch (s) {
    case State::Ok: return "ok";
    case State::Wrong: return "wrong";
    case State::Unchecked: return "unchecked";
    default: return "missing";
  }
}

// ---- GET /files/bios ---------------------------------------------------------------------------------------------------

http::Response list_route(const http::Request& req) {
  string all_s;
  const bool all = http::query_param(req, "all", all_s) && all_s == "1";
  std::vector<BiosSystem> sys = list_systems();
  if (sys.empty()) return webtransfer::err(404, "the BIOS list of this system is not available");
  if (!all) {  // only the systems that have games (rocknix-systems names them by short name; the full name also matches)
    std::vector<webtransfer::SystemInfo> have = webtransfer::systems_snapshot();
    std::vector<BiosSystem> kept;
    for (BiosSystem& b : sys) {
      bool game = false;
      for (const webtransfer::SystemInfo& si : have)
        if (si.count > 0 && (si.name == b.key || (!b.name.empty() && webtransfer::lower(si.fullname) == webtransfer::lower(b.name)))) game = true;
      if (game) kept.push_back(std::move(b));
    }
    sys.swap(kept);
  }
  const string root = root_dir();
  int root_fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  struct Row {
    const BiosSystem* s;
    std::vector<Probe> p;
    size_t missing = 0, wrong = 0;
  };
  std::vector<Row> rows;
  size_t t_missing = 0, t_wrong = 0, t_unchecked = 0, t_ok = 0;
  for (const BiosSystem& b : sys) {
    Row r;
    r.s = &b;
    for (const BiosFile& f : b.files) {
      Probe pr = root_fd >= 0 ? probe(root_fd, root, f) : Probe();
      r.missing += pr.st == State::Missing;
      r.wrong += pr.st == State::Wrong;
      t_unchecked += pr.st == State::Unchecked;
      t_ok += pr.st == State::Ok;
      r.p.push_back(std::move(pr));
    }
    t_missing += r.missing;
    t_wrong += r.wrong;
    rows.push_back(std::move(r));
  }
  if (root_fd >= 0) ::close(root_fd);
  std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.missing + a.wrong > 0 && b.missing + b.wrong == 0; });
  using oscontract::json_escape;
  string j = "{\"all\":" + string(all ? "true" : "false") + ",\"missing\":" + std::to_string(t_missing) + ",\"wrong\":" + std::to_string(t_wrong) +
             ",\"unchecked\":" + std::to_string(t_unchecked) + ",\"ok\":" + std::to_string(t_ok) + ",\"systems\":[";
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row& r = rows[i];
    j += string(i ? "," : "") + "{\"key\":\"" + json_escape(r.s->key) + "\",\"name\":\"" + json_escape(r.s->name.empty() ? r.s->key : r.s->name) +
         "\",\"problems\":" + std::to_string(r.missing + r.wrong) + ",\"files\":[";
    for (size_t k = 0; k < r.s->files.size(); ++k) {
      const BiosFile& f = r.s->files[k];
      j += string(k ? "," : "") + "{\"file\":\"" + json_escape(f.file) + "\",\"status\":\"" + state_name(r.p[k].st) + "\",\"zipped\":" +
           (f.zipped ? "true" : "false") + ",\"any\":" + (f.any ? "true" : "false") + ",\"md5\":[";
      if (!f.zipped)
        for (size_t m = 0; m < f.md5s.size(); ++m) j += string(m ? ",\"" : "\"") + f.md5s[m] + "\"";
      j += "]";
      if (r.p[k].st == State::Wrong) j += ",\"have\":\"" + r.p[k].md5 + "\"";
      j += "}";
    }
    j += "]}";
  }
  return webtransfer::reply(200, j + "]}");
}

// ---- POST /files/bios/put ----------------------------------------------------------------------------------------------

/// The entry of the list named `file`, with the md5s of every system that lists it.
bool find_entry(const string& file, BiosFile& out) {
  bool found = false;
  for (const BiosSystem& b : list_systems())
    for (const BiosFile& f : b.files) {
      if (f.file != file) continue;
      if (!found) {
        out = f;
        found = true;
        continue;
      }
      for (const string& m : f.md5s)
        if (std::find(out.md5s.begin(), out.md5s.end(), m) == out.md5s.end()) out.md5s.push_back(m);
      out.any = out.any || f.any;
      out.zipped = out.zipped || f.zipped;
    }
  return found;
}

constexpr uint64_t kMaxBios = 256ull * 1024 * 1024;

http::Response put_route(const http::Request& req, http::BodyStream& body, const webtransfer::Env& env) {
  using webtransfer::err;
  string file, f1, f2;
  if (!http::query_param(req, "file", file)) return err(400, "file is required");
  const bool force = http::query_param(req, "force", f1) && f1 == "1";
  const bool replace = http::query_param(req, "replace", f2) && f2 == "1";
  BiosFile ent;
  std::vector<string> comps;
  if (!find_entry(file, ent) || !safe_rel(file, comps)) return err(400, "not a BIOS file of the list");
  const uint64_t len = body.total();
  if (len == 0) return err(400, "empty file");
  if (len > kMaxBios) return err(413, "too big for a BIOS file");
  const string root = root_dir();
  int root_fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (root_fd < 0) return err(400, "the ROM folder does not exist");
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } root_closer{root_fd};

  // An existing file that is right (or a zip, which cannot be checked) is only replaced on purpose, with `replace=1`.
  bool existed = false;
  {
    int d = walk(root_fd, comps, comps.size() - 1, false, nullptr);
    struct stat sb;
    if (d >= 0) {
      existed = ::fstatat(d, comps.back().c_str(), &sb, AT_SYMLINK_NOFOLLOW) == 0;
      ::close(d);
      if (existed && !S_ISREG(sb.st_mode)) return err(400, "not a regular file");
    }
    if (existed && !replace) {
      const State st = probe(root_fd, root, ent).st;
      if (st == State::Ok || st == State::Unchecked) return err(409, "that BIOS file is already there");
    }
  }
  uint64_t avail = 0;
  const bool known = env.free_space ? env.free_space(root, avail) : webtransfer::real_free_space(root, avail);
  if (!known || avail < len) return err(413, "not enough free space");

  int made = -1;
  const size_t nd = comps.size() - 1;
  int dir = walk(root_fd, comps, nd, true, &made);
  if (dir < 0) {
    unmake(root_fd, comps, made, nd);
    return err(400, "cannot use that folder");
  }
  const string name = comps.back(), part = name + ".part";
  auto fail = [&](int fd, http::Response r) {
    if (fd >= 0) ::close(fd);
    ::unlinkat(dir, part.c_str(), 0);
    ::close(dir);
    unmake(root_fd, comps, made, nd);
    return r;
  };
  ::unlinkat(dir, part.c_str(), 0);  // leftover of an interrupted upload (only one runs at a time)
  int fd = ::openat(dir, part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd < 0) return fail(-1, err(500, "cannot create the file"));
  body.accept();
  scraper::Md5 h;
  std::vector<char> buf(64 * 1024);
  uint64_t written = 0;
  for (;;) {
    ptrdiff_t n = body.read(buf.data(), buf.size());
    if (n == 0) break;
    if (n < 0) return fail(fd, err(body.timed_out() ? 408 : 400, "upload interrupted"));
    h.update(buf.data(), size_t(n));
    const char* p = buf.data();
    size_t left = size_t(n);
    while (left > 0) {
      ssize_t w = ::write(fd, p, left);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        const bool full = errno == ENOSPC;
        return fail(fd, err(full ? 413 : 500, full ? "not enough free space" : "write failed"));
      }
      p += w;
      left -= size_t(w);
    }
    written += uint64_t(n);
  }
  const string md5 = h.hex();
  struct stat fb;
  if (::fsync(fd) != 0 || ::fstat(fd, &fb) != 0 || written != len) return fail(fd, err(500, "could not save the file"));
  const bool right = ent.zipped || ent.any || std::find(ent.md5s.begin(), ent.md5s.end(), md5) != ent.md5s.end();
  if (!right && !force) {
    http::Response r = fail(fd, err(422, "not the expected file"));
    r.body = "{\"error\":\"not the expected file\",\"md5\":\"" + md5 + "\"}";
    return r;
  }
  struct stat sb;
  const bool there = ::fstatat(dir, name.c_str(), &sb, AT_SYMLINK_NOFOLLOW) == 0;
  if (there && !S_ISREG(sb.st_mode)) return fail(fd, err(400, "not a regular file"));
  if (there && !existed && !replace) return fail(fd, err(409, "that BIOS file is already there"));  // appeared while we were receiving
  if (::close(fd) != 0 || ::renameat(dir, part.c_str(), dir, name.c_str()) != 0) {
    ::unlinkat(dir, part.c_str(), 0);
    ::close(dir);
    unmake(root_fd, comps, made, nd);
    return err(500, "could not save the file");
  }
  ::close(dir);
  if (!ent.zipped) remember(root + "/" + file, fb, md5);  // the next list need not read it again
  const char* st = ent.zipped ? "unchecked" : right ? "ok" : "wrong";
  return webtransfer::reply(200, "{\"ok\":true,\"file\":\"" + oscontract::json_escape(file) + "\",\"status\":\"" + st + "\",\"md5\":\"" + md5 +
                                     "\",\"bytes\":" + std::to_string(written) + "}");
}

}  // namespace

size_t hash_count() { return g_hashes.load(); }

// ---- the page ----------------------------------------------------------------------------------------------------------

ZF_REGISTER_WEB_MODULE([] {
  webtransfer::WebModule m;
  m.name = "bios";
  m.order = 40;
  m.owns = [](std::string_view p) { return p == "/files/bios"; };
  m.route = [](const http::Request& req, const webtransfer::Env&) {
    if (req.method != "GET") return webtransfer::err(405, "method not allowed");
    return list_route(req);
  };
  m.stream_owns = [](std::string_view meth, std::string_view p) { return meth == "POST" && p == "/files/bios/put"; };
  m.stream = [](const http::Request& req, http::BodyStream& body, const webtransfer::Env& env) { return put_route(req, body, env); };
  m.tab_id = "bi";
  m.tab_label = "BIOS";
  m.css = R"CSS(#bisys details{border-top:1px solid var(--line);padding:.4rem 0}#bisys summary{cursor:pointer;font-weight:600}
#bisys li{display:block}#bisys .st{font-weight:600;margin-left:.4rem}#bisys .st.ok{color:var(--ok)}#bisys .st.wrong,#bisys .st.missing{color:var(--bad)}
#bisys .fl{display:block;font-size:.8rem;opacity:.75;margin-top:.15rem}#bisys input[type=file]{margin-top:.3rem;padding:.35rem}#bisum{font-weight:600})CSS";
  m.section = R"HTML(<p id="bisum"></p>
<label><input type="checkbox" id="biall">Show all systems</label>
<div id="bisys"></div>)HTML";
  m.js = R"JS(
var busy=false,queue=[];
function el(t,c,x){var e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e}
function put(file,blob,replace,force,done){
 var q="/files/bios/put?file="+encodeURIComponent(file)+(replace?"&replace=1":"")+(force?"&force=1":"");
 var x=new XMLHttpRequest();x.open("POST",q);Z.auth(x);
 x.onload=function(){done(x)};x.onerror=function(){Z.say("Cannot reach the console.",true);done(null)};x.send(blob)}
function next(){
 if(busy||!queue.length)return;
 var it=queue.shift();busy=true;Z.say("Sending "+it.blob.name+"...");
 put(it.f.file,it.blob,it.replace,it.force,function(x){busy=false;
  if(x&&x.status===422&&!it.force){var h="";try{h=JSON.parse(x.responseText).md5}catch(_){}
   if(confirm("This is not the expected file. Save it anyway?"+(h?"\n(md5 "+h+")":""))){it.force=true;queue.unshift(it)}else{Z.say("Not saved.",true)}
   next();return}
  if(x&&!Z.jerr(x)){
   var r={};try{r=JSON.parse(x.responseText)}catch(_){}
   Z.say(r.status==="wrong"?"Saved "+it.f.file+", but it is not the expected file and may not work.":"Saved "+it.f.file+".",r.status==="wrong");
   list()}
  next()})}
function send(f,blob,replace){queue.push({f:f,blob:blob,replace:replace,force:false});next()}
function row(f){var li=el("li");li.appendChild(document.createTextNode(f.file));
 var s=f.status==="unchecked"?"there (zip, not checked)":f.status==="wrong"?"wrong file":f.status==="ok"?"OK":"missing";
 li.appendChild(el("span","st "+f.status,s));
 li.appendChild(el("span","fl",f.zipped||f.any||!f.md5.length?"":"md5 "+f.md5.join(" or ")));
 var inp=el("input");inp.type="file";inp.onchange=function(){var b=inp.files[0];if(b)send(f,b,f.status==="ok"||f.status==="unchecked");inp.value=""};
 li.appendChild(inp);return li}
function draw(d){
 var parts=[];if(d.missing)parts.push(d.missing+" missing");if(d.wrong)parts.push(d.wrong+" wrong");if(d.unchecked)parts.push(d.unchecked+" in zip files, not checked");
 Z.$("bisum").textContent=(d.missing||d.wrong)?parts.join(", "):("All BIOS files are fine."+(d.unchecked?" "+parts.join(", ")+".":""));
 var root=Z.$("bisys");root.textContent="";
 if(!d.systems.length){root.appendChild(el("p",null,"No systems with games."));return}
 d.systems.forEach(function(s){var de=el("details");if(s.problems)de.open=true;
  de.appendChild(el("summary",null,s.name+" - "+(s.problems?s.problems+" to fix":"OK")));
  var ul=el("ul");s.files.forEach(function(f){ul.appendChild(row(f))});de.appendChild(ul);root.appendChild(de)})}
function list(){var n=Z.nextReq();
 Z.call("GET","/files/bios"+(Z.$("biall").checked?"?all=1":""),function(x){
  if(!Z.isReq(n)||!x)return;if(Z.jerr(x))return;
  var d=null;try{d=JSON.parse(x.responseText)}catch(_){}
  if(d)draw(d)})}
Z.$("biall").onchange=list;
Z.ontab.bi=list;
)JS";
  return m;
});

}  // namespace webbios
}  // namespace zf
