// File manager engine (fileops.cpp) on a temp folder: listing and sort, copy / move / delete of files and trees, clash suffixes, cancel leaves
// no half file, links are never followed, protected paths and paths outside the roots are refused, rename and new folder rules.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "fileops.hpp"

using namespace zf::fileops;

namespace {

std::string g_root;

void put(const std::string& rel, const std::string& body) {
  std::ofstream(g_root + "/" + rel, std::ios::binary) << body;
}
void dir(const std::string& rel) { ::mkdir((g_root + "/" + rel).c_str(), 0755); }
bool has(const std::string& rel) {
  struct stat st;
  return ::lstat((g_root + "/" + rel).c_str(), &st) == 0;
}
std::string get(const std::string& rel) {
  std::ifstream f(g_root + "/" + rel, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
std::string P(const std::string& rel) { return g_root + "/" + rel; }

}  // namespace

int main() {
  char tpl[] = "/tmp/zf-fileops-XXXXXX";
  if (!::mkdtemp(tpl)) return 1;
  g_root = tpl;
  std::string outside_path = std::string(tpl) + "-outside";
  ::mkdir(outside_path.c_str(), 0755);
  { std::ofstream(outside_path + "/secret.txt") << "keep me"; }

  dir("roms");
  dir("keep");
  Policy pol;
  pol.roots = {P("roms"), P("keep")};
  pol.keep = {P("roms/system")};
  dir("roms/system");

  // ---- names, paths
  CHECK_EQ(normalize("/a//b/./c/../d/"), std::string("/a/b/d"));
  CHECK(within("/a/b", "/a"));
  CHECK(!within("/ab", "/a"));
  CHECK(valid_name("game.sfc") && !valid_name("") && !valid_name("..") && !valid_name("a/b") && !valid_name("."));

  // ---- listing and sort: folders first, hidden only when asked, no case
  dir("roms/zdir");
  put("roms/b.txt", std::string(10, 'x'));
  put("roms/A.txt", std::string(300, 'x'));
  put("roms/.hid", "h");
  std::vector<Entry> es;
  CHECK(list(P("roms"), Sort::Name, false, es));
  CHECK_EQ(es.size(), size_t(4));  // system, zdir, A.txt, b.txt
  CHECK(es[0].dir && es[1].dir && es[0].name == "system" && es[1].name == "zdir");
  CHECK_EQ(es[2].name, std::string("A.txt"));
  CHECK(list(P("roms"), Sort::Name, true, es));
  CHECK_EQ(es.size(), size_t(5));
  CHECK(list(P("roms"), Sort::Size, false, es));
  CHECK_EQ(es[2].name, std::string("A.txt"));  // biggest first among the files

  // ---- copy a file; a second copy gets " (2)"
  Progress pr;
  measure({P("roms/A.txt")}, &pr);
  CHECK_EQ(pr.files_total.load(), 1);
  CHECK_EQ(pr.bytes_total.load(), uint64_t(300));
  Result r = transfer(pol, {P("roms/A.txt")}, P("roms/zdir"), false, pr);
  CHECK(r.err == Err::None && r.done == 1);
  CHECK_EQ(get("roms/zdir/A.txt"), std::string(300, 'x'));
  r = transfer(pol, {P("roms/A.txt")}, P("roms/zdir"), false, pr);
  CHECK(has("roms/zdir/A (2).txt"));
  CHECK(!has("roms/zdir/A.txt.part"));
  CHECK_EQ(unique_name(P("roms/zdir"), "A.txt"), std::string("A (3).txt"));

  // ---- a tree, with a link inside that is copied as a link and not followed
  dir("roms/tree");
  dir("roms/tree/sub");
  put("roms/tree/sub/f.bin", "12345");
  CHECK(::symlink(outside_path.c_str(), P("roms/tree/out").c_str()) == 0);
  Progress p2;
  r = transfer(pol, {P("roms/tree")}, P("keep"), false, p2);
  CHECK(r.err == Err::None);
  CHECK_EQ(get("keep/tree/sub/f.bin"), std::string("12345"));
  struct stat st;
  CHECK(::lstat(P("keep/tree/out").c_str(), &st) == 0 && S_ISLNK(st.st_mode));

  // ---- delete the copy: the link goes, what it pointed to stays
  Progress p3;
  r = remove(pol, {P("keep/tree")}, p3);
  CHECK(r.err == Err::None && r.done == 1);
  CHECK(!has("keep/tree"));
  CHECK(::access((outside_path + "/secret.txt").c_str(), F_OK) == 0);

  // ---- a folder cannot go into itself
  Progress p4;
  r = transfer(pol, {P("roms/tree")}, P("roms/tree/sub"), false, p4);
  CHECK(r.err == Err::Inside);
  CHECK(!has("roms/tree/sub/tree"));

  // ---- move: same file system is a rename; same folder is a no-op
  Progress p5;
  r = transfer(pol, {P("roms/b.txt")}, P("keep"), true, p5);
  CHECK(r.err == Err::None && !has("roms/b.txt") && has("keep/b.txt"));
  r = transfer(pol, {P("keep/b.txt")}, P("keep"), true, p5);
  CHECK(r.err == Err::None && has("keep/b.txt") && !has("keep/b (2).txt"));

  // ---- cancel in the middle of a big copy leaves no half file
  {
    std::string big(3 * 1024 * 1024, 'z');
    put("roms/big.bin", big);
    Progress pc;
    pc.cancel = true;  // already asked: the first chunk sees it
    r = transfer(pol, {P("roms/big.bin")}, P("keep"), false, pc);
    CHECK(r.err == Err::Cancelled);
    CHECK(!has("keep/big.bin") && !has("keep/big.bin.part"));
  }

  // ---- protected and outside
  Progress p6;
  r = remove(pol, {P("roms/system")}, p6);
  CHECK(r.err == Err::Protected && has("roms/system"));
  r = remove(pol, {P("roms")}, p6);
  CHECK(r.err == Err::Protected && has("roms"));
  r = remove(pol, {outside_path + "/secret.txt"}, p6);
  CHECK(r.err == Err::Outside && ::access((outside_path + "/secret.txt").c_str(), F_OK) == 0);
  r = remove(pol, {P("roms/../../etc/passwd")}, p6);
  CHECK(r.err == Err::Outside);
  r = transfer(pol, {P("roms/A.txt")}, outside_path, false, p6);
  CHECK(r.err == Err::Outside && ::access((outside_path + "/A.txt").c_str(), F_OK) != 0);
  CHECK(!inside_real(pol, P("roms/tree/out")));  // a folder link that points out cannot be entered
  CHECK(inside_real(pol, P("roms/zdir")));
  r = rename_item(pol, P("roms/system"), "other");
  CHECK(r.err == Err::Protected);

  // ---- rename and new folder
  r = rename_item(pol, P("roms/A.txt"), "B.txt");
  CHECK(r.err == Err::None && has("roms/B.txt") && !has("roms/A.txt"));
  put("roms/C.txt", "c");
  r = rename_item(pol, P("roms/B.txt"), "C.txt");
  CHECK(r.err == Err::Exists && has("roms/B.txt"));
  r = rename_item(pol, P("roms/B.txt"), "x/y");
  CHECK(r.err == Err::BadName);
  r = make_dir(pol, P("roms"), "New folder");
  CHECK(r.err == Err::None && has("roms/New folder"));
  r = make_dir(pol, P("roms"), "New folder");
  CHECK(r.err == Err::Exists);
  r = make_dir(pol, outside_path, "x");
  CHECK(r.err == Err::Outside);

  // ---- rename that only changes the case on a file system that ignores it: origin and target are the same file (same device and inode), so it is a rename
  put("roms/Case.txt", "c");
  CHECK(::link(P("roms/Case.txt").c_str(), P("roms/case.txt").c_str()) == 0);  // stands for "the same file under another spelling"
  r = rename_item(pol, P("roms/Case.txt"), "case.txt");
  CHECK(r.err == Err::None && r.done == 1);

  // ---- a copy that froze earlier left "<name>.part": the next copy of that name goes over it
  dir("roms/stale");
  put("roms/stale/new.bin.part", "half");
  put("roms/new.bin", "whole file");
  Progress ps;
  r = transfer(pol, {P("roms/new.bin")}, P("roms/stale"), false, ps);  // just written: it may be a live upload, so it is left alone
  CHECK(r.err == Err::Exists && get("roms/stale/new.bin.part") == "half");
  {
    const struct timespec old2[2] = {{::time(nullptr) - 3600, 0}, {::time(nullptr) - 3600, 0}};
    CHECK(::utimensat(AT_FDCWD, P("roms/stale/new.bin.part").c_str(), old2, 0) == 0);
  }
  Progress ps2;
  r = transfer(pol, {P("roms/new.bin")}, P("roms/stale"), false, ps2);  // an hour old: what a frozen console left
  CHECK(r.err == Err::None);
  CHECK_EQ(get("roms/stale/new.bin"), std::string("whole file"));
  CHECK(!has("roms/stale/new.bin.part"));

  // ---- natural order: numbers by value, accents with their letter
  dir("roms/nat");
  for (const char* n : {"Game 10.sfc", "Game 2.sfc", "Game 1.sfc", "\xC3\x81vila.txt", "Zeta.txt", "Avila2.txt", "\xC3\xB1" "and\xC3\xBA.txt", "Name.txt"}) put(std::string("roms/nat/") + n, "x");
  CHECK(list(P("roms/nat"), Sort::Name, false, es));
  CHECK_EQ(es.size(), size_t(8));
  CHECK_EQ(es[0].name, std::string("\xC3\x81vila.txt"));
  CHECK_EQ(es[1].name, std::string("Avila2.txt"));
  CHECK_EQ(es[2].name, std::string("Game 1.sfc"));
  CHECK_EQ(es[3].name, std::string("Game 2.sfc"));
  CHECK_EQ(es[4].name, std::string("Game 10.sfc"));
  CHECK_EQ(es[5].name, std::string("Name.txt"));
  CHECK_EQ(es[6].name, std::string("\xC3\xB1" "and\xC3\xBA.txt"));
  CHECK_EQ(es[7].name, std::string("Zeta.txt"));

  // ---- natural_compare: numbers by value, accents and case folded, never equal for different names
  CHECK(natural_compare("Game 2", "Game 10") < 0 && natural_compare("Game 10", "Game 2") > 0);
  CHECK(natural_compare("a007", "a7") != 0 && natural_compare("a07", "a8") < 0);
  CHECK(natural_compare("\xC3\x81vila", "Avila") != 0 && natural_compare("\xC3\x81vila", "Azul") < 0 && natural_compare("\xC3\xA9" "clair", "Eagle") > 0);
  CHECK(natural_compare("\xC3\xA7" "a", "d") < 0 && natural_compare("\xC3\x91" "z", "na") > 0 && natural_compare("\xC3\x9C" "ber", "Uz") < 0);
  CHECK(natural_compare("abc", "ABD") < 0 && natural_compare("same", "same") == 0 && natural_compare("a", "ab") < 0);

  // ---- errors in words of the screen: what an errno means
  CHECK(err_from_errno(EFBIG, false) == Err::TooBig && err_from_errno(ENOSPC, true) == Err::NoSpace);
  CHECK(err_from_errno(EROFS, false) == Err::ReadOnly && err_from_errno(EACCES, true) == Err::ReadOnly);
  CHECK(err_from_errno(EPERM, true) == Err::FsName && err_from_errno(EINVAL, true) == Err::FsName);
  CHECK(err_from_errno(EPERM, false) == Err::Io);

  // ---- the failing item is named: a file inside a folder, and how many were done before
  {
    dir("roms/fail");
    dir("roms/fail/in");
    put("roms/fail/in/a.bin", "1");
    put("roms/fail/in/b.bin", "2");
    put("roms/ok1.bin", "1");
    put("roms/ok2.bin", "2");
    Progress pg;
    r = remove(pol, {P("roms/ok1.bin"), P("roms/ok2.bin"), P("roms/system")}, pg);  // the third is protected: two done of three, and it is named
    CHECK(r.err == Err::Protected && r.done == 2 && r.total == 3 && r.first == P("roms/system"));
    CHECK(!has("roms/ok1.bin") && !has("roms/ok2.bin") && has("roms/system"));
    // a copy that fails inside a folder names the file inside
    ::chmod(P("roms/fail/in/b.bin").c_str(), 0000);
    if (::geteuid() != 0) {
      Progress pg3;
      r = transfer(pol, {P("roms/fail")}, P("keep"), false, pg3);
      CHECK(r.err == Err::Io && r.first == P("roms/fail/in/b.bin") && r.total == 1 && r.done == 0);
      CHECK(!has("keep/fail"));
    }
    ::chmod(P("roms/fail/in/b.bin").c_str(), 0644);
  }

  // ---- a file system with no links: the link is skipped and counted, the rest is copied
  {
    dir("roms/lk");
    dir("roms/lk/d");
    put("roms/lk/d/f.bin", "data");
    CHECK(::symlink("f.bin", P("roms/lk/d/l1").c_str()) == 0);
    CHECK(::symlink("d", P("roms/lk/l2").c_str()) == 0);
    hooks().symlink = [](const char*, const char*) -> int {
      errno = EPERM;
      return -1;
    };
    Progress pl;
    r = transfer(pol, {P("roms/lk")}, P("keep"), false, pl);
    CHECK(r.err == Err::None && r.skipped == 2);
    CHECK_EQ(get("keep/lk/d/f.bin"), std::string("data"));
    CHECK(!has("keep/lk/d/l1") && !has("keep/lk/l2"));
    // a move that had to skip links keeps the originals (deleting them would lose the links)
    Policy pm2;
    pm2.roots = {P("roms"), P("keep")};
    char t3[] = "/dev/shm/zf-fileops-lnk-XXXXXX";
    CHECK(::mkdtemp(t3) != nullptr);
    pm2.roots.push_back(t3);
    Progress pl2;
    r = transfer(pm2, {P("roms/lk")}, t3, true, pl2);
    CHECK(r.err == Err::None && r.kept && r.skipped == 2);
    CHECK(has("roms/lk/d/l1"));  // the source is whole
    hooks().symlink = nullptr;
    const std::string c3 = std::string("rm -rf '") + t3 + "'";
    if (std::system(c3.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
  }

  // ---- FAT32: a file of 4 GB or more is refused at once; room is checked for a move across file systems too
  {
    dir("roms/fat");
    put("roms/fat/small.bin", "12345");
    {  // a sparse file of 4 GB: it takes no room on a file system that has holes
      const int fd = ::open(P("roms/fat/huge.bin").c_str(), O_WRONLY | O_CREAT, 0644);
      CHECK(fd >= 0);
      if (fd >= 0) {
        CHECK(::ftruncate(fd, off_t(4ull * 1024 * 1024 * 1024)) == 0);
        ::close(fd);
      }
    }
    hooks().is_fat32 = [](const std::string&) { return true; };
    Progress pt;
    r = transfer(pol, {P("roms/fat")}, P("keep"), false, pt);
    CHECK(r.err == Err::TooBig && r.first == P("roms/fat/huge.bin"));
    CHECK(!has("keep/fat") && !has("keep/fat/small.bin.part"));
    hooks().is_fat32 = nullptr;
    ::unlink(P("roms/fat/huge.bin").c_str());

    // a move to another file system checks the room first; inside one file system it is a rename and asks for nothing
    hooks().space = [](const std::string&, uint64_t& f, uint64_t& t) {
      f = 3, t = 1000;
      return true;
    };
    char t4[] = "/dev/shm/zf-fileops-room-XXXXXX";
    CHECK(::mkdtemp(t4) != nullptr);
    Policy pr2;
    pr2.roots = {P("roms"), P("keep"), t4};
    Progress pn;
    r = transfer(pr2, {P("roms/fat/small.bin")}, t4, true, pn);
    CHECK(r.err == Err::NoSpace);
    CHECK(has("roms/fat/small.bin") && ::access((std::string(t4) + "/small.bin").c_str(), F_OK) != 0);
    Progress pn2;
    r = transfer(pr2, {P("roms/fat/small.bin")}, P("keep"), true, pn2);
    CHECK(r.err == Err::None && has("keep/small.bin"));
    hooks().space = nullptr;
    const std::string c4 = std::string("rm -rf '") + t4 + "'";
    if (std::system(c4.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
  }

  // ---- protection: the system folders under the roms folder stay, their content does not; a warning for the system files
  {
    dir("roms/gb");
    put("roms/gb/game.gb", "g");
    put("roms/loose.txt", "l");
    dir("roms/conf");
    put("roms/conf/set.cfg", "c");
    Policy pk;
    pk.roots = {P("roms")};
    pk.keep_sub = {P("roms")};
    pk.warn = {P("roms/conf")};
    CHECK(!can_change(pk, P("roms/gb")));
    CHECK(!can_change(pk, P("roms/conf")));
    CHECK(can_change(pk, P("roms/gb/game.gb")));
    CHECK(can_change(pk, P("roms/loose.txt")));
    Progress pk1;
    r = remove(pk, {P("roms/gb")}, pk1);
    CHECK(r.err == Err::Protected && has("roms/gb/game.gb"));
    r = rename_item(pk, P("roms/gb"), "gb2");
    CHECK(r.err == Err::Protected && has("roms/gb"));
    r = rename_item(pk, P("roms/gb/game.gb"), "game2.gb");
    CHECK(r.err == Err::None && has("roms/gb/game2.gb"));
    CHECK(needs_warning(pk, P("roms/conf/set.cfg")) && needs_warning(pk, P("roms/conf/a/b")));
    CHECK(!needs_warning(pk, P("roms/conf")) && !needs_warning(pk, P("roms/gb/game2.gb")) && !needs_warning(pk, P("roms/confx/y")));
  }

  // ---- the name of the file being copied is available while it runs
  {
    Progress pn;
    pn.set_current("a.bin");
    CHECK_EQ(pn.current(), std::string("a.bin"));
  }

  // ---- space and text
  uint64_t fr = 0, tot = 0;
  CHECK(space(g_root, fr, tot) && tot > 0 && fr <= tot);
  CHECK_EQ(size_text(512), std::string("512 B"));
  CHECK_EQ(size_text(212 * 1024), std::string("212 KB"));
  CHECK_EQ(size_text(4ull * 1024 * 1024), std::string("4.0 MB"));
  CHECK_EQ(size_text(13ull * 1024 * 1024 * 1024), std::string("13.0 GB"));

  // ---- another file system (tmpfs in /dev/shm): a move across devices is copy + delete, a cancel leaves no half tree, a mount point is protected
  {
    char t2[] = "/dev/shm/zf-fileops-XXXXXX";
    CHECK(::mkdtemp(t2) != nullptr);
    struct stat a, b;
    CHECK(::stat(g_root.c_str(), &a) == 0 && ::stat(t2, &b) == 0);
    CHECK(a.st_dev != b.st_dev);  // the test only means something across two file systems: it fails, it does not skip, where they are the same
    const std::string shm = t2;
    Policy p2;
    p2.roots = {P("roms"), shm};
    dir("roms/mv");
    dir("roms/mv/sub");
    put("roms/mv/sub/f.bin", "payload");
    Progress pm;
    measure({P("roms/mv")}, &pm);
    r = transfer(p2, {P("roms/mv")}, shm, true, pm);
    CHECK(r.err == Err::None);
    CHECK(!has("roms/mv"));
    struct stat st2;
    CHECK(::stat((shm + "/mv/sub/f.bin").c_str(), &st2) == 0 && st2.st_size == 7);
    // cancelled at the first chunk: the source is whole, the other side has nothing (no folder, no .part)
    dir("roms/mv2");
    put("roms/mv2/big.bin", std::string(2 * 1024 * 1024, 'q'));
    Progress pk;
    pk.cancel = true;
    r = transfer(p2, {P("roms/mv2")}, shm, true, pk);
    CHECK(r.err == Err::Cancelled);
    CHECK(has("roms/mv2/big.bin"));
    CHECK(::stat((shm + "/mv2").c_str(), &st2) != 0);
    // the mount point of the tmpfs sits in /dev, which is another file system: it cannot be deleted or renamed
    Policy p3;
    p3.roots = {"/dev"};
    CHECK(!can_change(p3, "/dev/shm"));
    Progress pz;
    r = remove(p3, {"/dev/shm"}, pz);
    CHECK(r.err == Err::Protected);
    CHECK(::stat(shm.c_str(), &st2) == 0);
    const std::string cmd2 = "rm -rf '" + shm + "'";
    if (std::system(cmd2.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
  }

  const std::string cmd1 = "rm -rf '" + g_root + "' '" + outside_path + "'";
  if (std::system(cmd1.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
  return zt::done();
}
