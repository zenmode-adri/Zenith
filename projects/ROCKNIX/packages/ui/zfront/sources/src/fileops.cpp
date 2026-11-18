#include "fileops.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace zf::fileops {
namespace {

constexpr size_t kChunk = 256 * 1024;

std::string base_of(const std::string& p) {
  const size_t k = p.rfind('/');
  return k == std::string::npos ? p : p.substr(k + 1);
}
std::string dir_of(const std::string& p) {
  const size_t k = p.rfind('/');
  if (k == std::string::npos) return ".";
  return k == 0 ? "/" : p.substr(0, k);
}
std::string join(const std::string& d, const std::string& n) { return d == "/" ? "/" + n : d + "/" + n; }

std::string real(const std::string& p) {
  char buf[PATH_MAX];
  return ::realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}

bool exists_nofollow(const std::string& p) {
  struct stat st;
  return ::lstat(p.c_str(), &st) == 0;
}

Err err_of(int e) { return err_from_errno(e, false); }
Err err_create(int e) { return err_from_errno(e, true); }

constexpr uint64_t kFat32Max = 4ull * 1024 * 1024 * 1024;  // a FAT32 file is at most 4 GB - 1
constexpr long kMsdosMagic = 0x4d44;  // statfs f_type of msdos / vfat
constexpr long kStalePartSeconds = 30;  // a "<name>.part" not touched for this long (or dated in the future) is a leftover

// ---- natural order ----------------------------------------------------------------------------------------------------------------------------

// The base letter of the Latin-1 letters U+00C0..U+00FF ('?' = not a letter with an accent), in both cases.
constexpr char kFold[65] =
    "aaaaaa?c" "eeeeiiii" "?nooooo?" "ouuuuy??"
    "aaaaaa?c" "eeeeiiii" "?nooooo?" "ouuuuy?y";

// The folded character at s[i] (a letter with an accent becomes its base letter, upper case becomes lower), and `i` moves past it.
int fold_at(const std::string& s, size_t& i) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  if (c == 0xC3 && i + 1 < s.size()) {
    const unsigned char d = static_cast<unsigned char>(s[i + 1]);
    if (d >= 0x80 && d <= 0xBF && kFold[d - 0x80] != '?') {
      i += 2;
      return kFold[d - 0x80];
    }
  }
  ++i;
  if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
  return c;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// ---- sizes ----------------------------------------------------------------------------------------------------------------------------------

struct Sizes {
  uint64_t bytes = 0, biggest = 0;
  std::string big;  // the biggest file
};

void tree_size(const std::string& path, Sizes& z) {
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    if (DIR* d = ::opendir(path.c_str())) {
      while (const dirent* e = ::readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        tree_size(join(path, e->d_name), z);
      }
      ::closedir(d);
    }
  } else if (S_ISREG(st.st_mode)) {
    z.bytes += uint64_t(st.st_size);
    if (uint64_t(st.st_size) > z.biggest) z.biggest = uint64_t(st.st_size), z.big = path;
  }
}

// ---- walking --------------------------------------------------------------------------------------------------------------------------------

void walk(const std::string& path, Progress* pr) {
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    if (DIR* d = ::opendir(path.c_str())) {
      while (const dirent* e = ::readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        if (pr && pr->cancel) break;
        walk(join(path, e->d_name), pr);
      }
      ::closedir(d);
    }
  } else if (pr) {
    pr->files_total++;
    if (S_ISREG(st.st_mode)) pr->bytes_total += uint64_t(st.st_size);
  }
}

Err copy_file(const std::string& src, const std::string& dst, const struct stat& st, Progress& pr) {
  pr.set_current(base_of(src));
  const int in = ::open(src.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (in < 0) {
    pr.set_bad(src);
    return Err::Io;
  }
  const std::string tmp = dst + ".part";
  int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (out < 0 && errno == EEXIST) {  // what an earlier copy left when the console froze: a regular file nobody writes any more (one job at a time here;
    struct stat ps;                  // a web upload also writes "<name>.part", and it is alive: it touched the file in the last half minute)
    const std::time_t now = std::time(nullptr);
    if (::lstat(tmp.c_str(), &ps) == 0 && S_ISREG(ps.st_mode) && (ps.st_mtime > now || now - ps.st_mtime >= kStalePartSeconds) && ::unlink(tmp.c_str()) == 0)
      out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  }
  if (out < 0) {
    const int e = errno;
    ::close(in);
    pr.set_bad(src);
    return err_create(e);
  }
  std::vector<char> buf(kChunk);
  Err r = Err::None;
  for (;;) {
    if (pr.cancel) {
      r = Err::Cancelled;
      break;
    }
    const ssize_t n = ::read(in, buf.data(), buf.size());
    if (n < 0) {
      r = Err::Io;
      break;
    }
    if (n == 0) break;
    ssize_t off = 0;
    while (off < n) {
      const ssize_t w = ::write(out, buf.data() + off, size_t(n - off));
      if (w < 0) {
        r = err_of(errno);
        break;
      }
      off += w;
    }
    if (r != Err::None) break;
    pr.bytes_done += uint64_t(n);
  }
  ::close(in);
  if (r == Err::None) {
    ::fchmod(out, st.st_mode & 07777);
    const timespec ts[2] = {st.st_atim, st.st_mtim};
    ::futimens(out, ts);
  }
  if (::close(out) != 0 && r == Err::None) r = err_of(errno);
  if (r == Err::None && ::rename(tmp.c_str(), dst.c_str()) != 0) r = err_create(errno);
  if (r != Err::None) {
    ::unlink(tmp.c_str());
    pr.set_bad(src);
  }
  if (r == Err::None) pr.files_done++;
  return r;
}

Err remove_tree(const std::string& path, Progress& pr) {
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) return errno == ENOENT ? Err::None : Err::Io;
  if (S_ISDIR(st.st_mode)) {
    if (DIR* d = ::opendir(path.c_str())) {
      Err r = Err::None;
      while (const dirent* e = ::readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        if (pr.cancel) {
          r = Err::Cancelled;
          break;
        }
        r = remove_tree(join(path, e->d_name), pr);
        if (r != Err::None) break;
      }
      ::closedir(d);
      if (r != Err::None) return r;
    }
    if (::rmdir(path.c_str()) != 0) {
      const int e = errno;
      pr.set_bad(path);
      return e == ENOTEMPTY || e == EEXIST ? Err::Io : err_of(e);
    }
    return Err::None;
  }
  pr.set_current(base_of(path));
  if (::unlink(path.c_str()) != 0) {
    const int e = errno;
    pr.set_bad(path);
    return err_of(e);
  }
  pr.files_done++;
  return Err::None;
}

Err copy_tree(const std::string& src, const std::string& dst, Progress& pr) {
  struct stat st;
  if (::lstat(src.c_str(), &st) != 0) return Err::NotFound;
  if (S_ISLNK(st.st_mode)) {  // the link itself is copied, never what it points to
    char buf[PATH_MAX];
    const ssize_t n = ::readlink(src.c_str(), buf, sizeof buf - 1);
    if (n < 0) {
      pr.set_bad(src);
      return Err::Io;
    }
    buf[n] = 0;
    const int rc = hooks().symlink ? hooks().symlink(buf, dst.c_str()) : ::symlink(buf, dst.c_str());
    if (rc != 0) {
      const int e = errno;
      if (e == EPERM || e == ENOTSUP || e == EOPNOTSUPP) {  // FAT / exFAT have no links: left out, and told at the end
        pr.links_skipped++;
        return Err::None;
      }
      pr.set_bad(src);
      return err_create(e);
    }
    pr.files_done++;
    return Err::None;
  }
  if (S_ISREG(st.st_mode)) return copy_file(src, dst, st, pr);
  if (!S_ISDIR(st.st_mode)) return Err::None;  // sockets, devices, pipes: left alone
  if (::mkdir(dst.c_str(), (st.st_mode & 07777) | 0700) != 0) {
    const int e = errno;
    pr.set_bad(src);
    return err_create(e);
  }
  DIR* d = ::opendir(src.c_str());
  if (!d) {
    pr.set_bad(src);
    return Err::Io;
  }
  Err r = Err::None;
  while (const dirent* e = ::readdir(d)) {
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
    r = copy_tree(join(src, e->d_name), join(dst, e->d_name), pr);
    if (r != Err::None) break;
  }
  ::closedir(d);
  return r;
}

}  // namespace

// ---- errors and seams -----------------------------------------------------------------------------------------------------------------------

Err err_from_errno(int e, bool creating) {
  if (e == ENOSPC || e == EDQUOT) return Err::NoSpace;
  if (e == EFBIG) return Err::TooBig;
  if (e == EROFS || e == EACCES) return Err::ReadOnly;
  if (e == ENOENT) return Err::NotFound;
  if (e == EEXIST) return Err::Exists;
  if (creating && (e == EPERM || e == EINVAL || e == ENAMETOOLONG || e == EILSEQ)) return Err::FsName;
  return Err::Io;
}

Hooks& hooks() {
  static Hooks h;
  return h;
}

bool is_fat32(const std::string& path) {
  if (hooks().is_fat32) return hooks().is_fat32(path);
  struct statfs sf;
  return ::statfs(path.c_str(), &sf) == 0 && long(sf.f_type) == kMsdosMagic;
}

int natural_compare(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    const bool da = is_digit(a[i]), db = is_digit(b[j]);
    if (da && db) {  // two runs of digits: by value (leading zeros do not count)
      size_t ie = i, je = j;
      while (ie < a.size() && is_digit(a[ie])) ++ie;
      while (je < b.size() && is_digit(b[je])) ++je;
      size_t ia = i, jb = j;
      while (ia + 1 < ie && a[ia] == '0') ++ia;
      while (jb + 1 < je && b[jb] == '0') ++jb;
      if (ie - ia != je - jb) return ie - ia < je - jb ? -1 : 1;
      const int c = std::memcmp(a.data() + ia, b.data() + jb, ie - ia);
      if (c != 0) return c < 0 ? -1 : 1;
      i = ie, j = je;
      continue;
    }
    const int ca = da ? '0' : fold_at(a, i), cb = db ? '0' : fold_at(b, j);  // a digit run sits where '0' does among the other characters
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  const int c = a.compare(b);
  return c < 0 ? -1 : c > 0 ? 1 : 0;
}

// ---- paths ----------------------------------------------------------------------------------------------------------------------------------

std::string normalize(const std::string& path) {
  if (path.empty() || path[0] != '/') return path;
  std::vector<std::string> parts;
  size_t i = 0;
  while (i <= path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    const std::string c = path.substr(i, j - i);
    if (c == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!c.empty() && c != ".") {
      parts.push_back(c);
    }
    i = j + 1;
  }
  std::string out;
  for (const std::string& c : parts) out += "/" + c;
  return out.empty() ? "/" : out;
}

bool within(const std::string& path, const std::string& root) {
  const std::string p = normalize(path), r = normalize(root);
  if (p == r || r == "/") return true;
  return p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/';
}

static bool within_roots(const Policy& pol, const std::string& path) {
  for (const std::string& r : pol.roots)
    if (within(path, r)) return true;
  return false;
}

bool inside_real(const Policy& pol, const std::string& path) {
  const std::string rp = real(path);
  if (rp.empty()) return false;
  for (const std::string& r : pol.roots) {
    const std::string rr = real(r);
    if (!rr.empty() && within(rp, rr)) return true;
  }
  return false;
}

bool allowed(const Policy& pol, const std::string& path) {
  const std::string n = normalize(path);
  if (!within_roots(pol, n)) return false;
  for (const std::string& r : pol.roots)
    if (normalize(r) == n) return true;
  return inside_real(pol, dir_of(n));  // the folder it sits in, not the entry (a link out of the roots can still be removed)
}

bool can_change(const Policy& pol, const std::string& path) {
  if (!allowed(pol, path)) return false;
  const std::string n = normalize(path);
  for (const std::string& r : pol.roots)
    if (normalize(r) == n) return false;
  for (const std::string& k : pol.keep)
    if (normalize(k) == n) return false;
  for (const std::string& k : pol.keep_sub)  // the system folders under the roms folder: the folder goes nowhere, what is inside it may
    if (normalize(k) == dir_of(n)) {
      struct stat ks;
      if (::lstat(n.c_str(), &ks) == 0 && (S_ISDIR(ks.st_mode) || S_ISLNK(ks.st_mode))) return false;
    }
  struct stat a, b;  // a USB drive or a second card: its own file system, so "delete" would empty the whole device
  if (::lstat(n.c_str(), &a) == 0 && ::lstat(dir_of(n).c_str(), &b) == 0 && a.st_dev != b.st_dev) return false;
  return true;
}

bool needs_warning(const Policy& pol, const std::string& path) {
  const std::string n = normalize(path);
  for (const std::string& w : pol.warn)
    if (n != normalize(w) && within(n, w)) return true;
  return false;
}

bool valid_name(const std::string& name) {
  return !name.empty() && name.size() <= 255 && name != "." && name != ".." && name.find('/') == std::string::npos && name.find('\0') == std::string::npos;
}

std::string unique_name(const std::string& dir, const std::string& name) {
  if (!exists_nofollow(join(dir, name))) return name;
  const size_t dot = name.rfind('.');
  const bool ext = dot != std::string::npos && dot > 0;
  const std::string stem = ext ? name.substr(0, dot) : name, tail = ext ? name.substr(dot) : std::string();
  for (int i = 2; i < 10000; ++i) {
    const std::string c = stem + " (" + std::to_string(i) + ")" + tail;
    if (!exists_nofollow(join(dir, c))) return c;
  }
  return name + ".new";
}

// ---- listing --------------------------------------------------------------------------------------------------------------------------------

bool list(const std::string& dir, Sort sort, bool with_hidden, std::vector<Entry>& out) {
  out.clear();
  DIR* d = ::opendir(dir.c_str());
  if (!d) return false;
  while (const dirent* e = ::readdir(d)) {
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
    Entry en;
    en.name = e->d_name;
    en.hidden = en.name[0] == '.';
    if (en.hidden && !with_hidden) continue;
    const unsigned char dt = e->d_type;  // the kind comes with the name: only files (their size) and a sort by date need a stat of their own
    if (dt == DT_UNKNOWN || dt == DT_REG || sort == Sort::Date) {
      struct stat st;
      if (::lstat(join(dir, en.name).c_str(), &st) != 0) continue;
      en.link = S_ISLNK(st.st_mode);
      en.dir = S_ISDIR(st.st_mode);
      en.size = S_ISREG(st.st_mode) ? uint64_t(st.st_size) : 0;
      en.mtime = st.st_mtime;
    } else {
      en.link = dt == DT_LNK;
      en.dir = dt == DT_DIR;
    }
    out.push_back(std::move(en));
  }
  ::closedir(d);
  std::sort(out.begin(), out.end(), [sort](const Entry& a, const Entry& b) {
    if (a.dir != b.dir) return a.dir;
    if (sort == Sort::Size && a.size != b.size) return a.size > b.size;
    if (sort == Sort::Date && a.mtime != b.mtime) return a.mtime > b.mtime;
    return natural_compare(a.name, b.name) < 0;
  });
  return true;
}

// ---- operations -----------------------------------------------------------------------------------------------------------------------------

void measure(const std::vector<std::string>& srcs, Progress* p) {
  if (p) {
    p->bytes_total = 0;
    p->files_total = 0;
  }
  for (const std::string& s : srcs) {
    if (p && p->cancel) return;
    walk(s, p);
  }
}

static Result transfer_impl(const Policy& pol, const std::vector<std::string>& srcs, const std::string& dst, bool move, Progress& pr) {
  Result res;
  res.total = int(srcs.size());
  if (!inside_real(pol, dst)) {
    res.err = Err::Outside;
    res.first = dst;
    return res;
  }
  const std::string dn = real(dst);
  {  // before anything is copied: a file that cannot exist there (FAT32, 4 GB) and room for what has to be copied (a move inside one file system is a rename)
    struct stat ds;
    const bool have_dst = ::stat(dn.c_str(), &ds) == 0;
    const bool fat = is_fat32(dn);
    uint64_t need = pr.bytes_total;
    Sizes big;
    if (move || fat) {
      need = 0;
      for (const std::string& s : srcs) {
        struct stat st;
        if (::lstat(s.c_str(), &st) != 0) continue;
        if (move && have_dst && st.st_dev == ds.st_dev) continue;
        Sizes z;
        tree_size(s, z);
        need += z.bytes;
        if (z.biggest > big.biggest) big = z;
      }
    }
    if (fat && big.biggest >= kFat32Max) {
      res.err = Err::TooBig;
      res.first = big.big;
      return res;
    }
    uint64_t fr = 0, tot = 0;
    if (need > 0 && space(dn, fr, tot) && need > fr) {
      res.err = Err::NoSpace;
      res.first = dst;
      return res;
    }
  }
  for (const std::string& s : srcs) {
    if (pr.cancel) {
      res.err = Err::Cancelled;
      return res;
    }
    res.first = s;
    if (!allowed(pol, s)) {
      res.err = Err::Outside;
      return res;
    }
    if (move && !can_change(pol, s)) {
      res.err = Err::Protected;
      return res;
    }
    struct stat st;
    if (::lstat(s.c_str(), &st) != 0) {
      res.err = Err::NotFound;
      return res;
    }
    if (S_ISDIR(st.st_mode)) {
      const std::string rs = real(s);
      if (!rs.empty() && within(dn, rs)) {  // a folder cannot go into itself (nor into what it holds)
        res.err = Err::Inside;
        return res;
      }
    }
    const std::string name = base_of(normalize(s));
    if (move && real(dir_of(normalize(s))) == dn) {  // moved to where it already is: nothing to do
      res.done++;
      continue;
    }
    const std::string target = join(dn, unique_name(dn, name));
    if (move && ::rename(s.c_str(), target.c_str()) == 0) {
      res.done++;
      res.first = target;
      continue;
    }
    if (move && errno != EXDEV) {
      res.err = err_create(errno);
      return res;
    }
    const int skipped_before = pr.links_skipped;
    Err e = copy_tree(s, target, pr);
    if (e != Err::None) {
      Progress scrap;
      remove_tree(target, scrap);  // the half made copy goes away; the source is untouched
      res.err = e;
      if (!pr.bad().empty()) res.first = pr.bad();
      return res;
    }
    if (move && pr.links_skipped > skipped_before) {  // the copy lacks links: deleting the source would lose them, so it stays
      res.kept = true;
      res.done++;
      res.first = target;
      continue;
    }
    if (move) {  // the copy is whole: the source goes, and a cancel no longer stops it (no whole copy next to a half deleted source)
      Progress scrap;
      e = remove_tree(s, scrap);
      if (e != Err::None) {
        res.err = e;
        if (!scrap.bad().empty()) res.first = scrap.bad();
        return res;
      }
    }
    res.done++;
    res.first = target;
  }
  return res;
}

Result transfer(const Policy& pol, const std::vector<std::string>& srcs, const std::string& dst, bool move, Progress& pr) {
  Result res = transfer_impl(pol, srcs, dst, move, pr);
  res.skipped = pr.links_skipped;
  return res;
}

Result remove(const Policy& pol, const std::vector<std::string>& srcs, Progress& pr) {
  Result res;
  res.total = int(srcs.size());
  for (const std::string& s : srcs) {
    res.first = s;
    if (!can_change(pol, s)) {
      res.err = allowed(pol, s) ? Err::Protected : Err::Outside;
      return res;
    }
    const Err e = remove_tree(normalize(s), pr);
    if (e != Err::None) {
      res.err = e;
      if (!pr.bad().empty()) res.first = pr.bad();
      return res;
    }
    res.done++;
  }
  return res;
}

Result rename_item(const Policy& pol, const std::string& path, const std::string& new_name) {
  Result res;
  res.first = path;
  if (!valid_name(new_name)) {
    res.err = Err::BadName;
    return res;
  }
  if (!can_change(pol, path)) {
    res.err = allowed(pol, path) ? Err::Protected : Err::Outside;
    return res;
  }
  const std::string n = normalize(path), target = join(dir_of(n), new_name);
  if (target == n) return res;
  if (exists_nofollow(target)) {
    struct stat a, b;  // the same file under another spelling (FAT / exFAT ignore case): only the case changes, so it is a rename and not a clash
    if (::lstat(n.c_str(), &a) != 0 || ::lstat(target.c_str(), &b) != 0 || a.st_dev != b.st_dev || a.st_ino != b.st_ino) {
      res.err = Err::Exists;
      return res;
    }
  }
  if (::rename(n.c_str(), target.c_str()) != 0) {
    res.err = err_create(errno);
    return res;
  }
  res.done = 1;
  res.first = target;
  return res;
}

Result make_dir(const Policy& pol, const std::string& parent, const std::string& name) {
  Result res;
  if (!valid_name(name)) {
    res.err = Err::BadName;
    return res;
  }
  if (!inside_real(pol, parent)) {
    res.err = Err::Outside;
    return res;
  }
  const std::string target = join(real(parent), name);
  res.first = target;
  if (::mkdir(target.c_str(), 0755) != 0) {
    res.err = err_create(errno);
    return res;
  }
  res.done = 1;
  return res;
}

int count_entries(const std::string& dir, bool with_hidden) {
  DIR* d = ::opendir(dir.c_str());
  if (!d) return -1;
  int n = 0;
  while (const dirent* e = ::readdir(d)) {
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
    if (e->d_name[0] == '.' && !with_hidden) continue;
    ++n;
  }
  ::closedir(d);
  return n;
}

bool space(const std::string& path, uint64_t& free_bytes, uint64_t& total_bytes) {
  if (hooks().space) return hooks().space(path, free_bytes, total_bytes);
  struct statvfs vf;
  if (::statvfs(path.c_str(), &vf) != 0) return false;
  free_bytes = uint64_t(vf.f_bavail) * vf.f_frsize;
  total_bytes = uint64_t(vf.f_blocks) * vf.f_frsize;
  return true;
}

std::string size_text(uint64_t b) {
  char buf[32];
  if (b < 1024) std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
  else if (b < 1024ull * 1024) std::snprintf(buf, sizeof buf, "%llu KB", (unsigned long long)((b + 512) / 1024));
  else if (b < 1024ull * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f MB", double(b) / (1024.0 * 1024.0));
  else std::snprintf(buf, sizeof buf, "%.1f GB", double(b) / (1024.0 * 1024.0 * 1024.0));
  return buf;
}

}  // namespace zf::fileops
