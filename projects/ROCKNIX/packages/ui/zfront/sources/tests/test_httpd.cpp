// httpd: pure parser + the server over real local sockets (port 0).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <cstdio>
#include <string>
#include <thread>

#include "check.hpp"
#include "httpd.hpp"

using namespace zf;
using namespace zf::http;
using std::string;

namespace {

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

// Reads until the server closes (or `ms` pass).
string read_all(int fd, int ms = 3000) {
  string out;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  for (;;) {
    int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count());
    if (left <= 0) break;
    pollfd pf{fd, POLLIN, 0};
    if (::poll(&pf, 1, left) <= 0) break;
    char b[1024];
    ssize_t n = ::recv(fd, b, sizeof b, 0);
    if (n <= 0) break;
    out.append(b, size_t(n));
  }
  return out;
}

string talk(uint16_t port, const string& raw) {
  int fd = connect_to(port);
  if (fd < 0) return "CONNECT FAILED";
  ::send(fd, raw.data(), raw.size(), MSG_NOSIGNAL);
  string r = read_all(fd);
  ::close(fd);
  return r;
}

bool has(const string& s, const char* part) { return s.find(part) != string::npos; }

Response echo(const Request& r) {
  Response o;
  o.body = "m=" + r.method + " p=" + r.path + " q=" + r.query + " b=" + r.body;
  return o;
}

}  // namespace

int main() {
  // ---- parser
  {
    Request r;
    CHECK(parse_request("GET /runningGame HTTP/1.1\r\nHost: x\r\n\r\n", r) == Parse::Ok);
    CHECK_EQ(r.method, string("GET"));
    CHECK_EQ(r.path, string("/runningGame"));
    CHECK(r.query.empty() && r.body.empty());

    Request p;
    CHECK(parse_request("POST /notify?duration=1500&x=a%20b HTTP/1.1\r\ncontent-length: 5\r\n\r\nhelloEXTRA", p) == Parse::Ok);
    CHECK_EQ(p.body, string("hello"));  // only Content-Length bytes
    CHECK_EQ(p.query, string("duration=1500&x=a%20b"));
    string v;
    CHECK(query_param(p, "duration", v) && v == "1500");
    CHECK(query_param(p, "x", v) && v == "a b");
    CHECK(!query_param(p, "nope", v));

    Request q;
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc", q) == Parse::NeedMore);
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: 10\r\nExpect: 100-continue\r\n\r\n", q) == Parse::NeedMore && q.expect_continue);
    CHECK(parse_request("GET /x HTTP/1.1\r\nHost", q) == Parse::NeedMore);
    CHECK(parse_request("POST /x HTTP/1.1\r\n\r\n", q) == Parse::Ok && q.body.empty());  // no Content-Length = no body

    Request b;
    CHECK(parse_request("GET\r\n\r\n", b) == Parse::Bad);
    CHECK(parse_request("GET /x HTTP/2.0\r\n\r\n", b) == Parse::Bad);
    CHECK(parse_request("GET x HTTP/1.1\r\n\r\n", b) == Parse::Bad);       // target must start with '/'
    CHECK(parse_request("get /x HTTP/1.1\r\n\r\n", b) == Parse::Bad);       // lower-case method
    CHECK(parse_request("GET /a b HTTP/1.1\r\n\r\n", b) == Parse::Bad);    // space in the target
    CHECK(parse_request("GET /x HTTP/1.1\r\nBad Header: 1\r\n\r\n", b) == Parse::Bad);
    CHECK(parse_request("GET /x HTTP/1.1\r\n folded: 1\r\n\r\n", b) == Parse::Bad);
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: -1\r\n\r\n", b) == Parse::Bad);
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: 1x\r\n\r\n", b) == Parse::Bad);
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nab", b) == Parse::Bad);
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nab", b) == Parse::Ok);
    CHECK(parse_request("POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", b) == Parse::NotImplemented);
    CHECK(parse_request("DELETE /x HTTP/1.1\r\n\r\n", b) == Parse::NotImplemented);
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: 70000\r\n\r\n", b) == Parse::TooLarge);
    CHECK(parse_request("POST /x HTTP/1.1\r\nContent-Length: 65536\r\n\r\n", b) == Parse::NeedMore);
    CHECK(parse_request("GET /" + string(3000, 'a') + " HTTP/1.1\r\n\r\n", b) == Parse::TooLarge);
    CHECK(parse_request("GET /x HTTP/1.1\r\nX: " + string(9000, 'a') + "\r\n\r\n", b) == Parse::TooLarge);
    CHECK(parse_request(string(9000, 'a'), b) == Parse::TooLarge);  // no end of headers in sight

    string d;
    CHECK(url_decode("a%2Fb+c", d) && d == "a/b c");
    CHECK(!url_decode("bad%2", d));
    CHECK(!url_decode("bad%zz", d));
    CHECK(!url_decode("%", d));
    Response rs;
    rs.status = 201;
    rs.body = "[ false ]";
    string f = format_response(rs);
    CHECK(has(f.c_str(), "HTTP/1.1 201 "));
    CHECK(has(f.c_str(), "Content-Length: 9\r\n") && has(f.c_str(), "Connection: close\r\n") && f.size() > 9 && f.substr(f.size() - 9) == "[ false ]");
  }

  // ---- server over sockets
  {
    Server s;
    string err;
    ServerLimits lim;
    lim.read_deadline_ms = 400;
    CHECK(s.start(0, echo, err, "127.0.0.1", lim));
    uint16_t port = s.port();
    CHECK(port != 0 && s.running());

    string r = talk(port, "GET /a?b=1 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(has(r.c_str(), "HTTP/1.1 200 OK"));
    CHECK(has(r.c_str(), "m=GET p=/a q=b=1 b="));
    CHECK(has(r.c_str(), "Connection: close"));

    {  // body in two pieces
      int fd = connect_to(port);
      const string a = "POST /launch HTTP/1.1\r\nContent-Length: 11\r\n\r\n/storage";
      ::send(fd, a.data(), a.size(), 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      ::send(fd, "/roms", 5, 0);  // 8 + 5 = 13 > 11: the extra bytes are ignored
      string x = read_all(fd);
      ::close(fd);
      CHECK(has(x.c_str(), "m=POST p=/launch q= b=/storage/ro"));
    }
    {  // Expect: 100-continue
      int fd = connect_to(port);
      const string h = "POST /notify HTTP/1.1\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n";
      ::send(fd, h.data(), h.size(), 0);
      char b[64] = {0};
      pollfd pf{fd, POLLIN, 0};
      CHECK(::poll(&pf, 1, 1000) > 0);
      ssize_t n = ::recv(fd, b, sizeof b - 1, 0);
      CHECK(n > 0 && has(b, "100 Continue"));
      ::send(fd, "abc", 3, 0);
      string x = read_all(fd);
      ::close(fd);
      CHECK(has(x.c_str(), "b=abc"));
    }
    CHECK(has(talk(port, "garbage\r\n\r\n").c_str(), "HTTP/1.1 400"));
    CHECK(has(talk(port, "GET /x HTTP/1.1\r\nX: " + string(9000, 'a') + "\r\n\r\n").c_str(), "HTTP/1.1 413"));
    CHECK(has(talk(port, "POST /x HTTP/1.1\r\nContent-Length: 99999\r\n\r\n").c_str(), "HTTP/1.1 413"));
    CHECK(has(talk(port, "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n").c_str(), "HTTP/1.1 501"));
    CHECK(has(talk(port, "DELETE /x HTTP/1.1\r\n\r\n").c_str(), "HTTP/1.1 501"));
    {  // a client that stalls gets 408 after the deadline and does not block the next one for long
      auto t0 = std::chrono::steady_clock::now();
      int fd = connect_to(port);
      ::send(fd, "GET /x HTTP/1.1\r\n", 17, 0);
      string x = read_all(fd, 3000);
      ::close(fd);
      CHECK(has(x.c_str(), "HTTP/1.1 408"));
      long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
      CHECK(ms >= 300 && ms < 1500);
    }
    {  // a client that leaves half-way
      int fd = connect_to(port);
      ::send(fd, "POST /x HTTP/1.1\r\nContent-Length: 10\r\n\r\nab", 42, 0);
      ::close(fd);
    }
    CHECK(has(talk(port, "GET /still HTTP/1.1\r\n\r\n").c_str(), "p=/still"));  // still serving

    s.stop();
    CHECK(!s.running());
    s.stop();  // twice is fine
    CHECK_EQ(connect_to(port), -1);

    // the port can be taken again at once (SO_REUSEADDR) and a busy port is reported
    Server s2, s3;
    CHECK(s2.start(port, echo, err));
    CHECK(!s3.start(port, echo, err) && !err.empty());
    s2.stop();
  }

  // ---- an idle server uses no CPU: it sleeps in poll(2) with no timeout
  {
    Server s;
    string err;
    CHECK(s.start(0, echo, err));
    clock_t c0 = std::clock();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(std::clock() - c0 < CLOCKS_PER_SEC / 100);  // < 10 ms of CPU in 500 ms of idling
    s.stop();
  }
  return zt::done();
}
