// websaves: the Saves tab of the web transfer (download, upload, restore of save backups). The real httpd server on 127.0.0.1 with the
// stream worker, a fake /storage in a temp dir (ZF_WEBSAVES_STORAGE), and the console's jobs run by hand (`pump`): the job the route
// asks the main thread for is queued by `Env::on_main` and run here with a real App, so busy / one-at-a-time is observable.
// Malicious archives are built byte by byte (tar headers written here) and must be refused with nothing left behind.
#include <arpa/inet.h>
#include <dirent.h>
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
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "httpd.hpp"
#include "menu.hpp"
#include "oscontract.hpp"
#include "savebackup.hpp"
#include "util.hpp"
#include "webcore.hpp"
#include "webtransfer.hpp"

using namespace zf;
using std::string;
namespace sb = zf::savebackup;

namespace {

const char* kPin = "4821";

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

// ---- http helpers ------------------------------------------------------------------------------------------------------------

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

string read_all(int fd, int ms = 8000) {
  string out;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  for (;;) {
    int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count());
    if (left <= 0) break;
    pollfd pf{fd, POLLIN, 0};
    if (::poll(&pf, 1, left) <= 0) break;
    char b[8192];
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

string body_of(const string& r) {
  size_t p = r.find("\r\n\r\n");
  return p == string::npos ? string() : r.substr(p + 4);
}

string talk(uint16_t port, const string& raw) {
  int fd = connect_to(port);
  if (fd < 0) return "";
  send_str(fd, raw);
  string r = read_all(fd);
  ::close(fd);
  return r;
}

string req(uint16_t port, const char* method, const string& path, const char* pin = kPin, const string& body = "") {
  string h = string(method) + " " + path + " HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  return talk(port, h + "\r\n" + body);
}
string get(uint16_t port, const string& path, const char* pin = kPin) { return req(port, "GET", path, pin); }
string post(uint16_t port, const string& path, const char* pin = kPin) { return req(port, "POST", path, pin); }
int put(uint16_t port, const string& name, const string& data, string* out = nullptr) {
  string r = req(port, "POST", "/files/saves/put?name=" + name, kPin, data);
  if (out) *out = r;
  return status_of(r);
}

// ---- files -------------------------------------------------------------------------------------------------------------------

void mkdirs(const string& path) {
  for (size_t i = 1; i <= path.size(); ++i)
    if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
}

string slurp(const string& p) {
  string s;
  read_file(p, s);
  return s;
}

void spit(const string& p, const string& content) {
  mkdirs(p.substr(0, p.rfind('/')));
  std::ofstream(p, std::ios::binary) << content;
}

std::vector<string> names(const string& dir) {
  std::vector<string> v;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d))
      if (string(e->d_name) != "." && string(e->d_name) != "..") v.push_back(e->d_name);
    ::closedir(d);
  }
  std::sort(v.begin(), v.end());
  return v;
}

// ---- tar by hand -------------------------------------------------------------------------------------------------------------

string tar_header(const string& name, char type, uint64_t size, const string& link = "") {
  char h[512];
  std::memset(h, 0, sizeof h);
  std::memcpy(h, name.data(), std::min<size_t>(name.size(), 100));
  std::snprintf(h + 100, 8, "%07o", 0644);
  std::snprintf(h + 108, 8, "%07o", 0);
  std::snprintf(h + 116, 8, "%07o", 0);
  std::snprintf(h + 124, 12, "%011llo", (unsigned long long)size);
  std::snprintf(h + 136, 12, "%011o", 0);
  std::memset(h + 148, ' ', 8);
  h[156] = type;
  std::memcpy(h + 157, link.data(), std::min<size_t>(link.size(), 100));
  std::memcpy(h + 257, "ustar\0", 6);
  std::memcpy(h + 263, "00", 2);
  unsigned sum = 0;
  for (char c : h) sum += (unsigned char)c;
  std::snprintf(h + 148, 7, "%06o", sum);
  h[154] = 0;
  h[155] = ' ';
  return string(h, 512);
}

string tar_entry(const string& name, char type, const string& data = "", const string& link = "") {
  string e = tar_header(name, type, data.size(), link) + data;
  e.append((512 - data.size() % 512) % 512, '\0');
  return e;
}

string tar_end() { return string(1024, '\0'); }

string pax_rec(const string& key, const string& val) {
  const string rest = " " + key + "=" + val + "\n";
  size_t len = rest.size();
  for (int i = 0; i < 4; ++i) len = rest.size() + std::to_string(len).size();
  return std::to_string(len) + rest;
}

string gz(const string& tar, const string& tmp) {
  spit(tmp + "/x.tar", tar);
  string cmd = "gzip -c '" + tmp + "/x.tar' > '" + tmp + "/x.tar.gz'";
  if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "gzip failed\n");
  return slurp(tmp + "/x.tar.gz");
}

// ---- the rig -----------------------------------------------------------------------------------------------------------------

struct Rig {
  string root, st, bk, tmp;
  std::atomic<int> reloads{0};
  http::Server srv;
  webtransfer::Env env;
  std::mutex qm;
  std::deque<std::function<void(App&)>> queue;
  std::unique_ptr<App> app;

  struct Fake : Screen {
    Transition handle(Action, Ctx&) override { return Transition::none(); }
    void draw(Ctx&, std::vector<Op>&) override {}
  };

  Rig() {
    char t[] = "/tmp/zf-websaves-XXXXXX";
    root = ::mkdtemp(t);
    st = root + "/storage";
    bk = st + "/roms/backup";
    tmp = root + "/tmp";
    mkdirs(st);
    mkdirs(tmp);
    ::setenv("ZF_WEBSAVES_STORAGE", st.c_str(), 1);
    ::setenv("ZF_WEBSAVES_TMP", tmp.c_str(), 1);
    spit(st + "/roms/savestates/gba/Game.state1", "state-one");
    spit(st + "/roms/gba/Game.srm", "battery-original");
    spit(st + "/roms/gba/Game.gba", "ROM-NOT-A-SAVE");
    spit(st + "/roms/snes/My Game (USA).sav", "space-and-parens");
    spit(st + "/roms/psp/PSP/SAVEDATA/ULUS10001DATA00/DATA.BIN", "psp-save");
    spit(st + "/.config/ppsspp/PSP/SYSTEM/ppsspp.ini", "ini");
    Ctx cx;
    string err;
    CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
    cx.w = 640, cx.h = 480;
    cx.sv->cmd.dry_run = false;
    app = std::make_unique<App>(std::move(cx), std::make_unique<Fake>(), make_status_source("/nonexistent"));
    webtransfer::reset_for_test(kPin);
    env.reload = [this] { ++reloads; };
    env.on_main = [this](std::function<void(App&)> f) {
      std::lock_guard<std::mutex> lk(qm);
      queue.push_back(std::move(f));
    };
    srv.set_stream_handler([this](const http::Request& r, http::BodyStream& b) { return webtransfer::stream_route(r, b, env); }, webtransfer::stream_pred);
    string e;
    bool ok = srv.start(
        0,
        [this](const http::Request& r) {
          if (r.path.compare(0, 6, "/files") == 0) return webtransfer::route(r, env);
          http::Response x;
          x.status = 404;
          return x;
        },
        e);
    CHECK(ok);
  }
  ~Rig() {
    srv.stop();
    app.reset();
    string cmd = "rm -rf '" + root + "'";
    if (std::system(cmd.c_str()) != 0) {
    }
    ::unsetenv("ZF_WEBSAVES_STORAGE");
    ::unsetenv("ZF_WEBSAVES_TMP");
  }
  uint16_t port() const { return srv.port(); }
  // The question a restore opens on the console: run what the route queued (the dialog), then answer it.
  void answer(bool yes) {
    pump();
    if (yes) {
      app->handle(Action::Left);
      app->handle(Action::A);
    } else {
      app->handle(Action::B);
    }
    pump();
  }
  // The main thread's turn: run what the routes asked for, then let the console's jobs finish.
  void pump() {
    for (;;) {
      std::function<void(App&)> f;
      {
        std::lock_guard<std::mutex> lk(qm);
        if (queue.empty()) break;
        f = std::move(queue.front());
        queue.pop_front();
      }
      f(*app);
    }
    for (int i = 0; i < 3000; ++i) {
      app->cx.sv->jobs.drain();
      if (app->cx.sv->jobs.idle()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    app->cx.sv->jobs.drain();
  }
};

bool has(const string& s, const char* t) { return s.find(t) != string::npos; }

void test_in_plan() {
  for (const char* ok : {"roms/savestates/gba/Game.state1", "roms/gba/Game.srm", "roms/gba/Game.SAV", "roms/gba/Game.state.auto", "roms/snes/a/b/c.state2",
                         "roms/psp/PSP/SAVEDATA/X/DATA.BIN", ".config/ppsspp/PSP/SYSTEM/ppsspp.ini", "roms/psp/foo.srm"})
    CHECK(sb::in_plan(ok, false));
  for (const char* bad : {"", "/etc/passwd", "roms/gba/Game.gba", "roms/gba/Game.zip", "roms/backup/saves-20261109-1430.tar.gz", "roms/backup/stray.sav",
                          "roms/psp/PSP/GAME/Homebrew/EBOOT.PBP", "roms/psp/PSP/GAME/x.srm", ".config/ppsspp/PSP/GAME/other.bin", ".config/other/x.sav", "etc/passwd",
                          "roms/gba/images/x.srm", "roms/videos/intro.srm", "storage/roms/gba/Game.srm", "roms", "Game.srm", "roms/gba/manuals/m.sav"})
    CHECK(!sb::in_plan(bad, false));
  for (const char* ok : {"roms", "roms/gba", "roms/savestates", "roms/savestates/gba", "roms/psp/PSP", ".config", ".config/ppsspp/PSP/SYSTEM"})
    CHECK(sb::in_plan(ok, true));
  for (const char* bad : {"etc", "roms/backup", "roms/gba/images", ".config/other", "home"}) CHECK(!sb::in_plan(bad, true));
}

void test_off_and_auth() {
  Rig r;  // enabled is false
  CHECK_EQ(status_of(get(r.port(), "/files/saves")), 404);
  CHECK_EQ(status_of(post(r.port(), "/files/saves/create")), 404);
  CHECK_EQ(status_of(get(r.port(), "/files/saves/status")), 404);
  CHECK_EQ(status_of(get(r.port(), "/files/saves/get?name=saves-20261109-1430.tar.gz")), 404);
  CHECK_EQ(status_of(post(r.port(), "/files/saves/restore?name=saves-20261109-1430.tar.gz&confirm=1")), 404);
  CHECK_EQ(put(r.port(), "saves-20261109-1430.tar.gz", "abc"), 404);
  CHECK(r.queue.empty());
  // five wrong PINs lock everything for 30 s: start over between the groups
  auto fresh = [] {
    webtransfer::reset_for_test(kPin);
    webtransfer::set_enabled(true);
  };
  fresh();
  for (const char* p : {"/files/saves", "/files/saves/status", "/files/saves/get?name=saves-20261109-1430.tar.gz"}) {
    CHECK_EQ(status_of(get(r.port(), p, nullptr)), 403);
    CHECK_EQ(status_of(get(r.port(), p, "0000")), 403);
    fresh();
  }
  fresh();
  CHECK_EQ(status_of(post(r.port(), "/files/saves/create", nullptr)), 403);
  CHECK_EQ(status_of(post(r.port(), "/files/saves/restore?name=saves-20261109-1430.tar.gz&confirm=1", "0000")), 403);
  CHECK_EQ(status_of(req(r.port(), "POST", "/files/saves/put?name=saves-20261109-1430.tar.gz", nullptr, "abc")), 403);
  CHECK_EQ(status_of(req(r.port(), "POST", "/files/saves/put?name=saves-20261109-1430.tar.gz", "0000", "abc")), 403);
  CHECK_EQ(status_of(post(r.port(), "/files/saves/create", "0000")), 403);
  fresh();
  CHECK(r.queue.empty());
  CHECK(names(r.bk).empty());
  // methods
  CHECK_EQ(status_of(post(r.port(), "/files/saves")), 405);
  CHECK_EQ(status_of(get(r.port(), "/files/saves/create")), 405);
  CHECK_EQ(status_of(get(r.port(), "/files/saves/restore?name=x")), 405);
  webtransfer::set_enabled(false);
}

// create -> busy -> status -> list -> download; one operation at a time
void test_create_and_get() {
  Rig r;
  webtransfer::set_enabled(true);
  string l = body_of(get(r.port(), "/files/saves"));
  CHECK(has(l.c_str(), "\"backups\":[]") && has(l.c_str(), "\"busy\":false") && has(l.c_str(), "\"game\":false") && has(l.c_str(), "\"free\":"));
  CHECK(has(body_of(get(r.port(), "/files/saves/status")).c_str(), "\"last\":null"));
  CHECK_EQ(status_of(post(r.port(), "/files/saves/create")), 202);
  CHECK(has(body_of(get(r.port(), "/files/saves/status")).c_str(), "\"busy\":true"));
  CHECK(has(body_of(get(r.port(), "/files/saves")).c_str(), "\"busy\":true"));
  CHECK_EQ(status_of(post(r.port(), "/files/saves/create")), 409);  // one at a time (also against the menu: same flag)
  CHECK(names(r.bk).empty());                                       // nothing ran on the HTTP thread
  r.pump();
  string s = body_of(get(r.port(), "/files/saves/status"));
  CHECK(has(s.c_str(), "\"busy\":false") && has(s.c_str(), "\"op\":\"create\",\"ok\":true") && has(s.c_str(), "saves-"));
  std::vector<string> n = names(r.bk);
  CHECK_EQ(n.size(), size_t(1));
  CHECK(!n.empty() && sb::valid_name(n[0]));
  l = body_of(get(r.port(), "/files/saves"));
  CHECK(!n.empty() && has(l.c_str(), ("\"name\":\"" + n[0] + "\"").c_str()) && has(l.c_str(), "\"date\":\"20"));
  // the download is the file, byte for byte, as an attachment
  if (!n.empty()) {
    string d = get(r.port(), "/files/saves/get?name=" + n[0]);
    CHECK_EQ(status_of(d), 200);
    CHECK(has(d.c_str(), ("Content-Disposition: attachment; filename=\"" + n[0] + "\"").c_str()));
    CHECK(has(d.c_str(), "application/gzip"));
    const string file = slurp(r.bk + "/" + n[0]);
    CHECK(file.size() > 50);
    CHECK(body_of(d) == file);
    // and it passes its own check: uploading it again under another name is accepted
    CHECK_EQ(put(r.port(), "saves-20200101-0000.tar.gz", file), 200);
    CHECK_EQ(names(r.bk).size(), size_t(2));
    CHECK(slurp(r.bk + "/saves-20200101-0000.tar.gz") == file);
    CHECK_EQ(put(r.port(), "saves-20200101-0000.tar.gz", file), 409);  // never overwritten
    CHECK(slurp(r.bk + "/saves-20200101-0000.tar.gz") == file);
  }
  // names: only listed archives
  for (const char* bad : {"", "../../etc/passwd", "saves-20261109-1430.tar.gz/../x", "/etc/passwd", "other.tar.gz"})
    CHECK_EQ(status_of(get(r.port(), string("/files/saves/get?name=") + bad)), 400);
  CHECK_EQ(status_of(get(r.port(), "/files/saves/get?name=saves-19990101-0000.tar.gz")), 404);  // valid name, not there
  CHECK_EQ(status_of(get(r.port(), "/files/saves/get")), 400);
  CHECK_EQ(status_of(get(r.port(), "/files/saves/get?name=saves-20200101-0000.tar.gz.part")), 400);
  webtransfer::set_enabled(false);
}

// What the check lets through and what it refuses; a refused upload leaves nothing (no archive, no .part)
void test_upload_check() {
  Rig r;
  webtransfer::set_enabled(true);
  const string good_save = "state";
  auto upload = [&](const string& tar, int want, const char* label) {
    string out;
    int st = put(r.port(), "saves-20210101-0101.tar.gz", gz(tar, r.tmp), &out);
    if (st != want) std::fprintf(stderr, "%s: got %d, wanted %d: %s\n", label, st, want, body_of(out).c_str());
    CHECK_EQ(st, want);
    if (want == 422) {
      CHECK(names(r.bk).empty());
      CHECK(has(body_of(out).c_str(), "\"error\":\""));
    } else if (want == 200) {
      ::unlink((r.bk + "/saves-20210101-0101.tar.gz").c_str());
    }
    for (const string& f : names(r.bk)) CHECK(f.find(".part") == string::npos);
  };
  const string fine = tar_entry("roms/gba/Game.srm", '0', good_save);
  upload(fine + tar_end(), 200, "plain");
  upload(tar_entry("roms", '5') + tar_entry("roms/gba/", '5') + tar_entry("roms/gba/Game.srm", '0', good_save) + tar_end(), 200, "with folders");
  upload(tar_entry("roms/savestates/" + string(120, 'a') + "/x.state1", 'L', "") + tar_end(), 422, "longname header without file");
  {  // GNU long name for a file under a save folder
    const string longname = "roms/savestates/" + string(150, 'd') + "/Game.state1";
    upload(tar_entry("././@LongLink", 'L', longname + string(1, '\0')) + tar_entry("short", '0', good_save) + tar_end(), 200, "gnu long name");
    upload(tar_entry("././@LongLink", 'L', string("../../etc/x") + string(1, '\0')) + tar_entry("roms/gba/Game.srm", '0', good_save) + tar_end(), 422, "long name escape");
  }
  {  // pax: the console's tar ignores these headers, so one that renames or resizes an entry is refused
    upload(tar_entry("PaxHeader", 'x', pax_rec("path", "roms/snes/Zelda.srm")) + tar_entry("x", '0', good_save) + tar_end(), 422, "pax path over another name");
    upload(tar_entry("PaxHeader", 'x', pax_rec("path", "roms/gba/Game.srm")) + tar_entry(".config/autostart/x.sh", '0', good_save) + tar_end(), 422,
           "pax path hiding the name");
    upload(tar_entry("PaxHeader", 'x', pax_rec("path", "/etc/cron.d/x")) + tar_entry("roms/gba/Game.srm", '0', good_save) + tar_end(), 422, "pax absolute");
    upload(tar_entry("PaxHeader", 'x', pax_rec("linkpath", "/etc/passwd")) + fine + tar_end(), 422, "pax linkpath");
    upload(tar_entry("PaxHeader", 'x', pax_rec("mtime", "1600000000.5") + pax_rec("atime", "1600000000")) + fine + tar_end(), 200, "pax times only");
    upload(tar_entry("PaxHeader", 'x', pax_rec("mtime", "1600000000.5")) + tar_entry(".config/autostart/x.sh", '0', good_save) + tar_end(), 422,
           "pax times, name outside");
    upload(tar_entry("pax_global_header", 'g', pax_rec("comment", "x")) + fine + tar_end(), 422, "pax global");
    // pax size: the checker would jump over 1024 bytes and never see the entry hidden in them; the console's tar reads the header's 0
    string body = tar_entry(".config/autostart/evil.sh", '0', "#!/bin/sh\n");
    body.append(1024 - body.size(), '\0');
    upload(tar_entry("PaxHeader", 'x', pax_rec("size", "1024")) + tar_header("roms/gba/Game.srm", '0', 0) + body + fine + tar_end(), 422,
           "pax size hiding an entry");
  }
  upload(tar_entry("/etc/passwd", '0', good_save) + tar_end(), 422, "absolute path");
  upload(fine + tar_entry("/roms/gba/Game.srm", '0', good_save) + tar_end(), 422, "absolute path after a good one");
  upload(tar_entry("../x", '0', good_save) + tar_end(), 422, "dotdot");
  upload(tar_entry("roms/savestates/../../x.srm", '0', good_save) + tar_end(), 422, "dotdot inside");
  upload(tar_entry("roms/savestates/a/../b.state1", '0', good_save) + tar_end(), 422, "dotdot in the middle");
  upload(tar_entry("./roms/gba/Game.srm", '0', good_save) + tar_end(), 422, "leading dot component");
  upload(tar_entry("roms//gba/Game.srm", '0', good_save) + tar_end(), 422, "empty component");
  upload(fine + tar_entry("roms/gba/link.srm", '2', "", "/etc/passwd") + tar_end(), 422, "symlink");
  upload(fine + tar_entry("roms/gba/link.srm", '1', "", "roms/gba/Game.srm") + tar_end(), 422, "hardlink");
  upload(fine + tar_entry("roms/gba/dev.srm", '3', "") + tar_end(), 422, "char device");
  upload(fine + tar_entry("roms/gba/blk.srm", '4', "") + tar_end(), 422, "block device");
  upload(fine + tar_entry("roms/gba/fifo.srm", '6', "") + tar_end(), 422, "fifo");
  upload(fine + tar_entry("roms/gba/Game.gba", '0', "ROM") + tar_end(), 422, "a ROM is not a save");
  upload(fine + tar_entry("etc/shadow", '0', "x") + tar_end(), 422, "outside the planned folders");
  upload(fine + tar_entry("roms/backup/saves-20200101-0000.tar.gz", '0', "x") + tar_end(), 422, "inside the backup folder");
  upload(fine + tar_entry(".config/other/x.sav", '0', "x") + tar_end(), 422, "other config");
  upload(fine + tar_entry("storage/roms/gba/Game.srm", '0', "x") + tar_end(), 422, "storage prefix");
  upload(tar_end(), 422, "empty archive");
  upload(tar_entry("roms/gba", '5') + tar_end(), 422, "only folders");
  // not gzip / not tar / cut short / bad header
  {
    string out;
    CHECK_EQ(put(r.port(), "saves-20210101-0101.tar.gz", "this is not an archive at all", &out), 422);
    CHECK(has(body_of(out).c_str(), "not a gzip"));
    CHECK_EQ(put(r.port(), "saves-20210101-0101.tar.gz", string("\x1f\x8b", 2) + "junk junk junk", &out), 422);
    CHECK_EQ(put(r.port(), "saves-20210101-0101.tar.gz", gz("just some text, not a tar archive\n", r.tmp), &out), 422);
    string whole = gz(fine + tar_end(), r.tmp);
    CHECK_EQ(put(r.port(), "saves-20210101-0101.tar.gz", whole.substr(0, whole.size() / 2), &out), 422);
    string cut = fine + tar_entry("roms/gba/Other.srm", '0', string(2000, 'z'));
    cut.resize(cut.size() - 700);
    CHECK_EQ(put(r.port(), "saves-20210101-0101.tar.gz", gz(cut, r.tmp), &out), 422);
    string badsum = fine + tar_end();
    badsum[148] = '7';
    CHECK_EQ(put(r.port(), "saves-20210101-0101.tar.gz", gz(badsum, r.tmp), &out), 422);
    string flipped = whole;  // a damaged deflate stream
    flipped[flipped.size() - 6] ^= 0x55;
    CHECK_EQ(put(r.port(), "saves-20210101-0101.tar.gz", flipped, &out), 422);
  }
  // too big once unpacked: the header claims 5 GB of data (nothing is read for it)
  upload(tar_header("roms/gba/Big.srm", '0', 5ull << 30) + tar_end(), 422, "too big unpacked");
  CHECK(names(r.bk).empty());
  // names and sizes of the upload itself
  CHECK_EQ(put(r.port(), "evil.tar.gz", "x"), 400);
  CHECK_EQ(put(r.port(), "..%2F..%2Fx.tar.gz", "x"), 400);
  CHECK_EQ(put(r.port(), "saves-20210101-0101.tar.gz.part", "x"), 400);
  CHECK_EQ(put(r.port(), "saves-20210101-0101.tar.gz", ""), 400);
  CHECK_EQ(status_of(req(r.port(), "POST", "/files/saves/put", kPin, "x")), 400);
  CHECK(names(r.bk).empty());
  // the flag is released after a refusal: a backup can start
  CHECK_EQ(status_of(post(r.port(), "/files/saves/create")), 202);
  r.pump();
  webtransfer::set_enabled(false);
}

// restore: confirmation, game running, one at a time, and the files really come back (with the automatic backup first)
void test_restore() {
  Rig r;
  webtransfer::set_enabled(true);
  CHECK_EQ(status_of(post(r.port(), "/files/saves/create")), 202);
  r.pump();
  std::vector<string> n = names(r.bk);
  CHECK_EQ(n.size(), size_t(1));
  if (n.empty()) return;
  const string name = n[0], q = "/files/saves/restore?name=" + name;
  spit(r.st + "/roms/gba/Game.srm", "changed-after-the-backup");
  spit(r.st + "/roms/gba/Newer.srm", "made-later");
  CHECK_EQ(status_of(post(r.port(), q)), 400);                      // no confirm
  CHECK_EQ(status_of(post(r.port(), q + "&confirm=0")), 400);
  CHECK_EQ(status_of(post(r.port(), q + "&confirm=yes")), 400);
  CHECK_EQ(status_of(post(r.port(), "/files/saves/restore?confirm=1")), 400);
  CHECK_EQ(status_of(post(r.port(), "/files/saves/restore?name=../../x&confirm=1")), 400);
  CHECK_EQ(status_of(post(r.port(), "/files/saves/restore?name=saves-19990101-0000.tar.gz&confirm=1")), 404);
  oscontract::game_started("/x/rom.gba", "Game");
  CHECK(has(body_of(get(r.port(), "/files/saves")).c_str(), "\"game\":true"));
  CHECK_EQ(status_of(post(r.port(), q + "&confirm=1")), 409);       // a game runs
  CHECK(r.queue.empty());
  oscontract::game_ended();
  CHECK(slurp(r.st + "/roms/gba/Game.srm") == "changed-after-the-backup");  // nothing touched
  CHECK_EQ(status_of(post(r.port(), q + "&confirm=1")), 202);
  CHECK_EQ(status_of(post(r.port(), q + "&confirm=1")), 409);       // one at a time
  CHECK_EQ(status_of(post(r.port(), "/files/saves/create")), 409);
  CHECK_EQ(put(r.port(), "saves-20200202-0202.tar.gz", "x"), 409);
  // the console is asked first: until someone answers there nothing is touched, and No leaves everything as it was
  r.pump();
  CHECK(has(body_of(get(r.port(), "/files/saves/status")).c_str(), "\"busy\":true"));
  CHECK(slurp(r.st + "/roms/gba/Game.srm") == "changed-after-the-backup");
  r.answer(false);
  string s = body_of(get(r.port(), "/files/saves/status"));
  CHECK(has(s.c_str(), "\"busy\":false") && has(s.c_str(), "\"op\":\"restore\",\"ok\":false") && has(s.c_str(), "declined on the console"));
  CHECK(slurp(r.st + "/roms/gba/Game.srm") == "changed-after-the-backup");
  CHECK_EQ(names(r.bk).size(), size_t(1));
  CHECK_EQ(r.reloads.load(), 0);
  // a game that starts while the question waits: Yes does nothing
  CHECK_EQ(status_of(post(r.port(), q + "&confirm=1")), 202);
  r.pump();
  oscontract::game_started("/x/rom.gba", "Game");
  r.answer(true);
  oscontract::game_ended();
  s = body_of(get(r.port(), "/files/saves/status"));
  CHECK(has(s.c_str(), "\"busy\":false") && has(s.c_str(), "\"ok\":false") && has(s.c_str(), "a game is running"));
  CHECK(slurp(r.st + "/roms/gba/Game.srm") == "changed-after-the-backup");
  // and with Yes the saves come back
  CHECK_EQ(status_of(post(r.port(), q + "&confirm=1")), 202);
  r.answer(true);
  s = body_of(get(r.port(), "/files/saves/status"));
  CHECK(has(s.c_str(), "\"op\":\"restore\",\"ok\":true"));
  CHECK(slurp(r.st + "/roms/gba/Game.srm") == "battery-original");
  CHECK(slurp(r.st + "/roms/gba/Newer.srm") == "made-later");       // extraction only adds / replaces, like the menu
  CHECK_EQ(r.reloads.load(), 1);
  // the automatic backup of the saves that were replaced is there, and holds them
  std::vector<string> after = names(r.bk);
  CHECK_EQ(after.size(), size_t(2));
  string pre;
  for (const string& f : after)
    if (sb::is_pre(f)) pre = f;
  CHECK(!pre.empty());
  if (!pre.empty()) {
    string cmd = "tar -xzOf '" + r.bk + "/" + pre + "' roms/gba/Game.srm > '" + r.tmp + "/pre.srm'";
    CHECK_EQ(std::system(cmd.c_str()), 0);
    CHECK(slurp(r.tmp + "/pre.srm") == "changed-after-the-backup");
  }
  // a backup that no longer extracts: the status says why, the flag is released
  spit(r.bk + "/saves-20220202-0202.tar.gz", "not an archive");
  CHECK_EQ(status_of(post(r.port(), "/files/saves/restore?name=saves-20220202-0202.tar.gz&confirm=1")), 202);
  r.answer(true);
  s = body_of(get(r.port(), "/files/saves/status"));
  CHECK(has(s.c_str(), "\"busy\":false") && has(s.c_str(), "\"ok\":false") && has(s.c_str(), "damaged"));
  CHECK(slurp(r.st + "/roms/gba/Game.srm") == "battery-original");
  CHECK_EQ(r.reloads.load(), 1);
  webtransfer::set_enabled(false);
}

void test_page() {
  string html = webtransfer::page_html();
  if (const char* d = std::getenv("ZF_PAGE_DUMP")) {
    std::ofstream(d, std::ios::binary) << html;
  }
  CHECK(has(html.c_str(), "id=\"t-sv\"") && has(html.c_str(), ">Saves</button>") && has(html.c_str(), "id=\"p-sv\""));
  CHECK(has(html.c_str(), "Back up now") && has(html.c_str(), "Upload a backup") && has(html.c_str(), "Restore") && has(html.c_str(), "Download"));
  CHECK(has(html.c_str(), "Replace the saves on the console with this backup? A backup of the current saves is made first."));
  CHECK(has(html.c_str(), "/files/saves/put?name=") && has(html.c_str(), "/files/saves/restore?name=") && has(html.c_str(), "&confirm=1"));
  for (const char* bad : {"https://", "src=", "href=", "@import", "<link", "<iframe", "//cdn", "<img src", "window.open", "eval("})
    CHECK(!has(html.c_str(), bad));
}

}  // namespace

int main() {
  test_in_plan();
  test_off_and_auth();
  test_create_and_get();
  test_upload_check();
  test_restore();
  test_page();
  return zt::done();
}
