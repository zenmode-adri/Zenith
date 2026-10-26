// osapi: the routes the OS uses (sleep.sh, rocknix-fake-suspend...), directly and over a real local socket (port 0).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <vector>

#include "check.hpp"
#include "osapi.hpp"

using namespace zf;
using std::string;
using std::vector;

namespace {

http::Request req(const char* method, const char* path, const char* query = "", const char* body = "") {
  http::Request r;
  r.method = method;
  r.path = path;
  r.query = query;
  r.body = body;
  return r;
}

struct Rec {
  vector<string> notes;
  vector<int> ms;
  int clears = 0;
  vector<oscontract::Mode> quits;
  vector<string> launches;
  osapi::Ops ops() {
    osapi::Ops o;
    o.notify = [this](string t, int d) {
      notes.push_back(t);
      ms.push_back(d);
    };
    o.notify_clear = [this] { ++clears; };
    o.quit = [this](oscontract::Mode m) { quits.push_back(m); };
    o.launch = [this](string p) { launches.push_back(p); };
    return o;
  }
};

string talk(uint16_t port, const string& raw) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) return "CONNECT FAILED";
  ::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
  string out;
  for (;;) {
    pollfd pf{fd, POLLIN, 0};
    if (::poll(&pf, 1, 3000) <= 0) break;
    char b[1024];
    ssize_t n = ::recv(fd, b, sizeof b, 0);
    if (n <= 0) break;
    out.append(b, size_t(n));
  }
  ::close(fd);
  return out;
}

bool has(const string& s, const char* part) { return s.find(part) != string::npos; }

}  // namespace

int main() {
  oscontract::reset_for_test();
  Rec rec;
  osapi::Ops ops = rec.ops();

  // ---- notify
  {
    auto r = osapi::route(req("POST", "/notify", "duration=1500", "Suspending..."), ops);
    CHECK_EQ(r.status, 200);
    CHECK(rec.notes.size() == 1 && rec.notes[0] == "Suspending..." && rec.ms[0] == 1500);
    osapi::route(req("POST", "/notify", "", "Hola"), ops);
    CHECK_EQ(rec.ms.back(), osapi::kDefaultNotifyMs);
    osapi::route(req("POST", "/notify", "duration=5", "x"), ops);
    CHECK_EQ(rec.ms.back(), osapi::kMinNotifyMs);
    osapi::route(req("POST", "/notify", "duration=99999999", "x"), ops);
    CHECK_EQ(rec.ms.back(), osapi::kMaxNotifyMs);
    osapi::route(req("POST", "/notify", "duration=abc", "x"), ops);
    CHECK_EQ(rec.ms.back(), osapi::kDefaultNotifyMs);
    CHECK_EQ(osapi::route(req("POST", "/notify", "", ""), ops).status, 400);
    CHECK_EQ(osapi::route(req("POST", "/notify", "", "  \r\n"), ops).status, 400);
    CHECK_EQ(osapi::route(req("GET", "/notify"), ops).status, 405);
    size_t n = rec.notes.size();
    osapi::route(req("POST", "/notify", "", "a\nb\x01"), ops);
    CHECK_EQ(rec.notes.size(), n + 1);
    CHECK_EQ(rec.notes.back(), string("a b"));  // one line, no control characters
    CHECK_EQ(osapi::route(req("POST", "/notify/clear", "", "x"), ops).status, 200);
    CHECK_EQ(rec.clears, 1);
    CHECK_EQ(osapi::route(req("GET", "/notify/clear"), ops).status, 405);
  }

  // ---- runningGame / isIdle (200 / 201, what rocknix-fake-suspend branches on)
  {
    auto r = osapi::route(req("GET", "/runningGame"), ops);
    CHECK_EQ(r.status, 201);
    CHECK(has(r.body, "NO GAME RUNNING"));
    auto i = osapi::route(req("GET", "/isIdle"), ops);
    CHECK(i.status == 200 && i.body == "[ true ]");
    oscontract::game_started("/storage/roms/gb/T.gb", "Tetris");
    r = osapi::route(req("GET", "/runningGame"), ops);
    CHECK_EQ(r.status, 200);
    CHECK(has(r.body, "\"name\":\"Tetris\"") && has(r.body, "T.gb"));
    i = osapi::route(req("GET", "/isIdle"), ops);
    CHECK(i.status == 201 && i.body == "[ false ]");
    // while a game runs a second /launch is refused
    CHECK_EQ(osapi::route(req("POST", "/launch", "", "/storage/roms/gb/U.gb"), ops).status, 409);
    CHECK(rec.launches.empty());
    oscontract::game_ended();
    {
      oscontract::Busy b;
      CHECK_EQ(osapi::route(req("GET", "/isIdle"), ops).status, 201);
    }
    CHECK_EQ(osapi::route(req("GET", "/isIdle"), ops).status, 200);
    CHECK_EQ(osapi::route(req("POST", "/runningGame"), ops).status, 405);
  }

  // ---- quit / shutdown / restart (ES: /restart reboots the system)
  {
    CHECK_EQ(osapi::route(req("GET", "/quit"), ops).status, 200);
    CHECK_EQ(osapi::route(req("GET", "/shutdown"), ops).status, 200);
    CHECK_EQ(osapi::route(req("GET", "/restart"), ops).status, 200);
    CHECK(rec.quits.size() == 3 && rec.quits[0] == oscontract::Mode::Quit && rec.quits[1] == oscontract::Mode::Shutdown && rec.quits[2] == oscontract::Mode::Reboot);
    CHECK_EQ(osapi::route(req("GET", "/quit", "confirm=menu"), ops).status, 501);  // never quit without the asked-for confirmation
    CHECK_EQ(rec.quits.size(), 3ul);
    CHECK_EQ(osapi::route(req("POST", "/quit"), ops).status, 405);
  }

  // ---- launch
  {
    CHECK_EQ(osapi::route(req("POST", "/launch", "", "/storage/roms/gb/T.gb\r\n"), ops).status, 200);
    CHECK(rec.launches.size() == 1 && rec.launches[0] == "/storage/roms/gb/T.gb");  // trailing CRLF trimmed
    CHECK_EQ(osapi::route(req("POST", "/launch", "", ""), ops).status, 400);
    CHECK_EQ(osapi::route(req("POST", "/launch", "", "relative/path.gb"), ops).status, 400);
    CHECK_EQ(osapi::route(req("GET", "/launch"), ops).status, 405);
    CHECK_EQ(rec.launches.size(), 1ul);
  }

  // ---- emukill: JSON answer, no game = nothing killed
  {
    auto r = osapi::route(req("GET", "/emukill"), ops);
    CHECK(r.status == 200 && has(r.body, "false"));
  }

  // ---- everything else: 404 JSON
  for (const char* p : {"/", "/systems", "/systems/gb/games", "/caps", "/reloadgames", "/index.html", "/notify/", "/launch/x", "/messagebox"}) {
    auto r = osapi::route(req("GET", p), ops);
    CHECK_EQ(r.status, 404);
    CHECK(r.content_type == "application/json" && has(r.body, "not found"));
  }

  // ---- over a real socket, as curl in sleep.sh does it
  {
    Rec r2;
    osapi::Ops o2 = r2.ops();
    http::Server s;
    string err;
    CHECK(s.start(0, [o2](const http::Request& q) { return osapi::route(q, o2); }, err));
    uint16_t port = s.port();
    string note = "Suspendiendo...";
    string resp = talk(port, "POST /notify?duration=1500 HTTP/1.1\r\nHost: 127.0.0.1:1234\r\nUser-Agent: curl/8\r\nAccept: */*\r\nContent-Type: text/plain\r\nContent-Length: " +
                                     std::to_string(note.size()) + "\r\n\r\n" + note);
    CHECK(has(resp, "HTTP/1.1 200 OK"));
    CHECK(r2.notes.size() == 1 && r2.notes[0] == note && r2.ms[0] == 1500);
    resp = talk(port, "POST /notify/clear HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 1\r\n\r\nx");
    CHECK(has(resp, "HTTP/1.1 200 OK") && r2.clears == 1);
    resp = talk(port, "GET /runningGame HTTP/1.1\r\nHost: localhost:1234\r\nUser-Agent: curl/8\r\nAccept: */*\r\n\r\n");
    CHECK(has(resp, "HTTP/1.1 201 "));  // rocknix-fake-suspend: 201 = no game running
    oscontract::game_started("/r/x.gb", "X");
    resp = talk(port, "GET /runningGame HTTP/1.1\r\n\r\n");
    CHECK(has(resp, "HTTP/1.1 200 OK") && has(resp, "\"name\":\"X\""));  // answered while "the main thread is in the game"
    oscontract::game_ended();
    resp = talk(port, "GET /nope HTTP/1.1\r\n\r\n");
    CHECK(has(resp, "HTTP/1.1 404 ") && has(resp, "{\"error\":\"not found\"}"));
    s.stop();
  }
  return zt::done();
}
