// MODULE `webput` (web module of webtransfer): upload of whole folders and resumable uploads.
//
//   POST /files/put?system=<name>&path=<relative path>&offset=<N>&total=<T>[&replace=1][&final=1]   body = bytes N.. of the file,
//        STREAMED, at most 16 MB per request (the page sends 8 MB slices)
//   GET  /files/put/status?system=<name>&path=<relative path>   {"have":<bytes received, 0 if none>,"done":<target exists>,
//        "size":<target size, 0 if none>}
//   POST /files/put/rescan       read the game folders again (the page asks when the last file of a batch failed)
//
// `path` is `name` or `folder/.../name`: at most 4 components below the system's ROM folder, each one passes `valid_name`
// (no "..", no leading '.', no control characters, no '/' or backslash inside), 400 bytes in all. The client never names a place
// on disk: folders are created INSIDE the ROM folder only, one step at a time with mkdirat + openat(O_DIRECTORY | O_NOFOLLOW)
// from an fd of the ROM folder, and the file is opened with openat(O_NOFOLLOW) from its folder fd, so a symlink that already
// lies in the ROM folder (a folder or the file itself) can never take a write elsewhere. A file at the top level needs an
// extension the system lists (as the old /files/upload); inside a folder any extension is fine (.bin/.cue/.m3u of multi-disc
// games, PC games). The bytes go to the hidden `.<target>.zfpart` next to it (a dot file: never a game, never listed; the suffix
// is ours alone, so the sweep below can only remove our own leftovers). `offset` must equal the size of that file (0 starts
// again: it is truncated), else 409 with {"have":<size>}, which is where a client continues after a cut connection. Starting a
// file (offset 0) also removes the `.*.zfpart` left in that folder for more than 2 days (an upload nobody resumed). The last slice
// (offset + len == total) is fsynced and renamed over the target (an existing target only with `replace=1`, else 409). A slice
// cut in the middle keeps what arrived. `final=1` on the last file of a batch asks for the rescan of the game folders, once per
// batch; when that file fails the page asks with /files/put/rescan instead. "size" lets a client whose answer to the last slice
// was lost tell its own finished file (done, size == total) from one that was already there. Every route needs the PIN and answers 404 while the transfer is off (the core does both).
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "oscontract.hpp"
#include "webcore.hpp"

namespace zf {
namespace webtransfer {
namespace {

constexpr uint64_t kMaxSlice = 16u << 20;
constexpr size_t kMaxPathBytes = 400;
constexpr size_t kMaxParts = 4;
constexpr time_t kStaleSecs = 2 * 24 * 3600;
const char kPartSuffix[] = ".zfpart";

struct Fd {
  int v = -1;
  Fd() = default;
  explicit Fd(int fd) : v(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() { reset(-1); }
  void reset(int fd) {
    if (v >= 0) ::close(v);
    v = fd;
  }
  int release() {
    int r = v;
    v = -1;
    return r;
  }
};

bool parse_u64(const std::string& s, uint64_t& out) {
  if (s.empty() || s.size() > 18) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + uint64_t(c - '0');
  }
  out = v;
  return true;
}

bool split_path(const std::string& path, std::vector<std::string>& parts) {
  parts.clear();
  if (path.empty() || path.size() > kMaxPathBytes) return false;
  size_t start = 0;
  for (;;) {
    size_t slash = path.find('/', start);
    std::string c = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!valid_name(c)) return false;  // also catches an empty component (leading, trailing or double '/')
    parts.push_back(c);
    if (parts.size() > kMaxParts) return false;
    if (slash == std::string::npos) return true;
    start = slash + 1;
  }
}

enum class Dirs { Ok, Missing, Bad };

/// The fd of the folder that holds the file `parts.back()`: every folder is opened (and with `create`, made) relative to the one
/// before, never through a symlink.
Dirs open_dirs(const std::string& root, const std::vector<std::string>& parts, bool create, Fd& out) {
  Fd cur(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (cur.v < 0) return Dirs::Bad;
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    if (create && ::mkdirat(cur.v, parts[i].c_str(), 0755) != 0 && errno != EEXIST) return Dirs::Bad;
    int nfd = ::openat(cur.v, parts[i].c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (nfd < 0) return errno == ENOENT && !create ? Dirs::Missing : Dirs::Bad;
    cur.reset(nfd);
  }
  out.reset(cur.release());
  return Dirs::Ok;
}

/// system + path from the query, checked; false with `deny` filled.
bool target_of(const http::Request& req, SystemInfo& si, std::vector<std::string>& parts, http::Response& deny) {
  std::string sysname, path;
  if (!http::query_param(req, "system", sysname) || !http::query_param(req, "path", path)) {
    deny = err(400, "system and path are required");
    return false;
  }
  if (!split_path(path, parts)) {
    deny = err(400, "invalid path");
    return false;
  }
  if (!find_system(sysname, si) || si.rom_dir.empty()) {
    deny = err(400, "unknown system");
    return false;
  }
  if (parts.size() == 1 && !ext_allowed(parts[0], si.exts)) {
    deny = err(400, "this system does not accept that file type");
    return false;
  }
  return true;
}

/// Where the bytes of `name` gather until the last slice: hidden, with a suffix only this module uses.
std::string part_of(const std::string& name) { return "." + name + kPartSuffix; }

/// Removes the `.*.zfpart` regular files of the folder `dir` not written for kStaleSecs (never `keep`, never anything else).
void sweep_stale(int dir, const std::string& keep) {
  int fd = ::openat(dir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return;
  DIR* d = ::fdopendir(fd);
  if (!d) {
    ::close(fd);
    return;
  }
  const time_t now = ::time(nullptr);
  const size_t sl = sizeof kPartSuffix - 1;
  std::vector<std::string> old;
  while (dirent* e = ::readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() <= sl + 1 || n[0] != '.' || n.compare(n.size() - sl, sl, kPartSuffix) != 0 || n == keep) continue;
    struct stat sb;
    if (::fstatat(dir, n.c_str(), &sb, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(sb.st_mode) && now - sb.st_mtime > kStaleSecs) old.push_back(n);
  }
  ::closedir(d);
  for (const std::string& n : old) ::unlinkat(dir, n.c_str(), 0);
}

http::Response have_error(uint64_t have) {
  return reply(409, "{\"error\":\"the offset does not match what the console has\",\"have\":" + std::to_string(have) + "}");
}

http::Response put(const http::Request& req, http::BodyStream& body, const Env& env) {
  SystemInfo si;
  std::vector<std::string> parts;
  http::Response deny;
  if (!target_of(req, si, parts, deny)) return deny;
  std::string a, b, rep, fin;
  uint64_t offset = 0, total = 0;
  if (!http::query_param(req, "offset", a) || !parse_u64(a, offset) || !http::query_param(req, "total", b) || !parse_u64(b, total))
    return err(400, "offset and total are required");
  const bool replace = http::query_param(req, "replace", rep) && rep == "1";
  const bool final_file = http::query_param(req, "final", fin) && fin == "1";
  const uint64_t len = body.total();
  if (len > kMaxSlice) return err(413, "slice too big");
  if (offset > total || len > total - offset) return err(400, "offset and length go past the total");
  if (len == 0 && total > 0 && offset != total) return err(400, "empty slice");
  if (total == 0 && parts.size() == 1) return err(400, "empty file");
  struct stat sb;
  if (::stat(si.rom_dir.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) return err(400, "the ROM folder does not exist");
  uint64_t avail = 0;
  const bool known = env.free_space ? env.free_space(si.rom_dir, avail) : real_free_space(si.rom_dir, avail);
  if (!known || avail < total - offset) return err(413, "not enough free space");

  Fd dir;
  if (open_dirs(si.rom_dir, parts, true, dir) != Dirs::Ok) return err(400, "cannot use that folder");
  const std::string& name = parts.back();
  const std::string part = part_of(name);
  if (::fstatat(dir.v, name.c_str(), &sb, AT_SYMLINK_NOFOLLOW) == 0) {
    if (!replace) return err(409, "a file with that name already exists");
    if (!S_ISREG(sb.st_mode)) return err(400, "not a regular file");
  }
  Fd fd;
  if (offset == 0) {
    sweep_stale(dir.v, part);
    fd.reset(::openat(dir.v, part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644));
    if (fd.v < 0) return err(500, "cannot create the file");
  } else {
    fd.reset(::openat(dir.v, part.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.v < 0) return errno == ENOENT ? have_error(0) : err(500, "cannot open the file");
    if (::fstat(fd.v, &sb) != 0 || !S_ISREG(sb.st_mode)) return err(500, "cannot open the file");
    if (uint64_t(sb.st_size) != offset) return have_error(uint64_t(sb.st_size));
    if (::lseek(fd.v, off_t(offset), SEEK_SET) < 0) return err(500, "cannot open the file");
  }
  body.accept();  // only now does a client waiting for "100 Continue" send the data
  std::vector<char> buf(64 * 1024);
  uint64_t written = 0;
  for (;;) {
    ptrdiff_t n = body.read(buf.data(), buf.size());
    if (n == 0) break;
    if (n < 0) return err(body.timed_out() ? 408 : 400, "upload interrupted");  // what arrived stays: the client resumes
    const char* p = buf.data();
    size_t left = size_t(n);
    while (left > 0) {
      ssize_t w = ::write(fd.v, p, left);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        const bool full = errno == ENOSPC;
        fd.reset(-1);
        ::unlinkat(dir.v, part.c_str(), 0);
        return err(full ? 413 : 500, full ? "not enough free space" : "write failed");
      }
      p += w;
      left -= size_t(w);
    }
    written += uint64_t(n);
  }
  const bool synced = ::fsync(fd.v) == 0;
  const bool closed = ::close(fd.release()) == 0;
  if (!synced || !closed || written != len) {
    ::unlinkat(dir.v, part.c_str(), 0);
    return err(500, "could not save the file");
  }
  const bool complete = offset + len == total;
  if (complete) {
    if (!replace && ::fstatat(dir.v, name.c_str(), &sb, AT_SYMLINK_NOFOLLOW) == 0) {  // appeared while we were receiving
      ::unlinkat(dir.v, part.c_str(), 0);
      return err(409, "a file with that name already exists");
    }
    if (::renameat(dir.v, part.c_str(), dir.v, name.c_str()) != 0) {
      ::unlinkat(dir.v, part.c_str(), 0);
      return err(500, "could not save the file");
    }
    if (final_file && env.reload) env.reload();
  }
  return reply(200, "{\"ok\":true,\"name\":\"" + oscontract::json_escape(name) + "\",\"bytes\":" + std::to_string(len) +
                        ",\"have\":" + std::to_string(offset + len) + ",\"done\":" + (complete ? "true" : "false") + "}");
}

http::Response status(const http::Request& req, const Env&) {
  if (req.method != "GET") return err(405, "method not allowed");
  SystemInfo si;
  std::vector<std::string> parts;
  http::Response deny;
  if (!target_of(req, si, parts, deny)) return deny;
  Fd dir;
  const Dirs d = open_dirs(si.rom_dir, parts, false, dir);
  if (d == Dirs::Bad) return err(400, "cannot use that folder");
  uint64_t have = 0, size = 0;
  bool done = false;
  if (d == Dirs::Ok) {
    struct stat sb;
    if (::fstatat(dir.v, part_of(parts.back()).c_str(), &sb, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(sb.st_mode)) have = uint64_t(sb.st_size);
    done = ::fstatat(dir.v, parts.back().c_str(), &sb, AT_SYMLINK_NOFOLLOW) == 0;
    if (done && S_ISREG(sb.st_mode)) size = uint64_t(sb.st_size);
  }
  return reply(200, "{\"have\":" + std::to_string(have) + ",\"done\":" + (done ? "true" : "false") + ",\"size\":" +
                        std::to_string(size) + "}");
}

http::Response rescan(const http::Request& req, const Env& env) {
  if (req.method != "POST") return err(405, "method not allowed");
  if (env.reload) env.reload();
  return reply(200, "{\"ok\":true}");
}

}  // namespace

ZF_REGISTER_WEB_MODULE([] {
  WebModule m;
  m.name = "put";
  m.order = 10;
  m.owns = [](std::string_view path) { return path == "/files/put/status" || path == "/files/put/rescan"; };
  m.route = [](const http::Request& req, const Env& env) { return req.path == "/files/put/rescan" ? rescan(req, env) : status(req, env); };
  m.stream_owns = [](std::string_view method, std::string_view path) { return method == "POST" && path == "/files/put"; };
  m.stream = [](const http::Request& req, http::BodyStream& body, const Env& env) { return put(req, body, env); };
  return m;
});

}  // namespace webtransfer
}  // namespace zf
