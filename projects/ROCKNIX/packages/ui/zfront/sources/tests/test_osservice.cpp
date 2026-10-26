// osservice: the registered "http" service end to end on the PC: socket -> route -> Jobs -> main thread (drain_jobs) ->
// toasts / launch request / quit mode. Dry-run everywhere; the server stops through `oscontract::finish`.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <string>

#include "app.hpp"
#include "check.hpp"
#include "oscontract.hpp"
#include "registry.hpp"

using namespace zf;
using std::string;

namespace {

struct DummyScreen : Screen {
  Transition handle(Action, Ctx&) override { return Transition::none(); }
  void draw(Ctx&, std::vector<Op>&) override {}
};

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
  if (fd < 0) return "CONNECT FAILED";
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

string post(uint16_t port, const string& path, const string& body) {
  return talk(port, "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
}
string get(uint16_t port, const string& path) { return talk(port, "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"); }

bool has(const string& s, const char* part) { return s.find(part) != string::npos; }

// main-thread side: run finished job callbacks until `pred` holds
template <class F>
bool drain_until(App& app, F pred, int ms = 3000) {
  for (int i = 0; i < ms; ++i) {
    app.drain_jobs(Clock::now());
    if (pred()) return true;
    ::usleep(1000);
  }
  return false;
}

uint16_t free_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
  socklen_t l = sizeof sa;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &l);
  ::close(fd);
  return ntohs(sa.sin_port);
}

}  // namespace

int main() {
  oscontract::reset_for_test();
  uint16_t port = free_port();
  ::setenv("ZF_HTTP_PORT", std::to_string(port).c_str(), 1);
  ::unsetenv("ES_WAIT_WAYLAND_SOCKET");

  Ctx cx;
  cx.sv->cmd.dry_run = true;
  oscontract::startup(cx.sv, true);
  oscontract::install_signals(false);
  System sy;
  sy.name = sy.fullname = sy.theme = "gb";
  sy.rom_dir = "/tmp/zf-osservice-roms/gb";
  sy.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR% %GAMENAME% --controllers=\"%CONTROLLERSCONFIG%\"";
  sy.emulator = "libretro";
  sy.core = "gambatte";
  sy.loaded = true;
  Game g;
  g.name = "Tetris";
  g.path = sy.rom_dir + "/Tetris.gb";
  sy.games = {g};
  sy.count = 1;
  cx.systems.push_back(sy);
  App app(std::move(cx), std::make_unique<DummyScreen>(), make_status_source("/nonexistent"));

  Registry::instance().start_services(app);  // "oscontract" (wait: nothing requested) and "http" on ZF_HTTP_PORT

  string r = post(port, "/notify?duration=2000", "Suspendiendo...");
  CHECK(has(r.c_str(), "HTTP/1.1 200 OK"));
  CHECK(drain_until(app, [&] { return !app.cx.sv->toasts.items().empty(); }));
  CHECK(app.cx.sv->toasts.items().size() == 1 && app.cx.sv->toasts.items()[0].text == "Suspendiendo...");
  auto left = app.cx.sv->toasts.items()[0].until - Clock::now();
  CHECK(left > std::chrono::milliseconds(1000) && left <= std::chrono::milliseconds(2000));

  post(port, "/notify/clear", "x");  // after resume: everything goes, whatever its deadline
  CHECK(drain_until(app, [&] { return app.cx.sv->toasts.items().empty(); }));

  CHECK(has(get(port, "/runningGame").c_str(), "HTTP/1.1 201 "));
  CHECK(has(get(port, "/isIdle").c_str(), "[ true ]"));

  r = post(port, "/launch", "/tmp/zf-osservice-roms/gb/Tetris.gb\n");
  CHECK(has(r.c_str(), "HTTP/1.1 200 OK"));
  std::optional<Outcome> ex;
  CHECK(drain_until(app, [&] { return (ex = oscontract::take_external(app)).has_value(); }));
  CHECK(ex && ex->kind == Outcome::Launch && ex->cmd.find("Tetris.gb") != string::npos);

  // a game is "running" on the main thread: the HTTP thread still answers, a second launch is refused
  oscontract::game_started("/tmp/zf-osservice-roms/gb/Tetris.gb", "Tetris");
  CHECK(has(get(port, "/runningGame").c_str(), "HTTP/1.1 200 OK"));
  CHECK(has(get(port, "/isIdle").c_str(), "[ false ]"));
  CHECK(has(post(port, "/launch", "/tmp/zf-osservice-roms/gb/Tetris.gb").c_str(), "HTTP/1.1 409 "));
  oscontract::game_ended();

  CHECK(!oscontract::quit_requested());
  get(port, "/restart");  // ES quirk: the SYSTEM reboots
  CHECK(drain_until(app, [&] { return oscontract::quit_requested(); }));
  CHECK_EQ(int(oscontract::quit_mode()), int(oscontract::Mode::Reboot));
  bool ev = false;
  for (const string& l : oscontract::dry_log())
    if (l == "event quit reboot") ev = true;
  CHECK(ev);

  oscontract::finish();  // stops the server (on_finish) and, in dry-run, only logs the shutdown
  bool shut = false;
  for (const string& l : app.cx.sv->cmd.log)
    if (l == "shutdown -r now") shut = true;
  CHECK(shut);
  CHECK_EQ(connect_to(port), -1);  // the port is closed
  return zt::done();
}
