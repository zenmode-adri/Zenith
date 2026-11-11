// webpair: phones the console remembers. The real httpd server on 127.0.0.1 (port 0) with webtransfer's routes; the console's
// question is a test asker answered from a thread (yes, no, never), the list file lives in mkdtemp.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "httpd.hpp"
#include "webcore.hpp"
#include "webpair.hpp"
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

string talk(uint16_t port, const string& raw, int ms = 8000) {
  int fd = connect_to(port);
  if (fd < 0) return "";
  size_t off = 0;
  while (off < raw.size()) {
    ssize_t n = ::send(fd, raw.data() + off, raw.size() - off, MSG_NOSIGNAL);
    if (n <= 0) break;
    off += size_t(n);
  }
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
  ::close(fd);
  return out;
}

int status_of(const string& r) {
  if (r.compare(0, 9, "HTTP/1.1 ") != 0 || r.size() < 12) return 0;
  return std::atoi(r.c_str() + 9);
}

string body_of(const string& r) {
  size_t p = r.find("\r\n\r\n");
  return p == string::npos ? "" : r.substr(p + 4);
}

string pair_req(uint16_t port, const string& name, const char* pin = kPin) {
  string h = "POST /files/pair HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(name.size()) + "\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  return talk(port, h + "\r\n" + name);
}

string get(uint16_t port, const string& path, const string& header) {
  return talk(port, "GET " + path + " HTTP/1.1\r\nHost: x\r\n" + header + "\r\n");
}

string token_of(const string& r) {
  const string b = body_of(r), k = "\"token\":\"";
  size_t p = b.find(k);
  return p == string::npos ? "" : b.substr(p + k.size(), 64);
}

// What the console "answers" to the next question: 1 yes, 0 no, -1 never.
std::atomic<int> g_reply{1};
std::atomic<int> g_asked{0};
string g_last_name;

struct Rig {
  string root, file;
  http::Server srv;
  webtransfer::Env env;
  long long now = 1000000;

  Rig() {
    char t[] = "/tmp/zf-webpair-XXXXXX";
    root = ::mkdtemp(t);
    file = root + "/cfg/webdevices";
    ::setenv("ZF_WEBDEVICES", file.c_str(), 1);
    webtransfer::reset_for_test(kPin);
    webpair::reset_for_test();
    webpair::set_ask_timeout_ms(1500);
    webpair::set_clock_for_test([this] { return now; });
    webpair::set_asker_for_test([](const string& name, unsigned gen, const webtransfer::Env&) {
      ++g_asked;
      g_last_name = name;
      const int r = g_reply.load();
      if (r >= 0)
        std::thread([gen, r] {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          webpair::answer(gen, r == 1);
        }).detach();
      return true;
    });
    webtransfer::SystemInfo si;
    si.name = "gb";
    si.fullname = "Game Boy";
    si.rom_dir = root;
    si.exts = {".gb"};
    webtransfer::publish_info({si});
    webtransfer::set_enabled(true);
    srv.set_stream_handler([this](const http::Request& r, http::BodyStream& b) { return webtransfer::stream_route(r, b, env); }, webtransfer::stream_pred);
    string err;
    CHECK(srv.start(0, [this](const http::Request& r) { return webtransfer::route(r, env); }, err));
  }
  ~Rig() {
    srv.stop();
    webpair::reset_for_test();
    ::unsetenv("ZF_WEBDEVICES");
    string cmd = "rm -rf '" + root + "'";
    if (std::system(cmd.c_str()) != 0) {
    }
  }
  uint16_t port() const { return srv.port(); }
};

string slurp(const string& p) {
  string s;
  if (FILE* f = std::fopen(p.c_str(), "rb")) {
    char b[4096];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
  }
  return s;
}

void test_pair_yes_and_token() {
  Rig r;
  g_reply = 1;
  g_asked = 0;
  string x = pair_req(r.port(), "Android phone");
  CHECK_EQ(status_of(x), 200);
  CHECK_EQ(g_asked.load(), 1);
  CHECK(g_last_name == "Android phone");
  const string tok = token_of(x);
  CHECK_EQ(tok.size(), size_t(64));
  CHECK(x.find("Access-Control") == string::npos);
  CHECK(x.find("Set-Cookie") == string::npos);
  // the token opens a guarded route without the PIN
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-Token: " + tok + "\r\n")), 200);
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "")), 403);
  // the bare address the menu shows (no path) sends the browser on to the page
  {
    const string root = get(r.port(), "/", "");
    CHECK_EQ(status_of(root), 302);
    CHECK(root.find("Location: /files") != string::npos);
  }
  // the file: mode 0600, one line, the token and the name
  struct stat sb;
  CHECK(::stat(r.file.c_str(), &sb) == 0);
  CHECK_EQ(int(sb.st_mode & 0777), 0600);
  const string text = slurp(r.file);
  CHECK(text.find(tok + "\tAndroid phone\t") == 0);
  // it survives a restart (the list read again from the file)
  webpair::reload_for_test();
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-Token: " + tok + "\r\n")), 200);
  std::vector<webpair::Device> d = webpair::devices();
  CHECK_EQ(d.size(), size_t(1));
  if (!d.empty()) {
    CHECK(d[0].name == "Android phone");
    CHECK(d[0].id == tok.substr(0, 12));
    CHECK(text.find(d[0].id) == 0);
  }
  // forgotten: 403 again, and the file no longer has it
  CHECK(!d.empty() && webpair::forget(d[0].id));
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-Token: " + tok + "\r\n")), 403);
  CHECK(slurp(r.file).find(tok) == string::npos);
  CHECK(!webpair::forget("000000000000"));
}

void test_pair_refused() {
  Rig r;
  g_asked = 0;
  // wrong PIN: 403 before asking anything
  CHECK_EQ(status_of(pair_req(r.port(), "x", "0000")), 403);
  CHECK_EQ(status_of(pair_req(r.port(), "x", nullptr)), 403);
  CHECK_EQ(g_asked.load(), 0);
  // the console says no, or never answers: 403, no token, nothing kept
  g_reply = 0;
  string x = pair_req(r.port(), "iPhone");
  CHECK_EQ(status_of(x), 403);
  CHECK(token_of(x).empty());
  g_reply = -1;
  const auto t0 = std::chrono::steady_clock::now();
  x = pair_req(r.port(), "iPhone");
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK_EQ(status_of(x), 403);
  CHECK(ms >= 1400);
  CHECK(webpair::devices().empty());
  // a late answer to a question that is closed changes nothing
  CHECK(!webpair::answer(1, true));
  CHECK(!webpair::answer(999, true));
  CHECK(webpair::devices().empty());
}

void test_lockout() {
  Rig r;
  g_reply = 1;
  // five wrong attempts (PINs or tokens) lock the PIN and the pairing; a device already paired still gets in
  string tok = token_of(pair_req(r.port(), "PC"));
  CHECK_EQ(tok.size(), size_t(64));
  const string bad(64, 'f');
  for (int i = 0; i < 3; ++i) CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-Token: " + bad + "\r\n")), 403);
  CHECK_EQ(status_of(pair_req(r.port(), "x", "1111")), 403);
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-PIN: 2222\r\n")), 403);
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-PIN: " + string(kPin) + "\r\n")), 429);
  CHECK_EQ(status_of(pair_req(r.port(), "x")), 429);
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-Token: " + tok + "\r\n")), 200);
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-Token: " + bad + "\r\n")), 429);
}

void test_concurrent() {
  Rig r;
  g_reply = -1;  // the first question stays open
  webpair::set_ask_timeout_ms(2500);
  string first;
  std::thread t([&] { first = pair_req(r.port(), "first"); });
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const int second = status_of(pair_req(r.port(), "second"));
  CHECK(second == 409);  // another question open (or the worker taken)
  t.join();
  CHECK_EQ(status_of(first), 403);
}

void test_limits() {
  Rig r;
  g_reply = 1;
  std::vector<string> toks;
  for (int i = 0; i < 12; ++i) {
    r.now += 10;
    toks.push_back(token_of(pair_req(r.port(), "Phone " + std::to_string(i))));
    CHECK_EQ(toks.back().size(), size_t(64));
  }
  std::vector<webpair::Device> d = webpair::devices();
  CHECK_EQ(d.size(), webpair::kMaxDevices);
  if (!d.empty()) CHECK(d.front().name == "Phone 2");  // the two oldest went
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-Token: " + toks[0] + "\r\n")), 403);
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-Token: " + toks[11] + "\r\n")), 200);
  // last seen: written at most once a minute
  struct stat a, b;
  CHECK(::stat(r.file.c_str(), &a) == 0);
  const string before = slurp(r.file);
  r.now += 30;
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-Token: " + toks[11] + "\r\n")), 200);
  CHECK(slurp(r.file) == before);
  r.now += 40;
  CHECK_EQ(status_of(get(r.port(), "/files/systems", "X-Token: " + toks[11] + "\r\n")), 200);
  CHECK(slurp(r.file) != before);
  CHECK(slurp(r.file).find(toks[11] + "\tPhone 11\t") != string::npos);
  CHECK(::stat(r.file.c_str(), &b) == 0);
  webpair::forget_all();
  CHECK(webpair::devices().empty());
  CHECK(slurp(r.file).empty());
}

void test_names() {
  CHECK(webpair::sanitize_name("") == "Unknown device");
  CHECK(webpair::sanitize_name("   ") == "Unknown device");
  CHECK(webpair::sanitize_name("a\tb\nc\x01") == "a b c");
  CHECK(webpair::sanitize_name("\xC3\xA9t\xC3\xA9") == "t");
  CHECK_EQ(webpair::sanitize_name(string(100, 'x')).size(), webpair::kMaxNameChars);
}

void test_page() {
  const string html = webtransfer::page_html();
  CHECK(html.find("Access-Control") == string::npos);
  CHECK(html.find("document.cookie") == string::npos);
  CHECK(html.find("X-Token") != string::npos);
  CHECK(html.find("/files/pair") != string::npos);
  CHECK(html.find("history.replaceState") != string::npos);
  CHECK(html.find("localStorage.setItem(\"zf_token\"") != string::npos);
  CHECK(html.find("localStorage.setItem(\"zf_pin\"") == string::npos);  // the PIN is never kept
}

}  // namespace

int main() {
  test_names();
  test_pair_yes_and_token();
  test_pair_refused();
  test_lockout();
  test_concurrent();
  test_limits();
  test_page();
  return zt::done();
}
