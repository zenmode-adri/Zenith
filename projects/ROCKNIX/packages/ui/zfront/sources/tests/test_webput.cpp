// webput: folders and resumable uploads (POST /files/put, GET /files/put/status). The real httpd server on 127.0.0.1 (port 0) with
// the real stream worker, ROM folders in a temp dir, free space injected. No network beyond localhost.
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "httpd.hpp"
#include "webcore.hpp"
#include "webtransfer.hpp"

using namespace zf;
using std::string;

namespace {

const char* kPin = "4821";

int connect_to(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool send_str(int fd, const string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += size_t(n);
  }
  return true;
}

string read_all(int fd, int ms = 4000) {
  string out;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  for (;;) {
    int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count());
    if (left <= 0) break;
    pollfd pf{fd, POLLIN, 0};
    if (::poll(&pf, 1, left) <= 0) break;
    char b[2048];
    ssize_t n = ::recv(fd, b, sizeof b, 0);
    if (n <= 0) break;
    out.append(b, size_t(n));
  }
  return out;
}

int status_of(const string& r) {
  if (r.compare(0, 9, "HTTP/1.1 ") != 0 || r.size() < 12) return 0;
  return std::atoi(r.c_str() + 9);
}

string talk(uint16_t port, const string& raw) {
  int fd = connect_to(port);
  if (fd < 0) return "";
  send_str(fd, raw);
  string r = read_all(fd);
  ::close(fd);
  return r;
}

string pattern(size_t n) {
  string s(n, '\0');
  uint32_t x = 12345;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    s[i] = char(x >> 24);
  }
  return s;
}

string slurp(const string& p) {
  string s;
  if (FILE* f = std::fopen(p.c_str(), "rb")) {
    char b[8192];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
  }
  return s;
}

void write_file(const string& p, const string& data) {
  if (FILE* f = std::fopen(p.c_str(), "wb")) {
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
  }
}

bool exists(const string& p) {
  struct stat sb;
  return ::lstat(p.c_str(), &sb) == 0;
}

long long size_of(const string& p) {
  struct stat sb;
  return ::lstat(p.c_str(), &sb) == 0 ? (long long)sb.st_size : -1;
}

std::vector<string> list(const string& dir) {
  std::vector<string> v;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d))
      if (string(e->d_name) != "." && string(e->d_name) != "..") v.push_back(e->d_name);
    ::closedir(d);
  }
  std::sort(v.begin(), v.end());
  return v;
}

struct Rig {
  string root, gb;
  std::atomic<int> reloads{0};
  std::atomic<uint64_t> fake_free{~uint64_t(0)};
  http::Server srv;
  webtransfer::Env env;

  Rig() {
    char t[] = "/tmp/zf-webput-XXXXXX";
    root = ::mkdtemp(t);
    gb = root + "/gb";
    ::mkdir(gb.c_str(), 0755);
    webtransfer::reset_for_test(kPin);
    webtransfer::SystemInfo si;
    si.name = "gb";
    si.fullname = "Game Boy";
    si.rom_dir = gb;
    si.exts = {".gb", ".GBC", ".zip"};
    webtransfer::publish_info({si});
    env.reload = [this] { ++reloads; };
    env.free_space = [this](const string&, uint64_t& out) {
      out = fake_free.load();
      return true;
    };
    srv.set_stream_handler([this](const http::Request& r, http::BodyStream& b) { return webtransfer::stream_route(r, b, env); }, webtransfer::stream_pred);
    string err;
    bool ok = srv.start(
        0,
        [this](const http::Request& r) {
          if (r.path.compare(0, 6, "/files") == 0) return webtransfer::route(r, env);
          http::Response x;
          x.status = 404;
          return x;
        },
        err);
    CHECK(ok);
  }
  ~Rig() {
    srv.stop();
    string cmd = "rm -rf '" + root + "'";
    if (std::system(cmd.c_str()) != 0) {
    }
  }
  uint16_t port() const { return srv.port(); }
};

string put_head(const string& query, size_t len, const char* pin = kPin) {
  string h = "POST /files/put?" + query + " HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(len) + "\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  return h + "\r\n";
}

string put_raw(uint16_t port, const string& query, const string& body, const char* pin = kPin) { return talk(port, put_head(query, body.size(), pin) + body); }
int put(uint16_t port, const string& query, const string& body, const char* pin = kPin) { return status_of(put_raw(port, query, body, pin)); }

string get(uint16_t port, const string& path, const char* pin = nullptr) {
  string h = "GET " + path + " HTTP/1.1\r\nHost: x\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  return talk(port, h + "\r\n");
}

// A whole file in one go: path (already percent-encoded where needed), total = body size.
int put_whole(uint16_t port, const string& path, const string& body, const string& extra = "") {
  return put(port, "system=gb&path=" + path + "&offset=0&total=" + std::to_string(body.size()) + extra, body);
}

string slice_q(const string& path, size_t off, size_t total, const string& extra = "") {
  return "system=gb&path=" + path + "&offset=" + std::to_string(off) + "&total=" + std::to_string(total) + extra;
}

// Waits until the .part (cut upload) has settled at `want` bytes; the worker thread finishes a moment after the socket closes.
bool wait_size(const string& p, long long want) {
  for (int i = 0; i < 200; ++i) {
    if (size_of(p) == want) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

void test_nested_and_status() {
  Rig r;
  webtransfer::set_enabled(true);
  const string a = pattern(3000), b = pattern(500);
  CHECK_EQ(put_whole(r.port(), "Game%2Fdisc1%2Ftrack01.bin", a), 200);
  CHECK(slurp(r.gb + "/Game/disc1/track01.bin") == a);
  CHECK(!exists(r.gb + "/Game/disc1/.track01.bin.zfpart"));
  CHECK_EQ(r.reloads.load(), 0);  // not the last of a batch
  string st = get(r.port(), "/files/put/status?system=gb&path=Game%2Fdisc1%2Ftrack01.bin", kPin);
  CHECK_EQ(status_of(st), 200);
  CHECK(st.find("{\"have\":0,\"done\":true,\"size\":3000}") != string::npos);
  st = get(r.port(), "/files/put/status?system=gb&path=Game%2Fdisc1%2Fnone.bin", kPin);
  CHECK(st.find("{\"have\":0,\"done\":false,\"size\":0}") != string::npos);
  st = get(r.port(), "/files/put/status?system=gb&path=Nowhere%2Fdeep%2Ffile.bin", kPin);  // folders that do not exist: nothing is created
  CHECK_EQ(status_of(st), 200);
  CHECK(st.find("{\"have\":0,\"done\":false,\"size\":0}") != string::npos);
  CHECK(!exists(r.gb + "/Nowhere"));
  string last = put_raw(r.port(), "system=gb&path=Game%2Fgame.cue&offset=0&total=500&final=1", b);
  CHECK_EQ(status_of(last), 200);
  CHECK(last.find("\"done\":true") != string::npos);
  CHECK_EQ(r.reloads.load(), 1);  // once for the batch
  CHECK(slurp(r.gb + "/Game/game.cue") == b);
  CHECK(list(r.gb) == std::vector<string>{"Game"});
  CHECK(list(r.gb + "/Game") == (std::vector<string>{"disc1", "game.cue"}));
  // four levels are fine; a single top-level file is its own batch
  CHECK_EQ(put_whole(r.port(), "a%2Fb%2Fc%2Fd.bin", "x"), 200);
  CHECK_EQ(put_whole(r.port(), "top.gb", "x", "&final=1"), 200);
  CHECK_EQ(r.reloads.load(), 2);
  // a slice that does not finish the file never asks for the rescan; the status shows the .part
  CHECK_EQ(put(r.port(), slice_q("S%2Fpart.bin", 0, 10, "&final=1"), "abc"), 200);
  CHECK_EQ(r.reloads.load(), 2);
  st = get(r.port(), "/files/put/status?system=gb&path=S%2Fpart.bin", kPin);
  CHECK(st.find("{\"have\":3,\"done\":false,\"size\":0}") != string::npos);
}

void test_resume() {
  Rig r;
  webtransfer::set_enabled(true);
  const string all = pattern(1000);
  string x = put_raw(r.port(), slice_q("G%2Fbig.bin", 0, 1000), all.substr(0, 400));
  CHECK_EQ(status_of(x), 200);
  CHECK(x.find("\"have\":400,\"done\":false") != string::npos);
  CHECK(!exists(r.gb + "/G/big.bin"));
  CHECK_EQ(size_of(r.gb + "/G/.big.bin.zfpart"), 400);
  string st = get(r.port(), "/files/put/status?system=gb&path=G%2Fbig.bin", kPin);
  CHECK(st.find("{\"have\":400,\"done\":false,\"size\":0}") != string::npos);
  x = put_raw(r.port(), slice_q("G%2Fbig.bin", 400, 1000), all.substr(400));
  CHECK_EQ(status_of(x), 200);
  CHECK(x.find("\"done\":true") != string::npos);
  CHECK(slurp(r.gb + "/G/big.bin") == all);
  CHECK(!exists(r.gb + "/G/.big.bin.zfpart"));

  // a connection cut in the middle of a slice keeps what arrived; the client asks and continues from there
  {
    int fd = connect_to(r.port());
    CHECK(fd >= 0);
    send_str(fd, put_head(slice_q("G%2Fcut.bin", 0, 1000), 600) + all.substr(0, 250));
    ::close(fd);
  }
  CHECK(wait_size(r.gb + "/G/.cut.bin.zfpart", 250));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  st = get(r.port(), "/files/put/status?system=gb&path=G%2Fcut.bin", kPin);
  CHECK(st.find("{\"have\":250,\"done\":false,\"size\":0}") != string::npos);
  CHECK(!exists(r.gb + "/G/cut.bin"));
  // wrong offsets: 409 with what the console has, and the .part is not touched
  for (size_t off : {size_t(100), size_t(400), size_t(251)}) {
    x = put_raw(r.port(), slice_q("G%2Fcut.bin", off, 1000), all.substr(off, 100));
    CHECK_EQ(status_of(x), 409);
    CHECK(x.find("\"have\":250") != string::npos);
    CHECK_EQ(size_of(r.gb + "/G/.cut.bin.zfpart"), 250);
  }
  x = put_raw(r.port(), slice_q("G%2Fnopart.bin", 5, 1000), "abc");  // no .part at all
  CHECK_EQ(status_of(x), 409);
  CHECK(x.find("\"have\":0") != string::npos);
  CHECK(!exists(r.gb + "/G/.nopart.bin.zfpart"));
  x = put_raw(r.port(), slice_q("G%2Fcut.bin", 250, 1000), all.substr(250));
  CHECK_EQ(status_of(x), 200);
  CHECK(slurp(r.gb + "/G/cut.bin") == all);
  CHECK(!exists(r.gb + "/G/.cut.bin.zfpart"));

  // offset 0 starts again: the old .part is truncated
  write_file(r.gb + "/G/.again.bin.zfpart", pattern(900));
  CHECK_EQ(put(r.port(), slice_q("G%2Fagain.bin", 0, 500), all.substr(0, 200)), 200);
  CHECK_EQ(size_of(r.gb + "/G/.again.bin.zfpart"), 200);
  CHECK(slurp(r.gb + "/G/.again.bin.zfpart") == all.substr(0, 200));

  // every byte arrived but the cut came before the rename: an empty slice at the end finishes the file
  write_file(r.gb + "/G/.fin.bin.zfpart", all);
  x = put_raw(r.port(), slice_q("G%2Ffin.bin", 1000, 1000), "");
  CHECK_EQ(status_of(x), 200);
  CHECK(slurp(r.gb + "/G/fin.bin") == all);
  CHECK(!exists(r.gb + "/G/.fin.bin.zfpart"));
}

void test_bad_requests() {
  Rig r;
  webtransfer::set_enabled(true);
  const string long_part(150, 'a');
  for (const string& bad : {string("..%2Fx.gb"), string("%2E%2E%2Fx.gb"), string("G%2F..%2Fx.gb"), string(".hidden%2Fa.gb"), string("G%2F.hid.bin"),
                            string("%2Fabs%2Fa.gb"), string("a%5Cb.gb"), string("a%2F%2Fb.bin"), string("a%2F"), string("a%2Fb%2Fc%2Fd%2Fe.bin"),
                            string("a%00b.gb"), string(""), long_part + "%2F" + long_part + "%2F" + long_part + "%2Fx.bin"}) {
    int s = put_whole(r.port(), bad, "abc");
    if (s != 400) std::fprintf(stderr, "path '%s' answered %d\n", bad.c_str(), s);
    CHECK_EQ(s, 400);
    CHECK_EQ(status_of(get(r.port(), "/files/put/status?system=gb&path=" + bad, kPin)), 400);
  }
  CHECK(list(r.gb).empty());
  CHECK(list(r.root) == std::vector<string>{"gb"});
  // the query itself
  CHECK_EQ(put(r.port(), "system=gb&path=G%2Fa.bin&total=3", "abc"), 400);
  CHECK_EQ(put(r.port(), "system=gb&path=G%2Fa.bin&offset=x&total=3", "abc"), 400);
  CHECK_EQ(put(r.port(), "system=gb&path=G%2Fa.bin&offset=-1&total=3", "abc"), 400);
  CHECK_EQ(put(r.port(), "system=nope&path=G%2Fa.bin&offset=0&total=3", "abc"), 400);
  CHECK_EQ(put(r.port(), "path=G%2Fa.bin&offset=0&total=3", "abc"), 400);
  CHECK_EQ(put(r.port(), slice_q("G%2Fa.bin", 0, 2), "abc"), 400);  // more bytes than the total
  CHECK_EQ(put(r.port(), slice_q("G%2Fa.bin", 2, 3), "abc"), 400);  // goes past the total
  CHECK_EQ(put(r.port(), slice_q("G%2Fa.bin", 1, 3), ""), 400);     // empty slice that does not end the file
  CHECK_EQ(put(r.port(), slice_q("a.gb", 0, 0), ""), 400);          // empty file at the top level (as /files/upload)
  CHECK(list(r.gb).empty());
  CHECK_EQ(put(r.port(), slice_q("G%2Fempty.bin", 0, 0), ""), 200);  // inside a folder an empty file is fine
  CHECK_EQ(size_of(r.gb + "/G/empty.bin"), 0);
  // a slice over 16 MB is refused before any byte is read
  CHECK_EQ(status_of(talk(r.port(), put_head(slice_q("G%2Fhuge.bin", 0, 40u << 20), (16u << 20) + 1))), 413);
  CHECK(!exists(r.gb + "/G/.huge.bin.zfpart"));
}

void test_symlinks() {
  Rig r;
  webtransfer::set_enabled(true);
  const string outside = r.root + "/outside";
  ::mkdir(outside.c_str(), 0755);
  write_file(outside + "/victim", "orig");
  CHECK(::symlink(outside.c_str(), (r.gb + "/lnk").c_str()) == 0);
  ::mkdir((r.gb + "/Game").c_str(), 0755);
  CHECK(::symlink(outside.c_str(), (r.gb + "/Game/lnk2").c_str()) == 0);
  // a symlinked folder, at the first level or deeper, is not followed
  CHECK_EQ(put_whole(r.port(), "lnk%2Fx.bin", "abc"), 400);
  CHECK_EQ(put_whole(r.port(), "lnk%2Fsub%2Fx.bin", "abc"), 400);
  CHECK_EQ(put_whole(r.port(), "Game%2Flnk2%2Fx.bin", "abc"), 400);
  CHECK_EQ(status_of(get(r.port(), "/files/put/status?system=gb&path=lnk%2Fx.bin", kPin)), 400);
  CHECK(list(outside) == std::vector<string>{"victim"});
  // a symlink in place of the .part, at the top level and in a folder
  CHECK(::symlink((outside + "/victim").c_str(), (r.gb + "/.f.gb.zfpart").c_str()) == 0);
  CHECK(::symlink((outside + "/victim").c_str(), (r.gb + "/Game/.g.bin.zfpart").c_str()) == 0);
  CHECK_EQ(put_whole(r.port(), "f.gb", "evil"), 500);
  CHECK_EQ(put_whole(r.port(), "Game%2Fg.bin", "evil"), 500);
  CHECK_EQ(put(r.port(), slice_q("Game%2Fg.bin", 2, 10), "evil"), 500);  // a resume does not follow it either
  // a symlink in place of the target
  CHECK(::symlink((outside + "/victim").c_str(), (r.gb + "/t.gb").c_str()) == 0);
  CHECK_EQ(put_whole(r.port(), "t.gb", "evil"), 409);
  CHECK_EQ(put_whole(r.port(), "t.gb", "evil", "&replace=1"), 400);
  CHECK(slurp(outside + "/victim") == "orig");
  CHECK(list(outside) == std::vector<string>{"victim"});
}

void test_rules() {
  Rig r;
  webtransfer::set_enabled(true);
  // top level: the system's extensions (any case); inside a folder: anything
  CHECK_EQ(put_whole(r.port(), "a.txt", "x"), 400);
  CHECK_EQ(put_whole(r.port(), "noext", "x"), 400);
  CHECK_EQ(put_whole(r.port(), "ok.gb", "x"), 200);
  CHECK_EQ(put_whole(r.port(), "ok.gbc", "x"), 200);
  CHECK_EQ(put_whole(r.port(), "OK2.GB", "x"), 200);
  CHECK_EQ(put_whole(r.port(), "G%2Fa.txt", "x"), 200);
  CHECK_EQ(put_whole(r.port(), "G%2Fnoext", "x"), 200);
  CHECK(!exists(r.gb + "/a.txt") && !exists(r.gb + "/noext"));
  // replace
  CHECK_EQ(put_whole(r.port(), "G%2Fr.bin", "old"), 200);
  CHECK_EQ(put_whole(r.port(), "G%2Fr.bin", "new"), 409);
  CHECK(slurp(r.gb + "/G/r.bin") == "old");
  CHECK_EQ(put_whole(r.port(), "G%2Fr.bin", "new", "&replace=1"), 200);
  CHECK(slurp(r.gb + "/G/r.bin") == "new");
  // two slices over an existing file: refused up front without replace; with it the old file stays until the last slice
  const string all = pattern(100);
  CHECK_EQ(put(r.port(), slice_q("G%2Fr.bin", 0, 100), all.substr(0, 40)), 409);
  CHECK_EQ(put(r.port(), slice_q("G%2Fr.bin", 0, 100, "&replace=1"), all.substr(0, 40)), 200);
  CHECK(slurp(r.gb + "/G/r.bin") == "new");
  CHECK_EQ(put(r.port(), slice_q("G%2Fr.bin", 40, 100, "&replace=1"), all.substr(40)), 200);
  CHECK(slurp(r.gb + "/G/r.bin") == all);
  // a file that appears while the last slice is on its way is not overwritten
  CHECK_EQ(put(r.port(), slice_q("G%2Fwin.bin", 0, 100), all.substr(0, 40)), 200);
  write_file(r.gb + "/G/win.bin", "other");
  CHECK_EQ(put(r.port(), slice_q("G%2Fwin.bin", 40, 100), all.substr(40)), 409);
  CHECK(slurp(r.gb + "/G/win.bin") == "other");
  // the old route still works
  CHECK_EQ(status_of(talk(r.port(), string("POST /files/upload?system=gb&name=old.gb HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nX-PIN: ") + kPin + "\r\n\r\nabc")), 200);
  CHECK(slurp(r.gb + "/old.gb") == "abc");
}

void test_space() {
  Rig r;
  webtransfer::set_enabled(true);
  const string all = pattern(1000);
  r.fake_free = 999;
  CHECK_EQ(put_whole(r.port(), "Z%2Ff.bin", all), 413);
  CHECK(list(r.gb).empty());  // nothing was created, not even the folder
  r.fake_free = 1000;
  CHECK_EQ(put(r.port(), slice_q("Z%2Ff.bin", 0, 1000), all.substr(0, 400)), 200);
  r.fake_free = 599;  // a resume only needs the rest
  CHECK_EQ(put(r.port(), slice_q("Z%2Ff.bin", 400, 1000), all.substr(400)), 413);
  CHECK_EQ(size_of(r.gb + "/Z/.f.bin.zfpart"), 400);
  r.fake_free = 600;
  CHECK_EQ(put(r.port(), slice_q("Z%2Ff.bin", 400, 1000), all.substr(400)), 200);
  CHECK(slurp(r.gb + "/Z/f.bin") == all);
}

void test_gates() {
  Rig r;
  const string q = slice_q("G%2Fa.bin", 0, 3);
  CHECK_EQ(put(r.port(), q, "abc"), 404);  // switched off
  CHECK_EQ(status_of(get(r.port(), "/files/put/status?system=gb&path=G%2Fa.bin", kPin)), 404);
  webtransfer::set_enabled(true);
  CHECK_EQ(put(r.port(), q, "abc", nullptr), 403);
  CHECK_EQ(put(r.port(), q, "abc", "0000"), 403);
  CHECK_EQ(status_of(get(r.port(), "/files/put/status?system=gb&path=G%2Fa.bin")), 403);
  CHECK(list(r.gb).empty());
  // GET on the streamed path is not a route; POST on the status path is refused
  CHECK_EQ(status_of(get(r.port(), "/files/put?" + q, kPin)), 404);
  CHECK_EQ(status_of(talk(r.port(), string("POST /files/put/status?system=gb&path=G%2Fa.bin HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nX-PIN: ") + kPin + "\r\n\r\n")), 405);
  CHECK_EQ(put(r.port(), q, "abc"), 200);
  webtransfer::set_enabled(false);
  CHECK_EQ(put(r.port(), slice_q("G%2Fb.bin", 0, 3), "abc"), 404);
  CHECK(!exists(r.gb + "/G/b.bin"));
}

// Leftovers of uploads nobody resumed are swept when a file starts in that folder; nothing else is touched. The rescan route.
void test_tidy() {
  Rig r;
  webtransfer::set_enabled(true);
  CHECK(::mkdir((r.gb + "/T").c_str(), 0755) == 0);
  const string old_part = r.gb + "/T/.old.bin.zfpart", fresh = r.gb + "/T/.fresh.bin.zfpart", mine = r.gb + "/T/keep.part",
               hidden = r.gb + "/T/.notes.txt";
  for (const string* f : {&old_part, &fresh, &mine, &hidden}) write_file(*f, "abc");
  struct timespec ts[2];
  ts[0].tv_sec = ts[1].tv_sec = ::time(nullptr) - 3 * 24 * 3600;
  ts[0].tv_nsec = ts[1].tv_nsec = 0;
  for (const string* f : {&old_part, &mine, &hidden}) CHECK(::utimensat(AT_FDCWD, f->c_str(), ts, 0) == 0);
  CHECK_EQ(put(r.port(), slice_q("T%2Fnew.bin", 0, 10), "abc"), 200);  // a cut upload stays as a hidden file
  CHECK(!exists(old_part));
  CHECK(exists(fresh));
  CHECK(exists(mine));    // not our suffix
  CHECK(exists(hidden));  // not our suffix
  CHECK_EQ(size_of(r.gb + "/T/.new.bin.zfpart"), 3);
  CHECK(!exists(r.gb + "/T/new.bin.part"));
  // a resume (offset > 0) does not sweep
  CHECK(::utimensat(AT_FDCWD, fresh.c_str(), ts, 0) == 0);
  CHECK_EQ(put(r.port(), slice_q("T%2Fnew.bin", 3, 10), "defghij"), 200);
  CHECK(exists(fresh));
  CHECK(slurp(r.gb + "/T/new.bin") == "abcdefghij");
  // the status of a finished file says its size
  string st = get(r.port(), "/files/put/status?system=gb&path=T%2Fnew.bin", kPin);
  CHECK(st.find("{\"have\":0,\"done\":true,\"size\":10}") != string::npos);
  // rescan: POST only, PIN needed, asks once
  const int before = r.reloads.load();
  const string post = "POST /files/put/rescan HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n";
  CHECK_EQ(status_of(talk(r.port(), post + "X-PIN: " + kPin + "\r\n\r\n")), 200);
  CHECK_EQ(r.reloads.load(), before + 1);
  CHECK_EQ(status_of(talk(r.port(), post + "\r\n")), 403);
  CHECK_EQ(status_of(get(r.port(), "/files/put/rescan", kPin)), 405);
  CHECK_EQ(r.reloads.load(), before + 1);
}

void test_page() {
  const string html = webtransfer::page_html();
  for (const char* want : {"/files/put?", "/files/put/status?", "/files/put/rescan", "r.busy", "r.size===f.size", "webkitdirectory", "webkitRelativePath", "Reconnecting", "f.slice(", "ondrop"}) {
    bool found = html.find(want) != string::npos;
    if (!found) std::fprintf(stderr, "page lacks %s\n", want);
    CHECK(found);
  }
}

}  // namespace

int main() {
  test_nested_and_status();
  test_resume();
  test_bad_requests();
  test_symlinks();
  test_rules();
  test_space();
  test_gates();
  test_tidy();
  test_page();
  return zt::done();
}
