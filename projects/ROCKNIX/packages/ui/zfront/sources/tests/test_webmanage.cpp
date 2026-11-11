// webmanage: trash / restore / empty, rename and change the picture of a game from the phone. The real httpd server on 127.0.0.1
// with the real stream worker, ROM folders and gamelists in a temp dir, an App whose Ctx holds the systems (`on_main` runs
// the closure at once, then the test drains the jobs). Nothing outside the temp folder is touched.
#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <functional>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "gamemeta.hpp"
#include "httpd.hpp"
#include "oscontract.hpp"
#include "services.hpp"
#include "menu_game.hpp"
#include "registry.hpp"
#include "status.hpp"
#include "webcore.hpp"
#include "webtransfer.hpp"

using namespace zf;
using std::string;

namespace {

const char* kPin = "4821";

// ---- tiny HTTP client --------------------------------------------------------------------------------------------------
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

string talk(uint16_t port, const string& raw) {
  int fd = connect_to(port);
  if (fd < 0) return "";
  size_t off = 0;
  while (off < raw.size()) {
    ssize_t n = ::send(fd, raw.data() + off, raw.size() - off, MSG_NOSIGNAL);
    if (n <= 0) break;
    off += size_t(n);
  }
  string out;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
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
  ::close(fd);
  return out;
}

int status_of(const string& r) {
  if (r.compare(0, 9, "HTTP/1.1 ") != 0 || r.size() < 12) return 0;
  return std::atoi(r.c_str() + 9);
}

string body_of(const string& r) {
  size_t at = r.find("\r\n\r\n");
  return at == string::npos ? string() : r.substr(at + 4);
}

string call(uint16_t port, const string& method, const string& path, const string& body = "", const char* pin = kPin) {
  string h = method + " " + path + " HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  return talk(port, h + "\r\n" + body);
}

string pct(const string& s) {
  string o;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.') o += char(c);
    else {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", c);
      o += b;
    }
  }
  return o;
}

// ---- files -------------------------------------------------------------------------------------------------------------
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
void spit(const string& p, const string& s) {
  if (FILE* f = std::fopen(p.c_str(), "wb")) {
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
  }
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
bool has(const string& hay, const string& needle) { return hay.find(needle) != string::npos; }

const string kPng = string("\x89PNG\r\n\x1a\n", 8) + "0123456789abcdef-the-rest-of-a-picture";
const string kJpg = string("\xff\xd8\xff\xe0", 4) + "0123456789abcdef-the-rest-of-a-picture";
const string kWebp = string("RIFF\x24\0\0\0WEBPVP8 ", 16) + "0123456789abcdef";

// ---- the fixture -------------------------------------------------------------------------------------------------------
struct Struct {
  string root, dir, xml, states;
  std::unique_ptr<App> app;
  std::atomic<int> reloads{0};
  http::Server srv;
  webtransfer::Env env;

  struct Root : Screen {
    Transition handle(Action, Ctx&) override { return Transition::none(); }
    void draw(Ctx&, std::vector<Op>&) override {}
  };

  void settle() {
    Ctx& cx = app->cx;
    for (int i = 0; i < 2000 && !cx.sv->jobs.idle(); ++i) {
      cx.sv->jobs.drain();
      usleep(1000);
    }
    cx.sv->jobs.drain();
  }
  void publish() { webtransfer::publish(app->cx.systems); }

  Struct() {
    char t[] = "/tmp/zf-webmanage-XXXXXX";
    root = ::mkdtemp(t);
    dir = root + "/roms/gb";
    states = root + "/roms/savestates/gb";
    string cmd = "mkdir -p " + dir + "/images " + dir + "/sub " + states;
    if (std::system(cmd.c_str())) {
    }
    xml = dir + "/gamelist.xml";
    ::setenv("ZF_MANAGE_STATES", (root + "/roms/savestates").c_str(), 1);
    spit(dir + "/Foo.gb", "foo rom");
    spit(dir + "/Bar.gb", "bar rom");
    spit(dir + "/sub/Baz.gb", "baz rom");  // no gamelist entry
    spit(dir + "/Foo.srm", "foo save");
    spit(dir + "/images/Foo-image.png", kPng);
    spit(dir + "/images/Other.png", kPng);
    spit(states + "/Foo.state", "s0");
    spit(states + "/Foo.state1", "s1");
    spit(states + "/Foo.state.auto", "sa");
    spit(states + "/Foo.state.png", "sp");
    spit(states + "/Foobar.state", "not ours");
    spit(xml,
         "<?xml version=\"1.0\"?>\n<gameList>\n"
         "\t<game>\n\t\t<path>./Foo.gb</path>\n\t\t<name>Foo Game</name>\n\t\t<desc>About foo &amp; more</desc>\n\t\t<image>./images/Foo-image.png</image>\n"
         "\t\t<favorite>true</favorite>\n\t\t<playcount>3</playcount>\n\t</game>\n"
         "\t<game>\n\t\t<path>./Bar.gb</path>\n\t\t<name>Bar Game</name>\n\t</game>\n"
         "</gameList>\n");
    Ctx cx;
    cx.w = cx.h = 720;
    System s;
    s.name = "gb";
    s.fullname = "Game Boy";
    s.rom_dir = dir;
    s.gamelist = xml;
    s.exts = {".gb"};
    cx.systems.push_back(std::move(s));
    cx.open_system(0);
    app = std::make_unique<App>(std::move(cx), std::make_unique<Root>(), make_status_source("/nonexistent"));
    webtransfer::reset_for_test(kPin);
    publish();
    // the console's reload reads the game folders again: the games in memory follow the disk (a restored game comes back)
    env.reload = [this] {
      ++reloads;
      Ctx c2;
      System s2;
      s2.name = "gb";
      s2.rom_dir = dir;
      s2.gamelist = xml;
      s2.exts = {".gb"};
      c2.systems.push_back(std::move(s2));
      c2.open_system(0);
      app->cx.systems[0].games = std::move(c2.systems[0].games);
      publish();
    };
    env.refresh = [this] { publish(); };
    env.on_main = [this](std::function<void(App&)> f) { f(*app); };
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
        err, "127.0.0.1");
    CHECK(ok);
    webtransfer::set_enabled(true);
  }
  ~Struct() {
    srv.stop();
    webtransfer::set_enabled(false);
    ::unsetenv("ZF_MANAGE_STATES");
    string cmd = "rm -rf '" + root + "'";
    if (std::system(cmd.c_str())) {
    }
  }
  uint16_t port() const { return srv.port(); }
  string id(const string& file) const { return webtransfer::game_id(dir + "/" + file); }
  string post(const string& path, const string& body = "") { return call(port(), "POST", path, body); }
  string get(const string& path) { return call(port(), "GET", path); }
  /// The .trash item folders (names).
  std::vector<string> items() { return list(dir + "/.trash"); }
  string item_id() {
    auto v = items();
    return v.empty() ? string() : "gb|" + v[0];
  }
  // the games the loader sees on disk (a fresh read, like a rescan)
  std::vector<string> scanned() {
    Ctx cx;
    System s;
    s.name = "gb";
    s.rom_dir = dir;
    s.gamelist = xml;
    s.exts = {".gb"};
    cx.systems.push_back(std::move(s));
    cx.open_system(0);
    std::vector<string> v;
    for (const Game& g : cx.systems[0].games) v.push_back(g.path.substr(dir.size() + 1));
    std::sort(v.begin(), v.end());
    return v;
  }
  std::vector<string> memory() {
    std::vector<string> v;
    for (const Game& g : app->cx.systems[0].games) v.push_back(g.path.substr(dir.size() + 1));
    std::sort(v.begin(), v.end());
    return v;
  }
  const Game* in_memory(const string& file) {
    for (const Game& g : app->cx.systems[0].games)
      if (g.path == dir + "/" + file) return &g;
    return nullptr;
  }
};

// ---- tests -------------------------------------------------------------------------------------------------------------
void test_off_and_auth() {
  Struct f;
  webtransfer::set_enabled(false);
  for (const char* p : {"/files/game/trash?id=x", "/files/game/rename?id=x&name=a.gb", "/files/game/picture?id=x", "/files/trash/restore?item=x",
                        "/files/trash/empty?confirm=1"})
    CHECK_EQ(status_of(f.post(p)), 404);
  CHECK_EQ(status_of(f.get("/files/trash")), 404);
  CHECK_EQ(status_of(f.get("/files/game/info?id=x")), 404);
  CHECK(exists(f.dir + "/Foo.gb") && f.items().empty());
  webtransfer::set_enabled(true);
  const string tq = "/files/game/trash?id=" + f.id("Foo.gb");
  CHECK_EQ(status_of(call(f.port(), "POST", tq, "", nullptr)), 403);
  CHECK_EQ(status_of(call(f.port(), "POST", tq, "", "0000")), 403);
  CHECK_EQ(status_of(call(f.port(), "GET", "/files/trash", "", nullptr)), 403);
  CHECK_EQ(status_of(call(f.port(), "POST", "/files/game/picture?id=" + f.id("Foo.gb"), kPng, nullptr)), 403);
  CHECK(exists(f.dir + "/Foo.gb") && f.items().empty());
  webtransfer::reset_for_test(kPin);
  webtransfer::set_enabled(true);
  f.publish();
  CHECK_EQ(status_of(f.get("/files/trash")), 200);
  CHECK_EQ(status_of(f.post("/files/trash")), 405);  // wrong method
  CHECK_EQ(status_of(f.get("/files/game/trash?id=" + f.id("Foo.gb"))), 405);
}

void test_unknown() {
  Struct f;
  CHECK_EQ(status_of(f.post("/files/game/trash")), 404);
  CHECK_EQ(status_of(f.get("/files/game/info?id=nope")), 404);
  CHECK_EQ(status_of(f.post("/files/game/trash?id=0123456789abcdef")), 404);
  CHECK_EQ(status_of(f.post("/files/game/rename?id=0123456789abcdef&name=a.gb")), 404);
  CHECK_EQ(status_of(f.post("/files/game/picture?id=0123456789abcdef", kPng)), 404);
  CHECK_EQ(status_of(f.post("/files/trash/restore?item=gb%7C20260101-000000-0000")), 404);
  CHECK_EQ(status_of(f.post("/files/trash/restore?item=..%2F..%7Cx")), 404);
  CHECK_EQ(status_of(f.post("/files/trash/restore?item=gb%7C..%2F..%2Fetc")), 404);
  CHECK_EQ(status_of(f.post("/files/trash/restore?item=nosuch%7C20260101-000000-0000")), 404);
  CHECK_EQ(status_of(f.post("/files/trash/restore")), 404);
  CHECK(exists(f.dir + "/Foo.gb") && exists(f.dir + "/Bar.gb"));
}

void test_trash_restore_empty() {
  Struct f;
  CHECK_EQ(f.scanned().size(), size_t(3));
  // a game is running: nothing moves
  oscontract::game_started(f.dir + "/Foo.gb", "Foo Game");
  CHECK_EQ(status_of(f.post("/files/game/trash?id=" + f.id("Foo.gb"))), 409);
  oscontract::game_ended();
  CHECK(exists(f.dir + "/Foo.gb") && f.items().empty());

  // trash Foo: the ROM and the picture move, the entry is kept, the game leaves memory and the gamelist
  const string r = f.post("/files/game/trash?id=" + f.id("Foo.gb"));
  CHECK_EQ(status_of(r), 202);
  f.settle();
  CHECK(!exists(f.dir + "/Foo.gb") && !exists(f.dir + "/images/Foo-image.png"));
  CHECK(exists(f.dir + "/Bar.gb") && exists(f.dir + "/images/Other.png"));
  CHECK_EQ(f.items().size(), size_t(1));
  const string item = f.dir + "/.trash/" + f.items()[0];
  CHECK_EQ(slurp(item + "/Foo.gb"), string("foo rom"));
  CHECK_EQ(slurp(item + "/m0"), kPng);
  const string entry = slurp(item + "/entry.json");
  CHECK(has(entry, "\"Foo Game\"") && has(entry, "About foo &amp; more") == false && has(entry, "About foo & more") && has(entry, "./images/Foo-image.png"));
  CHECK(has(entry, "\"favorite\":\"true\"") && has(entry, "\"rel\":\"Foo.gb\""));
  CHECK(has(slurp(f.xml), "Bar Game") && !has(slurp(f.xml), "Foo.gb") && !has(slurp(f.xml), "Foo Game"));
  CHECK(f.in_memory("Foo.gb") == nullptr && f.in_memory("Bar.gb") != nullptr);
  CHECK(exists(f.dir + "/Foo.srm") && exists(f.states + "/Foo.state"));  // saves stay for a restore
  // .trash never shows as games, whatever is in it (the loader reads the folder again)
  CHECK(f.scanned() == (std::vector<string>{"Bar.gb", "sub/Baz.gb"}));
  CHECK(!has(f.get("/files/games?system=gb"), "Foo Game"));
  // the list
  string lst = f.get("/files/trash");
  CHECK_EQ(status_of(lst), 200);
  CHECK(has(lst, "\"name\":\"Foo Game\"") && has(lst, "\"system\":\"gb\"") && has(lst, "\"size\":7") && has(lst, "\"date\":\"20"));
  CHECK(has(lst, "\"id\":\"gb|" + f.items()[0] + "\""));

  // restore
  const string iid = f.item_id();
  CHECK_EQ(status_of(f.post("/files/trash/restore?item=" + pct(iid))), 202);
  f.settle();
  CHECK_EQ(slurp(f.dir + "/Foo.gb"), string("foo rom"));
  CHECK_EQ(slurp(f.dir + "/images/Foo-image.png"), kPng);
  CHECK(f.items().empty());
  string x = slurp(f.xml);
  CHECK(has(x, "<path>./Foo.gb</path>") && has(x, "<name>Foo Game</name>") && has(x, "About foo &amp; more") && has(x, "<image>./images/Foo-image.png</image>"));
  CHECK(has(x, "<favorite>true</favorite>") && has(x, "<playcount>3</playcount>") && has(x, "Bar Game"));
  CHECK(f.reloads > 0);
  CHECK(has(f.get("/files/trash"), "\"items\":[]"));
  CHECK_EQ(status_of(f.post("/files/trash/restore?item=" + pct(iid))), 404);  // already back

  // a file with that name exists now: 409, the item stays; once it is gone the restore works
  CHECK_EQ(status_of(f.post("/files/game/trash?id=" + f.id("Foo.gb"))), 202);
  f.settle();
  spit(f.dir + "/Foo.gb", "a new foo");
  CHECK_EQ(status_of(f.post("/files/trash/restore?item=" + pct(f.item_id()))), 409);
  CHECK_EQ(slurp(f.dir + "/Foo.gb"), string("a new foo"));
  CHECK_EQ(f.items().size(), size_t(1));
  ::unlink((f.dir + "/Foo.gb").c_str());
  CHECK_EQ(status_of(f.post("/files/trash/restore?item=" + pct(f.item_id()))), 202);
  f.settle();
  CHECK_EQ(slurp(f.dir + "/Foo.gb"), string("foo rom"));

  // a second item of the same game does not clash with the first
  CHECK_EQ(status_of(f.post("/files/game/trash?id=" + f.id("Foo.gb"))), 202);
  f.settle();
  spit(f.dir + "/Foo.gb", "second foo");
  f.publish();  // the new file is not a game of the snapshot: it cannot be named, so it stays
  CHECK_EQ(f.items().size(), size_t(1));
  ::unlink((f.dir + "/Foo.gb").c_str());

  // empty: without confirm nothing goes; with it everything does, and only the trash
  CHECK_EQ(status_of(f.post("/files/trash/empty")), 400);
  CHECK_EQ(status_of(f.post("/files/trash/empty?confirm=0")), 400);
  CHECK_EQ(f.items().size(), size_t(1));
  string e = f.post("/files/trash/empty?confirm=1");
  CHECK_EQ(status_of(e), 200);
  CHECK(has(e, "\"removed\":1"));
  CHECK(!exists(f.dir + "/.trash"));
  CHECK(exists(f.dir + "/Bar.gb") && exists(f.dir + "/Foo.srm") && exists(f.states + "/Foo.state"));
  CHECK_EQ(status_of(f.post("/files/trash/empty?confirm=1")), 200);  // nothing left: still fine
  CHECK(has(f.get("/files/trash"), "\"items\":[]"));
}

void test_trash_without_entry() {
  Struct f;
  CHECK_EQ(status_of(f.post("/files/game/trash?id=" + f.id("sub/Baz.gb"))), 202);
  f.settle();
  CHECK(!exists(f.dir + "/sub/Baz.gb") && f.in_memory("sub/Baz.gb") == nullptr);
  CHECK(has(slurp(f.xml), "Foo Game"));  // the gamelist is not damaged
  CHECK_EQ(status_of(f.post("/files/trash/restore?item=" + pct(f.item_id()))), 202);
  f.settle();
  CHECK_EQ(slurp(f.dir + "/sub/Baz.gb"), string("baz rom"));
  CHECK(!has(slurp(f.xml), "Baz"));  // no entry is made up for a game that had none
}

void test_rename() {
  Struct f;
  const string q = "/files/game/rename?id=" + f.id("Foo.gb");
  CHECK_EQ(status_of(f.post(q)), 400);  // no name
  for (const char* bad : {"a%2Fb.gb", "..%2Fx.gb", "a..b.gb", ".hidden.gb", "x%5Cy.gb", "x%01y.gb", "Foo.gb", "New.sfc", "New", "New.GBC"})
    CHECK_EQ(status_of(f.post(q + "&name=" + bad)), 400);
  CHECK_EQ(status_of(f.post(q + "&name=" + pct(string(300, 'a') + ".gb"))), 400);
  CHECK_EQ(status_of(f.post(q + "&name=Bar.gb")), 409);  // never overwrites
  CHECK_EQ(status_of(f.post(q + "&name=Fine.gb&title=")), 400);  // an empty title is not a title
  CHECK(exists(f.dir + "/Foo.gb") && slurp(f.dir + "/Bar.gb") == "bar rom" && !exists(f.dir + "/Fine.gb"));
  oscontract::game_started(f.dir + "/Foo.gb", "Foo Game");
  CHECK_EQ(status_of(f.post(q + "&name=Fine.gb")), 409);
  oscontract::game_ended();
  CHECK(exists(f.dir + "/Foo.gb"));

  CHECK_EQ(status_of(f.get("/files/game/info?id=" + f.id("Foo.gb"))), 200);
  CHECK(has(f.get("/files/game/info?id=" + f.id("Foo.gb")), "\"file\":\"Foo.gb\"") && has(f.get("/files/game/info?id=" + f.id("Foo.gb")), "\"title\":\"Foo Game\""));

  const string r = f.post(q + "&name=Renamed%20(USA).gb");
  CHECK_EQ(status_of(r), 202);
  f.settle();
  const string nd = f.dir + "/Renamed (USA)";
  CHECK(!exists(f.dir + "/Foo.gb"));
  CHECK_EQ(slurp(nd + ".gb"), string("foo rom"));
  CHECK_EQ(slurp(nd + ".srm"), string("foo save"));
  CHECK(!exists(f.dir + "/Foo.srm"));
  CHECK_EQ(slurp(f.states + "/Renamed (USA).state"), string("s0"));
  CHECK_EQ(slurp(f.states + "/Renamed (USA).state1"), string("s1"));
  CHECK_EQ(slurp(f.states + "/Renamed (USA).state.auto"), string("sa"));
  CHECK_EQ(slurp(f.states + "/Renamed (USA).state.png"), string("sp"));
  CHECK(!exists(f.states + "/Foo.state") && !exists(f.states + "/Foo.state.png"));
  CHECK_EQ(slurp(f.states + "/Foobar.state"), string("not ours"));  // another game's stem
  CHECK_EQ(slurp(f.dir + "/images/Renamed (USA)-image.png"), kPng);
  CHECK(!exists(f.dir + "/images/Foo-image.png") && exists(f.dir + "/images/Other.png"));
  string x = slurp(f.xml);
  CHECK(has(x, "<path>./Renamed (USA).gb</path>") && !has(x, "./Foo.gb"));
  CHECK(has(x, "<image>./images/Renamed (USA)-image.png</image>") && has(x, "<name>Foo Game</name>"));
  CHECK(has(x, "<favorite>true</favorite>") && has(x, "<playcount>3</playcount>") && has(x, "Bar Game"));
  CHECK(f.in_memory("Foo.gb") == nullptr);
  const Game* g = f.in_memory("Renamed (USA).gb");
  CHECK(g != nullptr);
  if (g) CHECK_EQ(g->name, string("Foo Game"));
  CHECK(f.scanned() == (std::vector<string>{"Bar.gb", "Renamed (USA).gb", "sub/Baz.gb"}));

  // with a title; the extension may change its case only
  const string q2 = "/files/game/rename?id=" + f.id("Renamed (USA).gb");
  CHECK_EQ(status_of(f.post(q2 + "&name=Second.GB&title=" + pct("A New Title"))), 202);
  f.settle();
  CHECK(exists(f.dir + "/Second.GB") && !exists(f.dir + "/Renamed (USA).gb"));
  CHECK(has(slurp(f.xml), "<name>A New Title</name>") && has(slurp(f.xml), "<path>./Second.GB</path>"));
  g = f.in_memory("Second.GB");
  CHECK(g != nullptr);
  if (g) CHECK_EQ(g->name, string("A New Title"));
  // the snapshot follows: the old id is gone, the new one launches/renames
  CHECK_EQ(status_of(f.post(q2 + "&name=Third.gb")), 404);

  // a game with no gamelist entry: the file moves, the list follows, no entry is made up
  const string q3 = "/files/game/rename?id=" + f.id("sub/Baz.gb");
  CHECK_EQ(status_of(f.post(q3 + "&name=Qux.gb")), 202);
  f.settle();
  CHECK_EQ(slurp(f.dir + "/sub/Qux.gb"), string("baz rom"));
  CHECK(f.in_memory("sub/Qux.gb") != nullptr && f.in_memory("sub/Baz.gb") == nullptr);
  CHECK(!has(slurp(f.xml), "Qux"));
}

void test_picture() {
  Struct f;
  const string q = "/files/game/picture?id=" + f.id("Foo.gb");
  // wrong magic, too small, too big, a game that does not exist
  CHECK_EQ(status_of(f.post(q, string(100, 'x'))), 400);
  CHECK_EQ(status_of(f.post(q, "GIF89a" + string(100, 'x'))), 400);
  CHECK_EQ(status_of(f.post(q, "tiny")), 400);
  CHECK_EQ(status_of(call(f.port(), "POST", q + "x", kPng)), 404);
  {
    string h = "POST " + q + " HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(9u << 20) + "\r\nX-PIN: " + kPin + "\r\n\r\n";
    CHECK_EQ(status_of(talk(f.port(), h)), 413);  // refused from the header alone
  }
  CHECK_EQ(slurp(f.dir + "/images/Foo-image.png"), kPng);
  for (const string& n : list(f.dir + "/images")) CHECK(!has(n, ".part"));
  CHECK_EQ(status_of(f.get(q)), 405);

  // a PNG replaces the old picture of the game
  const string png = string("\x89PNG\r\n\x1a\n", 8) + string(70000, 'p');
  CHECK_EQ(status_of(f.post(q, png)), 202);
  f.settle();
  CHECK_EQ(slurp(f.dir + "/images/Foo-image.png"), png);
  string x = slurp(f.xml);
  CHECK(has(x, "<image>./images/Foo-image.png</image>") && has(x, "<thumbnail>./images/Foo-image.png</thumbnail>"));
  CHECK(has(x, "<name>Foo Game</name>") && has(x, "<playcount>3</playcount>"));

  // a JPEG for a game with an entry but no picture; a WebP for one with no entry at all
  CHECK_EQ(status_of(f.post("/files/game/picture?id=" + f.id("Bar.gb"), kJpg)), 202);
  f.settle();
  CHECK_EQ(slurp(f.dir + "/images/Bar-image.jpg"), kJpg);
  x = slurp(f.xml);
  CHECK(has(x, "<image>./images/Bar-image.jpg</image>") && has(x, "Bar Game"));
  CHECK_EQ(status_of(f.post("/files/game/picture?id=" + f.id("sub/Baz.gb"), kWebp)), 202);
  f.settle();
  CHECK_EQ(slurp(f.dir + "/images/Baz-image.webp"), kWebp);
  CHECK(has(slurp(f.xml), "<path>./sub/Baz.gb</path>") && has(slurp(f.xml), "<image>./images/Baz-image.webp</image>"));
  CHECK_EQ(f.scanned().size(), size_t(3));  // pictures are not games
  // the phone's cover route now serves the new picture
  CHECK(has(f.get("/files/cover?id=" + f.id("Bar.gb")), "image/jpeg"));

  // the picture of a game that runs may be changed (nothing of the game moves)
  oscontract::game_started(f.dir + "/Foo.gb", "Foo Game");
  CHECK_EQ(status_of(f.post(q, kPng)), 202);
  oscontract::game_ended();
  f.settle();
}

void test_page() {
  string html = webtransfer::page_html();
  for (const char* want : {"/files/game/trash", "/files/game/rename", "/files/game/picture", "/files/game/info", "/files/trash", "/files/trash/restore",
                           "/files/trash/empty?confirm=1", "Move to trash", "Empty trash", "This cannot be undone", "Change picture", "Rename", "Restore",
                           "id=\"mgsheet\"", "Z.onrow=", "if(Z.onrow)Z.onrow(g,li);"})
    CHECK(has(html, want));
  for (const char* bad : {"https://", "src=", "href=", "@import", "<link", "<iframe", "//cdn", "<img src", "window.open", "eval("})
    CHECK(!has(html, bad));
  CHECK(!has(html, "/files/delete"));
}

}  // namespace

// FIND PICTURE: the console opens its pictures-only one-game scrape; refused while a game runs, POST only, unknown game 404
void test_find() {
  Struct f;
  const string q = "/files/game/find?id=" + f.id("Foo.gb");
  CHECK_EQ(status_of(f.get(q)), 405);
  CHECK_EQ(status_of(f.post("/files/game/find?id=nope")), 404);
  oscontract::game_started(f.dir + "/Foo.gb", "Foo Game");
  CHECK_EQ(status_of(f.post(q)), 409);
  oscontract::game_ended();
  CHECK_EQ(status_of(f.post(q)), 202);
  f.settle();
  CHECK(Registry::instance().make("menu.scraper.game.media", f.app->cx, mg::game_arg("gb", f.dir + "/Foo.gb")) != nullptr);
  const string html = webtransfer::page_html();
  CHECK(has(html, "Find picture online") && has(html, "/files/game/find?id="));
  CHECK(has(slurp(f.dir + "/Foo.gb"), "foo rom"));  // nothing on disk moved
}

int main() {
  test_off_and_auth();
  test_unknown();
  test_trash_restore_empty();
  test_trash_without_entry();
  test_rename();
  test_picture();
  test_find();
  test_page();
  return zt::done();
}
