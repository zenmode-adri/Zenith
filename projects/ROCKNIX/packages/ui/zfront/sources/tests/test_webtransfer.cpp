// webtransfer: the opt-in web upload. The real httpd server on 127.0.0.1 (port 0) with the real stream worker, ROM folders in
// a temp dir, free space injected. No network beyond localhost.
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "httpd.hpp"
#include "osapi.hpp"
#include "webcore.hpp"
#include "oscontract.hpp"
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

string upload_head(const string& query, size_t len, const char* pin = kPin, bool expect = false) {
  string h = "POST /files/upload?" + query + " HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(len) + "\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  if (expect) h += "Expect: 100-continue\r\n";
  return h + "\r\n";
}

int upload_status(uint16_t port, const string& query, const string& body, const char* pin = kPin) {
  return status_of(talk(port, upload_head(query, body.size(), pin) + body));
}

string get(uint16_t port, const string& path, const char* pin = nullptr) {
  string h = "GET " + path + " HTTP/1.1\r\nHost: x\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  return talk(port, h + "\r\n");
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

bool exists(const string& p) {
  struct stat sb;
  return ::lstat(p.c_str(), &sb) == 0;
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

string pattern(size_t n) {
  string s(n, '\0');
  uint32_t x = 12345;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    s[i] = char(x >> 24);
  }
  return s;
}

struct Rig {
  string root, gb;
  std::atomic<int> reloads{0};
  std::atomic<uint64_t> fake_free{~uint64_t(0)};
  std::atomic<bool> use_fake{false};
  http::Server srv;
  webtransfer::Env env;
  std::mutex lm;
  std::vector<string> launched, opened;
  string shots;

  explicit Rig(http::ServerLimits lim = http::ServerLimits()) {
    char t[] = "/tmp/zf-webtransfer-XXXXXX";
    root = ::mkdtemp(t);
    gb = root + "/gb";
    ::mkdir(gb.c_str(), 0755);
    webtransfer::reset_for_test(kPin);
    webtransfer::SystemInfo si;
    si.name = "gb";
    si.fullname = "Game Boy";
    si.rom_dir = gb;
    si.exts = {".gb", ".GBC", ".zip"};
    si.count = 7;
    webtransfer::publish_info({si});
    env.reload = [this] { ++reloads; };
    shots = root + "/shots";
    ::mkdir(shots.c_str(), 0755);
    env.shots_dir = shots;
    env.launch = [this](const string& p) {
      std::lock_guard<std::mutex> lk(lm);
      launched.push_back(p);
    };
    env.open_system = [this](const string& n) {
      std::lock_guard<std::mutex> lk(lm);
      opened.push_back(n);
    };
    env.free_space = [this](const string&, uint64_t& out) {
      out = use_fake ? fake_free.load() : ~uint64_t(0);
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
        err, "127.0.0.1", lim);
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

void test_off() {
  Rig r;  // enabled is false
  CHECK_EQ(status_of(get(r.port(), "/files")), 404);
  CHECK_EQ(status_of(get(r.port(), "/files/", kPin)), 404);
  CHECK_EQ(status_of(get(r.port(), "/files/systems", kPin)), 404);
  CHECK_EQ(upload_status(r.port(), "system=gb&name=a.gb", "abc"), 404);
  CHECK(list(r.gb).empty());
}

// Nothing on the page is fetched from anywhere but the console: every url( is the console's own /files/... or an inline
// data: picture, and "http://" only appears as the SVG namespace inside those pictures (an XML name, never fetched).
bool only_local_urls(const string& html) {
  for (size_t at = html.find("url("); at != string::npos; at = html.find("url(", at + 4)) {
    const string rest = html.substr(at + 4, 24);
    if (rest.compare(0, 8, "\"/files/") != 0 && rest.compare(0, 19, "\"data:image/svg+xml") != 0) {
      std::fprintf(stderr, "page fetches %s\n", rest.c_str());
      return false;
    }
  }
  for (size_t at = html.find("http"); at != string::npos; at = html.find("http", at + 4))
    if (html.compare(at, 26, "http://www.w3.org/2000/svg") != 0 && html.compare(at, 5, "http:") == 0) {
      std::fprintf(stderr, "page names %s\n", html.substr(at, 40).c_str());
      return false;
    }
  return html.find("https:") == string::npos;
}

void test_page_and_systems() {
  Rig r;
  webtransfer::set_enabled(true);
  string page = get(r.port(), "/files");
  CHECK_EQ(status_of(page), 200);
  CHECK(page.find("text/html") != string::npos);
  CHECK(page.find("/files/put") != string::npos && page.find("X-PIN") != string::npos);  // the page sends through /files/put (the old route stays for old clients)
  // self-contained: nothing is fetched from anywhere but the console itself
  string html = webtransfer::page_html();
  for (const char* bad : {"https://", "src=", "href=", "@import", "<link", "<iframe", "//cdn"})
    CHECK(html.find(bad) == string::npos);
  CHECK(only_local_urls(html));
  CHECK(html.find("<script>") != string::npos && html.find("<style>") != string::npos);
  CHECK(html.find("viewport") != string::npos);

  CHECK_EQ(status_of(get(r.port(), "/files/systems")), 403);        // no PIN
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "0000")), 403);  // wrong PIN
  string ok = get(r.port(), "/files/systems", kPin);
  CHECK_EQ(status_of(ok), 200);
  CHECK(ok.find("\"name\":\"gb\"") != string::npos && ok.find("\"fullname\":\"Game Boy\"") != string::npos);
  CHECK(ok.find(".GBC") != string::npos && ok.find("\"count\":7") != string::npos && ok.find(r.gb) != string::npos);
  CHECK_EQ(status_of(get(r.port(), "/files/other", kPin)), 404);
  // the PIN the console makes is 4 digits (the rig's fixed one is dropped: this is the last check of the test)
  webtransfer::reset_for_test("");
  string p = webtransfer::pin();
  CHECK_EQ(p.size(), size_t(4));
  for (char c : p) CHECK(c >= '0' && c <= '9');
}

// A system of es_systems.cfg with no games is still an upload target (the first game of a system comes from the phone); a count
// not known yet is null, not a huge number
void test_empty_systems() {
  Rig r;
  webtransfer::set_enabled(true);
  string snes = r.root + "/snes";
  ::mkdir(snes.c_str(), 0755);
  std::vector<System> loaded(1), also(2);
  loaded[0].name = "gb";
  loaded[0].fullname = "Game Boy";
  loaded[0].rom_dir = r.gb + "/";
  loaded[0].exts = {".gb"};
  loaded[0].count = kCountPending;
  also[0] = loaded[0];  // already listed: not twice
  also[1].name = "snes";
  also[1].fullname = "Super Nintendo";
  also[1].rom_dir = snes;
  also[1].exts = {".sfc"};
  webtransfer::publish(loaded, &also);
  string ok = get(r.port(), "/files/systems", kPin);
  CHECK_EQ(status_of(ok), 200);
  CHECK(ok.find("\"name\":\"snes\"") != string::npos);
  CHECK_EQ(ok.find("\"name\":\"gb\""), ok.rfind("\"name\":\"gb\""));
  CHECK(ok.find("\"count\":null") != string::npos && ok.find("4294967295") == string::npos && ok.find("18446744073709551615") == string::npos);
  CHECK(ok.find("\"count\":0") != string::npos);
  CHECK_EQ(upload_status(r.port(), "system=snes&name=First.sfc", "abc"), 200);
  CHECK_EQ(list(snes).size(), size_t(1));
  // a publish without them (the carousel's hot reload) keeps the empty systems read last time
  webtransfer::publish(loaded);
  ok = get(r.port(), "/files/systems", kPin);
  CHECK(ok.find("\"name\":\"snes\"") != string::npos);
  CHECK_EQ(ok.find("\"name\":\"gb\""), ok.rfind("\"name\":\"gb\""));
  CHECK_EQ(upload_status(r.port(), "system=snes&name=Second.sfc", "abc"), 200);
  // never read (the background read failed or has not finished): unknown system, as before
  webtransfer::reset_for_test(kPin);
  webtransfer::set_enabled(true);
  webtransfer::publish(loaded);
  CHECK_EQ(upload_status(r.port(), "system=snes&name=Third.sfc", "abc"), 400);
}

void test_pin_lockout() {
  Rig r;
  webtransfer::set_enabled(true);
  CHECK_EQ(upload_status(r.port(), "system=gb&name=a.gb", "abc", "1111"), 403);
  CHECK_EQ(upload_status(r.port(), "system=gb&name=a.gb", "abc", nullptr), 403);
  for (int i = 0; i < 3; ++i) CHECK_EQ(status_of(get(r.port(), "/files/systems", "2222")), 403);
  // five in a row: locked, even the right PIN waits
  CHECK_EQ(status_of(get(r.port(), "/files/systems", kPin)), 429);
  CHECK_EQ(upload_status(r.port(), "system=gb&name=a.gb", "abc"), 429);
  CHECK(list(r.gb).empty());
  webtransfer::reset_for_test(kPin);
  webtransfer::set_enabled(true);
  webtransfer::publish_info({[&] {
    webtransfer::SystemInfo si;
    si.name = "gb";
    si.rom_dir = r.gb;
    si.exts = {".gb"};
    return si;
  }()});
  CHECK_EQ(upload_status(r.port(), "system=gb&name=a.gb", "abc"), 200);
}

void test_names() {
  using webtransfer::ext_allowed;
  using webtransfer::valid_name;
  CHECK(valid_name("Pokemon Red (USA).gb"));
  CHECK(valid_name("a.b.gb"));
  CHECK(valid_name("ñandú.gb"));
  for (const char* bad : {"", ".hidden.gb", "..", "a/b.gb", "a\\b.gb", "x..gb", "a\x01.gb", "a\x7f.gb", "a\nb.gb", "/etc/passwd"})
    CHECK(!valid_name(bad));
  CHECK(!valid_name(string(201, 'a') + ".gb"));
  CHECK(ext_allowed("a.gb", {".gb"}));
  CHECK(ext_allowed("A.GB", {".gb"}));
  CHECK(ext_allowed("a.gbc", {".GBC"}));
  CHECK(!ext_allowed("a.exe", {".gb"}));
  CHECK(!ext_allowed("agb", {".gb"}));
  CHECK(!ext_allowed("a.", {".gb"}));
  CHECK(!ext_allowed("a.gb.part", {".gb"}));

  Rig r;
  webtransfer::set_enabled(true);
  const uint16_t pt = r.port();
  CHECK_EQ(upload_status(pt, "system=gb&name=..%2F..%2Fx.gb", "abc"), 400);
  CHECK_EQ(upload_status(pt, "system=gb&name=a%2Fb.gb", "abc"), 400);
  CHECK_EQ(upload_status(pt, "system=gb&name=a%5Cb.gb", "abc"), 400);
  CHECK_EQ(upload_status(pt, "system=gb&name=.hidden.gb", "abc"), 400);
  CHECK_EQ(upload_status(pt, "system=gb&name=a%01b.gb", "abc"), 400);
  CHECK_EQ(upload_status(pt, "system=gb&name=virus.exe", "abc"), 400);
  CHECK_EQ(upload_status(pt, "system=gb&name=noext", "abc"), 400);
  CHECK_EQ(upload_status(pt, "system=nes&name=a.gb", "abc"), 400);  // unknown system
  CHECK_EQ(upload_status(pt, "name=a.gb", "abc"), 400);
  CHECK_EQ(upload_status(pt, "system=gb&name=a.gb", ""), 400);        // empty file
  CHECK(list(r.gb).empty());
  CHECK(list(r.root) == (std::vector<string>{"gb", "shots"}));        // nothing escaped the ROM folder (shots: the rig's screenshots)
  CHECK_EQ(r.reloads.load(), 0);
}

void test_streamed_upload() {
  Rig r;
  webtransfer::set_enabled(true);
  const string data = pattern(700 * 1024 + 13);  // bigger than the 64 KB request cap of the other routes
  {
    int fd = connect_to(r.port());
    CHECK(fd >= 0);
    string head = upload_head("system=gb&name=Big%20Game%20(EU).gb", data.size());
    send_str(fd, head);
    size_t off = 0;
    while (off < data.size()) {  // in pieces, with pauses: it is read from the socket as it comes
      size_t n = std::min<size_t>(100 * 1024 + 7, data.size() - off);
      send_str(fd, data.substr(off, n));
      off += n;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    string resp = read_all(fd);
    ::close(fd);
    CHECK_EQ(status_of(resp), 200);
    CHECK(resp.find("\"bytes\":" + std::to_string(data.size())) != string::npos);
  }
  const string final_path = r.gb + "/Big Game (EU).gb";
  CHECK(slurp(final_path) == data);
  CHECK(list(r.gb) == std::vector<string>{"Big Game (EU).gb"});  // no .part left
  CHECK_EQ(r.reloads.load(), 1);

  // with "Expect: 100-continue" the data is only asked for after the checks
  {
    int fd = connect_to(r.port());
    string small = pattern(5000);
    send_str(fd, upload_head("system=gb&name=exp.gb", small.size(), kPin, true));
    char b[64] = {0};
    pollfd pf{fd, POLLIN, 0};
    CHECK(::poll(&pf, 1, 2000) > 0);
    ssize_t n = ::recv(fd, b, sizeof b - 1, 0);
    CHECK(n > 0 && string(b).find("100 Continue") != string::npos);
    send_str(fd, small);
    CHECK_EQ(status_of(read_all(fd)), 200);
    ::close(fd);
    CHECK(slurp(r.gb + "/exp.gb") == small);
  }
  // a refused upload (wrong PIN) is refused before any 100 Continue
  {
    int fd = connect_to(r.port());
    send_str(fd, upload_head("system=gb&name=no.gb", 5000, "9999", true));
    string resp = read_all(fd, 1500);
    ::close(fd);
    CHECK_EQ(status_of(resp), 403);
    CHECK(resp.find("100 Continue") == string::npos);
  }
}

void test_no_overwrite() {
  Rig r;
  webtransfer::set_enabled(true);
  CHECK_EQ(upload_status(r.port(), "system=gb&name=a.gb", "first"), 200);
  CHECK_EQ(upload_status(r.port(), "system=gb&name=a.gb", "second"), 409);
  CHECK_EQ(slurp(r.gb + "/a.gb"), string("first"));
  CHECK_EQ(upload_status(r.port(), "system=gb&name=a.gb&replace=0", "second"), 409);
  CHECK_EQ(slurp(r.gb + "/a.gb"), string("first"));
  CHECK_EQ(upload_status(r.port(), "system=gb&name=a.gb&replace=1", "second"), 200);
  CHECK_EQ(slurp(r.gb + "/a.gb"), string("second"));
  CHECK(list(r.gb) == std::vector<string>{"a.gb"});
  // replace never follows a symlink out of the folder
  ::symlink("/etc/hostname", (r.gb + "/link.gb").c_str());
  CHECK_EQ(upload_status(r.port(), "system=gb&name=link.gb&replace=1", "x"), 400);
  CHECK_EQ(upload_status(r.port(), "system=gb&name=link.gb", "x"), 409);
}

void test_space() {
  Rig r;
  webtransfer::set_enabled(true);
  r.use_fake = true;
  r.fake_free = 1000;
  string body = pattern(2000);
  CHECK_EQ(upload_status(r.port(), "system=gb&name=big.gb", body), 413);
  CHECK(list(r.gb).empty());  // nothing was created, not even a .part
  r.fake_free = 2000;
  CHECK_EQ(upload_status(r.port(), "system=gb&name=big.gb", body), 200);
  CHECK(slurp(r.gb + "/big.gb") == body);
}

void test_one_at_a_time_and_stalls() {
  http::ServerLimits lim;
  lim.stream_idle_ms = 400;
  Rig r(lim);
  webtransfer::set_enabled(true);
  const string data = pattern(100000);
  // A starts and pauses (but within the idle limit); B is told to wait; A can still finish
  int a = connect_to(r.port());
  send_str(a, upload_head("system=gb&name=a.gb", data.size()));
  send_str(a, data.substr(0, 1000));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  CHECK_EQ(upload_status(r.port(), "system=gb&name=b.gb", "bbb"), 409);
  CHECK(!exists(r.gb + "/b.gb"));
  send_str(a, data.substr(1000));
  CHECK_EQ(status_of(read_all(a)), 200);
  ::close(a);
  CHECK(slurp(r.gb + "/a.gb") == data);
  // the control routes are not blocked by a slow upload
  int c = connect_to(r.port());
  send_str(c, upload_head("system=gb&name=c.gb", data.size()));
  send_str(c, data.substr(0, 10));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK_EQ(status_of(get(r.port(), "/files")), 200);
  // C went silent: the idle limit answers 408 and removes the partial file
  string resp = read_all(c, 3000);
  ::close(c);
  CHECK_EQ(status_of(resp), 408);
  CHECK(!exists(r.gb + "/c.gb") && !exists(r.gb + "/c.gb.part"));
  // a client that leaves half way: nothing is left behind and the next upload works
  int d = connect_to(r.port());
  send_str(d, upload_head("system=gb&name=d.gb", data.size()));
  send_str(d, data.substr(0, 5000));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ::close(d);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(!exists(r.gb + "/d.gb") && !exists(r.gb + "/d.gb.part"));
  CHECK_EQ(upload_status(r.port(), "system=gb&name=e.gb", "eee"), 200);
  CHECK(list(r.gb) == (std::vector<string>{"a.gb", "e.gb"}));
}

void test_stop_with_upload_running() {
  Rig r;
  webtransfer::set_enabled(true);
  int a = connect_to(r.port());
  send_str(a, upload_head("system=gb&name=s.gb", 100000));
  send_str(a, "abc");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  auto t0 = std::chrono::steady_clock::now();
  r.srv.stop();  // must not wait for the client
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  ::close(a);
  CHECK(ms < 2000);
  CHECK(!exists(r.gb + "/s.gb") && !exists(r.gb + "/s.gb.part"));
}

int g_on_enable = 0;

void test_listener() {
  webtransfer::reset_for_test(kPin);
  webtransfer::set_listen_port(0);
  webtransfer::Env env;
  env.on_enable = [] { ++g_on_enable; };
  webtransfer::set_env(env);
  CHECK(!webtransfer::listening());
  // the control server (loopback, no stream handler) never serves /files
  {
    http::Server ctl;
    string err;
    CHECK(ctl.start(0, [](const http::Request&) { return http::Response(); }, err));
    CHECK_EQ(status_of(talk(ctl.port(), upload_head("system=gb&name=a.gb", 3) + "abc")), 404);
  }
  webtransfer::publish_info({[] {
    webtransfer::SystemInfo si;
    si.name = "gb";
    si.rom_dir = "/nonexistent";
    si.exts = {".gb"};
    return si;
  }()});
  CHECK(webtransfer::apply(true));
  CHECK(webtransfer::listening() && webtransfer::enabled());
  CHECK_EQ(g_on_enable, 1);  // told once the listener runs: what only the transfer needs is prepared then
  const uint16_t pt = webtransfer::listen_port();
  CHECK(pt != 0);
  CHECK_EQ(status_of(get(pt, "/files")), 200);
  // the control routes do not exist on it
  for (const char* p : {"/isIdle", "/runningGame", "/quit", "/shutdown", "/restart", "/emukill", "/reloadgames", "/notify", "/files/other"})
    CHECK_EQ(status_of(get(pt, p)), 404);
  CHECK_EQ(status_of(talk(pt, "POST /launch HTTP/1.1\r\nContent-Length: 4\r\n\r\n/rom")), 404);
  // a second start while it runs is harmless; turning off closes the port
  CHECK(webtransfer::start_listener());
  CHECK(webtransfer::apply(false));
  CHECK(!webtransfer::listening() && !webtransfer::enabled());
  CHECK(connect_to(pt) < 0);
  // port busy: apply(true) fails and says why
  int busy = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  CHECK(::bind(busy, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0);
  ::listen(busy, 1);
  socklen_t sl = sizeof sa;
  ::getsockname(busy, reinterpret_cast<sockaddr*>(&sa), &sl);
  webtransfer::set_listen_port(ntohs(sa.sin_port));
  CHECK(!webtransfer::apply(true));
  CHECK_EQ(g_on_enable, 1);  // it could not listen: nothing to prepare
  CHECK(!webtransfer::listening() && !webtransfer::listen_error().empty());
  ::close(busy);
  webtransfer::apply(false);
  webtransfer::set_listen_port(webtransfer::kWebPort);
}

void test_control_api_is_local_only() {
  osapi::Ops ops;
  http::Request req;
  req.method = "GET";
  req.path = "/isIdle";
  CHECK_EQ(osapi::route(req, ops).status, 200);  // from the console itself
  req.local = false;
  CHECK_EQ(osapi::route(req, ops).status, 403);  // from the network
  req.path = "/shutdown";
  CHECK_EQ(osapi::route(req, ops).status, 403);
  req.path = "/nothing";
  CHECK_EQ(osapi::route(req, ops).status, 404);
}

void test_parser() {
  http::Request q;
  using http::Parse;
  // streamed route: headers are enough, the body is not buffered or capped
  CHECK(http::parse_request("POST /files/upload?system=gb&name=a.gb HTTP/1.1\r\nContent-Length: 1500000000\r\nX-PIN: 1\r\n\r\nabc", q) == Parse::Stream);
  CHECK_EQ(q.content_length, uint64_t(1500000000));
  CHECK_EQ(q.body, string("abc"));
  CHECK_EQ(q.path, string("/files/upload"));
  string pin;
  CHECK(http::header_value(q, "x-pin", pin) && pin == "1");
  // every other route keeps the cap
  CHECK(http::parse_request("POST /launch HTTP/1.1\r\nContent-Length: 70000\r\n\r\n", q) == Parse::TooLarge);
  CHECK(http::parse_request("GET /files/upload HTTP/1.1\r\nContent-Length: 70000\r\n\r\n", q) == Parse::TooLarge);
}

// ---- phone remote ------------------------------------------------------------------------------------------------------

string body_of(const string& resp) {
  size_t i = resp.find("\r\n\r\n");
  return i == string::npos ? string() : resp.substr(i + 4);
}

string post(uint16_t port, const string& path, const char* pin = nullptr, const string& body = "") {
  string h = "POST " + path + " HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  return talk(port, h + "\r\n" + body);
}

void write_file(const string& p, const string& data) {
  if (FILE* f = std::fopen(p.c_str(), "wb")) {
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
  }
}

// Systems "gb" (loaded: Alpha, Beta Quest, Gamma with pictures in a gamelist) and "nes" (not loaded).
struct Games {
  string alpha, beta, gamma, outside, id_alpha, id_beta, id_gamma, id_out;
};

Games publish_games(Rig& r) {
  Games g;
  g.alpha = r.gb + "/Alpha.gb";
  g.beta = r.gb + "/Beta Quest.gb";
  g.gamma = r.gb + "/Gamma.gb";
  g.outside = r.gb + "/Outside.gb";
  ::mkdir((r.gb + "/media").c_str(), 0755);
  write_file(r.gb + "/media/gamma.png", "\x89PNG-gamma-picture");
  write_file(r.root + "/secret.png", "\x89PNG-SECRET");
  ::unlink((r.gb + "/media/link.png").c_str());
  CHECK(::symlink((r.root + "/secret.png").c_str(), (r.gb + "/media/link.png").c_str()) == 0);
  string xml = "<gameList>\n";
  auto entry = [&](const string& rel, const string& img) {
    xml += "<game>\n<path>" + rel + "</path>\n<image>" + img + "</image>\n</game>\n";
  };
  std::vector<std::pair<uint32_t, uint32_t>> spans;
  for (auto& e : std::vector<std::pair<string, string>>{{"./Gamma.gb", "./media/gamma.png"}, {"./Outside.gb", "../secret.png"}, {"./Beta Quest.gb", "./media/link.png"}}) {
    size_t b = xml.size();
    entry(e.first, e.second);
    spans.push_back({uint32_t(b), uint32_t(xml.size())});
  }
  xml += "</gameList>\n";
  write_file(r.gb + "/gamelist.xml", xml);

  webtransfer::SystemInfo gb;
  gb.name = "gb";
  gb.fullname = "Game Boy";
  gb.rom_dir = r.gb;
  gb.exts = {".gb"};
  gb.gamelist = r.gb + "/gamelist.xml";
  gb.loaded = true;
  webtransfer::GameInfo a{"Alpha", g.alpha, GameMeta()}, b{"Beta Quest", g.beta, GameMeta()}, c{"Gamma", g.gamma, GameMeta()}, o{"Outside", g.outside, GameMeta()};
  c.meta.span_begin = spans[0].first;
  c.meta.span_end = spans[0].second;
  o.meta.span_begin = spans[1].first;
  o.meta.span_end = spans[1].second;
  b.meta.span_begin = spans[2].first;
  b.meta.span_end = spans[2].second;
  gb.games = {a, b, c, o};
  gb.count = 4;
  webtransfer::SystemInfo nes;
  nes.name = "nes";
  nes.fullname = "NES";
  nes.rom_dir = r.root + "/nes";
  nes.exts = {".nes"};
  nes.count = 9;
  webtransfer::publish_info({gb, nes});
  g.id_alpha = webtransfer::game_id(g.alpha);
  g.id_beta = webtransfer::game_id(g.beta);
  g.id_gamma = webtransfer::game_id(g.gamma);
  g.id_out = webtransfer::game_id(g.outside);
  return g;
}

void test_remote_off_and_pin() {
  Rig r;  // off
  Games g = publish_games(r);
  const string id = "?id=" + g.id_alpha;
  for (const char* p : {"/files/games", "/files/shots", "/files/shot?id=a.png", "/files/cover?id=0000000000000000"}) {
    CHECK_EQ(status_of(get(r.port(), p, kPin)), 404);
    CHECK_EQ(status_of(get(r.port(), p)), 404);
  }
  CHECK_EQ(status_of(post(r.port(), "/files/launch" + id, kPin)), 404);
  CHECK_EQ(status_of(post(r.port(), "/files/quit", kPin)), 404);
  CHECK(r.launched.empty());

  webtransfer::set_enabled(true);
  const char* gets[] = {"/files/games", "/files/shots", "/files/shot?id=a.png", "/files/cover?id=0000000000000000"};
  for (const char* p : gets) {
    CHECK_EQ(status_of(get(r.port(), p)), 403);
    CHECK_EQ(status_of(get(r.port(), p, "0000")), 403);
    CHECK_EQ(status_of(get(r.port(), "/files/systems", kPin)), 200);  // a right PIN clears the wrong-PIN count
  }
  CHECK_EQ(status_of(post(r.port(), "/files/launch" + id)), 403);
  CHECK_EQ(status_of(get(r.port(), "/files/systems", kPin)), 200);
  CHECK_EQ(status_of(post(r.port(), "/files/quit")), 403);
  CHECK(r.launched.empty());
  // wrong verbs
  CHECK_EQ(status_of(get(r.port(), "/files/launch" + id, kPin)), 405);
  CHECK_EQ(status_of(post(r.port(), "/files/games", kPin)), 405);
  CHECK_EQ(status_of(get(r.port(), "/files/quit", kPin)), 405);
  CHECK(r.launched.empty());
}

void test_remote_games() {
  Rig r;
  webtransfer::set_enabled(true);
  Games g = publish_games(r);
  string all = get(r.port(), "/files/games", kPin);
  CHECK_EQ(status_of(all), 200);
  string b = body_of(all);
  CHECK(b.find("\"name\":\"Alpha\"") != string::npos && b.find("\"name\":\"Gamma\"") != string::npos);
  CHECK(b.find(g.id_alpha) != string::npos && b.find("\"total\":4") != string::npos);
  // never an absolute path, only ids
  CHECK(b.find(r.root) == string::npos && b.find("/tmp/") == string::npos && b.find(".gb\"") == string::npos);
  // search is case-insensitive and filters
  string q = body_of(get(r.port(), "/files/games?system=gb&q=bEtA%20q", kPin));
  CHECK(q.find("Beta Quest") != string::npos && q.find("Alpha") == string::npos && q.find("\"total\":1") != string::npos);
  CHECK(body_of(get(r.port(), "/files/games?q=nothing-like-this", kPin)).find("\"total\":0") != string::npos);
  // the cover flag follows the gamelist
  CHECK(body_of(get(r.port(), "/files/games?q=gamma", kPin)).find("\"cover\":true") != string::npos);
  CHECK(body_of(get(r.port(), "/files/games?q=alpha", kPin)).find("\"cover\":false") != string::npos);
  // paging
  string pg = body_of(get(r.port(), "/files/games?system=gb&limit=2&offset=1", kPin));
  CHECK(pg.find("\"more\":true") != string::npos && pg.find("Beta Quest") != string::npos && pg.find("Alpha") == string::npos);
  // unknown system: 404; a system not read yet asks the main thread to open it and says so
  CHECK_EQ(status_of(get(r.port(), "/files/games?system=zzz", kPin)), 404);
  string nes = body_of(get(r.port(), "/files/games?system=nes", kPin));
  CHECK(nes.find("\"loaded\":false") != string::npos && nes.find("\"games\":[]") != string::npos);
  {
    std::lock_guard<std::mutex> lk(r.lm);
    CHECK(!r.opened.empty());
    for (const string& n : r.opened) CHECK(n == "nes");  // searches over every system ask for "nes" too, never for ""
    CHECK(r.opened.back() == "nes");
  }
}

void test_remote_launch() {
  Rig r;
  webtransfer::set_enabled(true);
  Games g = publish_games(r);
  // ids that are not ids: paths, traversal, wrong length, upper case, empty
  for (const string& bad : {g.alpha, string("/etc/passwd"), string("../../etc/passwd"), string("00"), string("ABCDEF0123456789"), string("zzzzzzzzzzzzzzzz"), string("")}) {
    string enc;
    for (unsigned char ch : bad) {
      char t[4];
      std::snprintf(t, sizeof t, "%%%02X", ch);
      enc += t;
    }
    CHECK_EQ(status_of(post(r.port(), "/files/launch?id=" + enc, kPin)), 400);
  }
  CHECK_EQ(status_of(post(r.port(), "/files/launch", kPin, g.alpha)), 400);  // a path in the body is no id either
  CHECK_EQ(status_of(post(r.port(), "/files/launch?id=0123456789abcdef", kPin)), 404);  // looks right, not in the snapshot
  CHECK(r.launched.empty());
  // a game of the snapshot: the id is mapped to its path and launched once
  string ok = post(r.port(), "/files/launch?id=" + g.id_beta, kPin);
  CHECK_EQ(status_of(ok), 200);
  CHECK(body_of(ok).find("Beta Quest") != string::npos && body_of(ok).find(r.root) == string::npos);
  {
    std::lock_guard<std::mutex> lk(r.lm);
    CHECK_EQ(r.launched.size(), size_t(1));
    CHECK(r.launched[0] == g.beta);
  }
  // a double tap right after is refused and does not launch again
  CHECK_EQ(status_of(post(r.port(), "/files/launch?id=" + g.id_beta, kPin)), 409);
  CHECK_EQ(r.launched.size(), size_t(1));
  // the id may also come in the body
  webtransfer::reset_for_test(kPin);
  webtransfer::set_enabled(true);
  Games g2 = publish_games(r);
  CHECK_EQ(status_of(post(r.port(), "/files/launch", kPin, g2.id_alpha + "\n")), 200);
  CHECK_EQ(r.launched.size(), size_t(2));
  CHECK(r.launched[1] == g2.alpha);
  // a game is running: 409, nothing launched
  webtransfer::reset_for_test(kPin);
  webtransfer::set_enabled(true);
  Games g3 = publish_games(r);
  oscontract::paths().kill_file = r.root + "/kill";  // no real kill target on the machine running the test (and no startup: commands are dry)
  oscontract::game_started(g3.alpha, "Alpha");
  CHECK_EQ(status_of(post(r.port(), "/files/launch?id=" + g3.id_beta, kPin)), 409);
  CHECK_EQ(r.launched.size(), size_t(2));
  // quit: answers 200 whatever the kill target is (nothing is spawned by the test's own state)
  string qr = post(r.port(), "/files/quit", kPin);
  CHECK_EQ(status_of(qr), 200);
  CHECK(body_of(qr).find("\"killed\"") != string::npos);
  oscontract::game_ended();
  CHECK_EQ(status_of(post(r.port(), "/files/quit", kPin)), 200);
  CHECK(body_of(post(r.port(), "/files/quit", kPin)).find("\"killed\":false") != string::npos);
}

void test_remote_shots() {
  Rig r;
  webtransfer::set_enabled(true);
  publish_games(r);
  write_file(r.shots + "/old.png", "\x89PNG-old");
  write_file(r.shots + "/new.jpg", "JPEG-new");
  write_file(r.shots + "/.hidden.png", "x");
  write_file(r.shots + "/notes.txt", "not an image");
  ::mkdir((r.shots + "/dir.png").c_str(), 0755);
  write_file(r.root + "/outside.png", "\x89PNG-OUTSIDE");
  CHECK(::symlink((r.root + "/outside.png").c_str(), (r.shots + "/link.png").c_str()) == 0);
  struct timespec ts[2] = {{1000000000, 0}, {1000000000, 0}};
  ::utimensat(AT_FDCWD, (r.shots + "/old.png").c_str(), ts, 0);
  ts[0].tv_sec = ts[1].tv_sec = 1700000000;
  ::utimensat(AT_FDCWD, (r.shots + "/new.jpg").c_str(), ts, 0);

  string l = body_of(get(r.port(), "/files/shots", kPin));
  size_t pn = l.find("new.jpg"), po = l.find("old.png");
  CHECK(pn != string::npos && po != string::npos && pn < po);  // newest first
  for (const char* no : {".hidden", "notes.txt", "dir.png", "link.png", "/tmp/"}) CHECK(l.find(no) == string::npos);

  string one = get(r.port(), "/files/shot?id=old.png", kPin);
  CHECK_EQ(status_of(one), 200);
  CHECK(one.find("image/png") != string::npos && body_of(one) == "\x89PNG-old");
  CHECK(get(r.port(), "/files/shot?id=new.jpg", kPin).find("image/jpeg") != string::npos);
  // traversal and everything that is not a plain image name of that folder
  for (const char* bad : {"../outside.png", "..%2Foutside.png", "%2e%2e%2foutside.png", "%2Fetc%2Fpasswd", "/etc/passwd", "sub%2Fold.png", "..", ".hidden.png", "old.png%00.txt", "notes.txt", "dir.png", "link.png", "missing.png", ""}) {
    int st = status_of(get(r.port(), string("/files/shot?id=") + bad, kPin));
    CHECK(st == 400 || st == 404);
    CHECK(body_of(get(r.port(), string("/files/shot?id=") + bad, kPin)).find("OUTSIDE") == string::npos);
  }
  CHECK_EQ(status_of(get(r.port(), "/files/shot", kPin)), 400);
}

void test_remote_cover() {
  Rig r;
  webtransfer::set_enabled(true);
  Games g = publish_games(r);
  string c = get(r.port(), "/files/cover?id=" + g.id_gamma, kPin);
  CHECK_EQ(status_of(c), 200);
  CHECK(c.find("image/png") != string::npos && body_of(c) == "\x89PNG-gamma-picture");
  // no picture in the gamelist, picture outside the ROM folder (".."), symlink pointing outside: all 404 and never the file
  for (const string& id : {g.id_alpha, g.id_out, g.id_beta}) {
    string x = get(r.port(), "/files/cover?id=" + id, kPin);
    CHECK_EQ(status_of(x), 404);
    CHECK(x.find("SECRET") == string::npos);
  }
  // only ids of the snapshot, never paths
  CHECK_EQ(status_of(get(r.port(), "/files/cover?id=%2Fetc%2Fpasswd", kPin)), 404);
  CHECK_EQ(status_of(get(r.port(), "/files/cover?id=../secret.png", kPin)), 404);
  CHECK_EQ(status_of(get(r.port(), "/files/cover", kPin)), 404);
}

// A feature module registered like the real ones (webcore.hpp): a tab, a guarded route, a public one and a streamed upload.
std::atomic<uint64_t> g_mod_bytes{0};
ZF_REGISTER_WEB_MODULE([] {
  webtransfer::WebModule m;
  m.name = "testmod";
  m.order = 50;
  m.owns = [](std::string_view p) { return p == "/files/tm" || p == "/files/tm-pub"; };
  m.public_path = [](std::string_view p) { return p == "/files/tm-pub"; };
  m.route = [](const http::Request& q, const webtransfer::Env&) { return webtransfer::reply(200, q.path == "/files/tm" ? "{\"tm\":1}" : "{\"pub\":1}"); };
  m.stream_owns = [](std::string_view meth, std::string_view p) { return meth == "POST" && p == "/files/tm-up"; };
  m.stream = [](const http::Request&, http::BodyStream& b, const webtransfer::Env&) {
    b.accept();
    char buf[4096];
    uint64_t n = 0;
    for (;;) {
      ptrdiff_t k = b.read(buf, sizeof buf);
      if (k <= 0) break;
      n += uint64_t(k);
    }
    g_mod_bytes = n;
    return webtransfer::reply(200, "{}");
  };
  m.tab_id = "tm";
  m.tab_label = "T&M";
  m.section = "<p id=\"tmsec\"></p>";
  m.css = "#tmsec{color:red}";
  m.html = "<div id=\"tmdlg\"></div>";
  m.js = "Z.ontab.tm=function(){Z.say(\"tm\")};";
  return m;
});

void test_modules() {
  string html = webtransfer::page_html();
  CHECK(html.find("<button type=\"button\" id=\"t-tm\">T&amp;M</button>") != string::npos);
  CHECK(html.find("<section id=\"p-tm\" hidden>\n<p id=\"tmsec\"></p>\n</section>") != string::npos);
  CHECK(html.find("#tmsec{color:red}") != string::npos);  // other modules' css may follow
  // module html goes before the message line (other modules' html may sit between)
  CHECK(html.find("<div id=\"tmdlg\"></div>") != string::npos && html.find("<div id=\"tmdlg\"></div>") < html.find("<p id=\"msg\""));
  CHECK(html.find("var tabs=[\"up\",\"ga\",\"sh\"") != string::npos && html.find("\"tm\"") != string::npos);  // other modules add their tabs
  CHECK(html.find("(function(Z){\nZ.ontab.tm=function(){Z.say(\"tm\")};\n})(Z);") != string::npos);
  CHECK(html.find("id=\"t-tm\"") > html.find("id=\"t-sh\""));
  CHECK(html.find("id=\"p-tm\"") > html.find("id=\"p-sh\""));
  CHECK(html.find("/*@") == string::npos);  // every marker filled
  Rig r;
  CHECK_EQ(status_of(get(r.port(), "/files/tm", kPin)), 404);  // switched off: as if it did not exist
  webtransfer::set_enabled(true);
  CHECK_EQ(status_of(get(r.port(), "/files/tm")), 403);
  CHECK_EQ(status_of(get(r.port(), "/files/tm", "0000")), 403);
  string ok = get(r.port(), "/files/tm", kPin);
  CHECK_EQ(status_of(ok), 200);
  CHECK(ok.find("{\"tm\":1}") != string::npos);
  CHECK_EQ(status_of(get(r.port(), "/files/tm-pub")), 200);  // public: no PIN
  CHECK_EQ(status_of(get(r.port(), "/files/nobody")), 404);  // owned by nobody: 404, PIN or not
  const string body = pattern(200000);
  const string head = "POST /files/tm-up HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) + "\r\nX-PIN: " + kPin + "\r\n\r\n";
  CHECK_EQ(status_of(talk(r.port(), head + body)), 200);
  CHECK_EQ(g_mod_bytes.load(), uint64_t(body.size()));
  CHECK_EQ(status_of(talk(r.port(), "POST /files/tm-up HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc")), 403);
  CHECK_EQ(upload_status(r.port(), "system=gb&name=m.gb", "abc"), 200);  // the core upload streams as before
  webtransfer::set_enabled(false);
  CHECK_EQ(status_of(talk(r.port(), head + body)), 404);
}

void test_remote_page() {
  string html = webtransfer::page_html();
  if (const char* d = std::getenv("ZF_PAGE_DUMP")) write_file(d, html);  // node --check of the script (by hand)
  for (const char* want : {"Upload", "Games", "Screenshots", "Close game", "/files/games", "/files/launch", "/files/quit", "/files/shots", "/files/shot?id=", "/files/cover", "type=\"search\""})
    CHECK(html.find(want) != string::npos);
  for (const char* bad : {"https://", "src=", "href=", "@import", "<link", "<iframe", "//cdn", "<img src", "window.open", "eval("})
    CHECK(html.find(bad) == string::npos);
  CHECK(only_local_urls(html));
  {  // the page answers with a policy that lets it load nothing from outside and keeps it out of frames
    Rig pr;
    webtransfer::set_enabled(true);
    const string a = get(pr.port(), "/files");
    CHECK_EQ(status_of(a), 200);
    CHECK(a.find("Content-Security-Policy: default-src 'none'") != string::npos && a.find("connect-src 'self'") != string::npos);
    CHECK(a.find("frame-ancestors 'none'") != string::npos && a.find("X-Frame-Options: DENY\r\n") != string::npos);
    CHECK(a.find("Referrer-Policy: no-referrer\r\n") != string::npos && a.find("X-Content-Type-Options: nosniff\r\n") != string::npos);
    webtransfer::set_enabled(false);
  }
  // the system of an upload is fixed when Upload is pressed: the slices and the status asks never read the box again (changing it
  // half-way sent the next slices of the file to another system)
  CHECK(html.find("enc($(\"sys\").value)") == string::npos);
  CHECK(html.find("sysn=$(\"sys\").value") != string::npos && html.find("it.s=sysn") != string::npos);
  CHECK(html.find("enc(it.s)") != string::npos);
  // Enter in the PIN box is "Continue"
  CHECK(html.find("$(\"pin\").onkeydown") != string::npos);
  // no way to delete a single game outright from the page or the routes: games go to the trash (webmanage), which can be restored;
  // only "Empty trash" removes anything for good, and it says so
  CHECK(html.find("/files/delete") == string::npos && html.find("\"DELETE\"") == string::npos);
  CHECK(html.find("/files/game/trash") != string::npos && html.find("This cannot be undone") != string::npos);
  Rig r;
  webtransfer::set_enabled(true);
  CHECK_EQ(status_of(post(r.port(), "/files/delete", kPin)), 404);
  CHECK_EQ(status_of(get(r.port(), "/files/delete?id=a", kPin)), 404);
}

}  // namespace

int main() {
  test_parser();
  test_names();
  test_off();
  test_page_and_systems();
  test_modules();
  test_empty_systems();
  test_pin_lockout();
  test_streamed_upload();
  test_no_overwrite();
  test_space();
  test_one_at_a_time_and_stalls();
  test_stop_with_upload_running();
  test_control_api_is_local_only();
  test_listener();
  test_remote_off_and_pin();
  test_remote_games();
  test_remote_launch();
  test_remote_shots();
  test_remote_cover();
  test_remote_page();
  return zt::done();
}
