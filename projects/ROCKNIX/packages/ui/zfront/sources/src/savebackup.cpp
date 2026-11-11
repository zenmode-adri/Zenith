#include "savebackup.hpp"

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "util.hpp"

namespace zf::savebackup {
namespace {

constexpr int kMaxDepth = 10;
constexpr int kLong = 600000;  // ms

void add(Plan& p, const std::string& rel, const struct stat& st) {
  p.files.push_back(rel);
  p.bytes += uint64_t(st.st_size);
}

// Every regular file under `rel` (symbolic links are not followed).
void walk_all(Plan& p, const std::string& storage, const std::string& rel, int depth) {
  DIR* d = ::opendir((storage + "/" + rel).c_str());
  if (!d) return;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    std::string r = rel + "/" + n;
    struct stat st {};
    if (::lstat((storage + "/" + r).c_str(), &st) != 0) continue;
    if (S_ISREG(st.st_mode)) add(p, r, st);
    else if (S_ISDIR(st.st_mode) && depth < kMaxDepth) walk_all(p, storage, r, depth + 1);
  }
  ::closedir(d);
}

bool save_name(const std::string& n) {
  return fnmatch("*.srm", n.c_str(), FNM_CASEFOLD) == 0 || fnmatch("*.sav", n.c_str(), FNM_CASEFOLD) == 0 ||
         fnmatch("*.state*", n.c_str(), FNM_CASEFOLD) == 0;
}

// roms/** : only the save files; the folders of the other save locations and of the backups are skipped.
void walk_roms(Plan& p, const std::string& storage, const std::string& rel, int depth) {
  DIR* d = ::opendir((storage + "/" + rel).c_str());
  if (!d) return;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    std::string r = rel + "/" + n;
    if (r == "roms/backup" || r == "roms/savestates" || r == "roms/psp/PSP") continue;
    if (n == "images" || n == "videos" || n == "manuals" || n == "media") continue;  // only to be quick: nothing there matches anyway
    struct stat st {};
    if (::lstat((storage + "/" + r).c_str(), &st) != 0) continue;
    if (S_ISREG(st.st_mode)) {
      if (save_name(n)) add(p, r, st);
    } else if (S_ISDIR(st.st_mode) && depth < kMaxDepth) {
      walk_roms(p, storage, r, depth + 1);
    }
  }
  ::closedir(d);
}

bool has_component(std::string_view rel, std::string_view comp) {
  size_t a = 0;
  while (a <= rel.size()) {
    size_t e = rel.find('/', a);
    if (e == std::string_view::npos) e = rel.size();
    if (rel.substr(a, e - a) == comp) return true;
    a = e + 1;
  }
  return false;
}

bool under(std::string_view rel, std::string_view root) { return rel == root || (starts_with(rel, root) && rel[root.size()] == '/'); }

bool digits(std::string_view s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }); }

void mkdirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i)
    if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
}

}  // namespace

Plan plan(const std::string& storage) {
  Plan p;
  walk_all(p, storage, "roms/savestates", 0);
  walk_all(p, storage, "roms/psp/PSP/SAVEDATA", 0);
  walk_all(p, storage, ".config/ppsspp/PSP/SYSTEM", 0);
  walk_roms(p, storage, "roms", 0);
  std::sort(p.files.begin(), p.files.end());
  p.files.erase(std::unique(p.files.begin(), p.files.end()), p.files.end());
  return p;
}

bool in_plan(std::string_view rel, bool is_dir) {
  if (rel.empty() || rel[0] == '/') return false;
  for (std::string_view root : {"roms/savestates", "roms/psp/PSP/SAVEDATA", ".config/ppsspp/PSP/SYSTEM"})
    if (under(rel, root)) return true;
  if (is_dir)  // the folders leading to those locations
    for (std::string_view root : {"roms/savestates", "roms/psp/PSP/SAVEDATA", ".config/ppsspp/PSP/SYSTEM", "roms/psp/PSP", "roms/psp", ".config/ppsspp/PSP",
                                  ".config/ppsspp", ".config"})
      if (rel == root) return true;
  if (!under(rel, "roms") || rel == "roms") return rel == "roms" && is_dir;
  // roms/**: save files only; the other save locations, the backups and the media folders are never walked
  if (under(rel, "roms/backup") || under(rel, "roms/psp/PSP")) return false;
  for (std::string_view skip : {"images", "videos", "manuals", "media"})
    if (has_component(rel, skip)) return false;
  if (is_dir) return true;
  return save_name(std::string(rel.substr(rel.rfind('/') + 1)));
}

std::atomic<bool>& busy_flag() {
  static std::atomic<bool> b{false};
  return b;
}

std::atomic<int>& generation() {
  static std::atomic<int> g{0};
  return g;
}

bool valid_name(std::string_view n) {
  constexpr std::string_view pre = "saves-", ext = ".tar.gz";
  if (!starts_with(n, pre) || !ends_with(n, ext) || n.size() < pre.size() + ext.size()) return false;
  std::string_view m = n.substr(pre.size(), n.size() - pre.size() - ext.size());  // YYYYMMDD-HHMM[-preN]
  if (m.size() < 13 || m[8] != '-' || !digits(m.substr(0, 8)) || !digits(m.substr(9, 4))) return false;
  m.remove_prefix(13);
  if (m.empty()) return true;
  if (!starts_with(m, "-pre")) return false;
  m.remove_prefix(4);
  return m.empty() || (digits(m) && m[0] != '0');
}

bool is_pre(std::string_view n) { return valid_name(n) && n.find("-pre") != std::string_view::npos; }

std::string stamp_now() {
  std::time_t t = std::time(nullptr);
  std::tm tmv {};
  localtime_r(&t, &tmv);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y%m%d-%H%M", &tmv);
  return buf;
}

std::string pretty_date(std::string_view n) {
  if (!valid_name(n)) return {};
  std::string_view s = n.substr(6);
  return std::string(s.substr(0, 4)) + "-" + std::string(s.substr(4, 2)) + "-" + std::string(s.substr(6, 2)) + " " + std::string(s.substr(9, 2)) + ":" +
         std::string(s.substr(11, 2));
}

std::vector<Backup> list(const std::string& dir) {
  std::vector<Backup> v;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d)) {
      std::string n = e->d_name;
      if (!valid_name(n)) continue;
      struct stat st {};
      if (::stat((dir + "/" + n).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
      v.push_back({n, uint64_t(st.st_size)});
    }
    ::closedir(d);
  }
  std::sort(v.begin(), v.end(), [](const Backup& a, const Backup& b) { return a.name > b.name; });
  return v;
}

std::string size_text(uint64_t b) {
  char buf[32];
  if (b >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", double(b) / double(1 << 20));
  else if (b >= 1024) std::snprintf(buf, sizeof buf, "%llu KB", (unsigned long long)((b + 512) / 1024));
  else std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
  return buf;
}

uint64_t free_bytes(const std::string& dir) {
  std::string d = dir;
  struct stat st {};
  while (d.size() > 1 && ::stat(d.c_str(), &st) != 0) d = path_dir(d);
  struct statvfs vf {};
  if (::statvfs(d.c_str(), &vf) != 0) return 0;
  return uint64_t(vf.f_bavail) * uint64_t(vf.f_frsize);
}

std::string create_cmd(const std::string& storage, const std::string& dir, const std::string& name, const std::string& listfile) {
  const std::string tar = shell_quote(dir + "/" + name.substr(0, name.size() - 3) + ".tmp");  // saves-X.tar.tmp
  const std::string part = shell_quote(dir + "/" + name + ".part");
  const std::string fin = shell_quote(dir + "/" + name);
  return "cd " + shell_quote(storage) + " && tar -cf " + tar + " -T " + shell_quote(listfile) + " && gzip -c " + tar + " > " + part + " && mv " + part + " " +
         fin + "; rc=$?; rm -f " + tar + " " + part + "; exit $rc";
}

std::string verify_cmd(const std::string& archive) {
  const std::string a = shell_quote(archive);
  return "gzip -t " + a + " && gzip -dc " + a + " | tar -tf - >/dev/null";
}

std::string extract_cmd(const std::string& storage, const std::string& archive) {
  return "gzip -dc " + shell_quote(archive) + " | tar -xf - -C " + shell_quote(storage);
}

Result create(Services& sv, const std::string& storage, const std::string& tmp, const std::string& stamp, bool pre) {
  Result r;
  const std::string dir = backup_dir(storage);
  std::string name = "saves-" + stamp + ".tar.gz";
  if (pre) {
    name = "saves-" + stamp + "-pre.tar.gz";
    for (int i = 2; i < 100 && file_exists(dir + "/" + name); ++i) name = "saves-" + stamp + "-pre" + std::to_string(i) + ".tar.gz";
  }
  if (!valid_name(name)) {
    r.err = Err::BadName;
    return r;
  }
  if (file_exists(dir + "/" + name)) {
    r.err = Err::Exists;
    return r;
  }
  Plan p = plan(storage);
  r.files = p.files.size();
  if (p.files.empty()) {
    r.err = Err::NoSaves;
    return r;
  }
  mkdirs(dir);
  if (free_bytes(dir) < 2 * p.bytes + 65536) {  // the tar is written first, then compressed next to it
    r.err = Err::NoSpace;
    return r;
  }
  const std::string listfile = tmp + "/zf-savebackup-" + std::to_string(::getpid()) + ".list";
  {
    std::ofstream f(listfile, std::ios::binary | std::ios::trunc);
    for (const std::string& n : p.files) f << n << '\n';
    f.flush();
    if (!f) {
      r.err = Err::Failed;
      return r;
    }
  }
  CmdResult c = sv.cmd.run(create_cmd(storage, dir, name, listfile), kLong);
  ::unlink(listfile.c_str());
  if (c.exit_code != 0) {
    r.err = Err::Failed;
    return r;
  }
  r.name = name;
  struct stat st {};
  if (::stat((dir + "/" + name).c_str(), &st) == 0) r.bytes = uint64_t(st.st_size);
  else if (!sv.cmd.dry_run) r.err = Err::Failed;
  return r;
}

Result restore(Services& sv, const std::string& storage, const std::string& tmp, const std::string& stamp, const std::string& name) {
  Result r;
  const std::string dir = backup_dir(storage);
  if (!valid_name(name)) {
    r.err = Err::BadName;
    return r;
  }
  const std::string archive = dir + "/" + name;
  if (!sv.cmd.dry_run && !file_exists(archive)) {
    r.err = Err::Damaged;
    return r;
  }
  if (sv.cmd.run(verify_cmd(archive), kLong).exit_code != 0) {
    r.err = Err::Damaged;
    return r;
  }
  Result b = create(sv, storage, tmp, stamp, true);
  if (b.err == Err::None) {
    r.pre = b.name;
  } else if (b.err != Err::NoSaves) {  // nothing to save is fine; anything else stops before a single file is replaced
    r.err = b.err == Err::NoSpace ? Err::NoSpace : Err::PreFailed;
    return r;
  }
  if (sv.cmd.run(extract_cmd(storage, archive), kLong).exit_code != 0) {
    r.err = Err::Failed;
    return r;
  }
  r.name = name;
  return r;
}

bool remove_backup(const std::string& dir, const std::string& name) {
  if (!valid_name(name)) return false;
  return ::unlink((dir + "/" + name).c_str()) == 0;
}

}  // namespace zf::savebackup
