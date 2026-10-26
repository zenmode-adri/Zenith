#include "httpd.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace zf {
namespace http {

namespace {

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = char(x + 32);
    if (y >= 'A' && y <= 'Z') y = char(y + 32);
    if (x != y) return false;
  }
  return true;
}

std::string_view trim_ws(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// RFC 7230 token character (header names)
bool is_token_char(unsigned char c) {
  if (c <= 0x20 || c >= 0x7f) return false;
  switch (c) {
    case '(': case ')': case ',': case '/': case ':': case ';': case '<': case '=': case '>': case '?': case '@':
    case '[': case '\\': case ']': case '{': case '}': case '"':
      return false;
    default:
      return true;
  }
}

int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

Parse parse_request(std::string_view buf, Request& req) {
  size_t he = buf.find("\r\n\r\n");
  if (he == std::string_view::npos) return buf.size() > kMaxHeaderBytes ? Parse::TooLarge : Parse::NeedMore;
  if (he + 4 > kMaxHeaderBytes) return Parse::TooLarge;
  std::string_view head = buf.substr(0, he);

  // ---- request line
  size_t le = head.find("\r\n");
  std::string_view line = head.substr(0, le);
  size_t s1 = line.find(' ');
  if (s1 == std::string_view::npos || s1 == 0 || s1 > 8) return Parse::Bad;
  std::string_view method = line.substr(0, s1);
  for (char c : method)
    if (c < 'A' || c > 'Z') return Parse::Bad;
  size_t s2 = line.find(' ', s1 + 1);
  if (s2 == std::string_view::npos) return Parse::Bad;
  std::string_view target = line.substr(s1 + 1, s2 - s1 - 1);
  std::string_view version = line.substr(s2 + 1);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") return Parse::Bad;
  if (target.empty() || target[0] != '/') return Parse::Bad;
  if (target.size() > kMaxTargetBytes) return Parse::TooLarge;
  for (char c : target)
    if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) == 0x7f) return Parse::Bad;
  if (method != "GET" && method != "POST") return Parse::NotImplemented;

  // ---- headers
  bool have_len = false, expect = false;
  size_t clen = 0;
  size_t pos = le == std::string_view::npos ? head.size() : le + 2;
  while (pos < head.size()) {
    size_t e = head.find("\r\n", pos);
    std::string_view h = head.substr(pos, e == std::string_view::npos ? std::string_view::npos : e - pos);
    pos = e == std::string_view::npos ? head.size() : e + 2;
    if (h.empty() || h[0] == ' ' || h[0] == '\t') return Parse::Bad;  // empty line inside the block / obsolete folding
    size_t colon = h.find(':');
    if (colon == std::string_view::npos || colon == 0) return Parse::Bad;
    std::string_view name = h.substr(0, colon);
    for (char c : name)
      if (!is_token_char(static_cast<unsigned char>(c))) return Parse::Bad;
    std::string_view value = trim_ws(h.substr(colon + 1));
    if (ieq(name, "content-length")) {
      if (value.empty() || value.size() > 9) return Parse::Bad;
      size_t n = 0;
      for (char c : value) {
        if (c < '0' || c > '9') return Parse::Bad;
        n = n * 10 + size_t(c - '0');
      }
      if (have_len && n != clen) return Parse::Bad;  // conflicting lengths
      have_len = true;
      clen = n;
    } else if (ieq(name, "transfer-encoding")) {
      return Parse::NotImplemented;  // chunked (or anything else) is not supported
    } else if (ieq(name, "expect")) {
      expect = ieq(value, "100-continue");
    }
  }
  if (clen > kMaxBodyBytes) return Parse::TooLarge;

  size_t total = he + 4 + clen;
  if (buf.size() < total) {
    req.expect_continue = expect;
    return Parse::NeedMore;
  }
  req.method.assign(method);
  size_t q = target.find('?');
  req.path.assign(target.substr(0, q));
  req.query.assign(q == std::string_view::npos ? std::string_view() : target.substr(q + 1));
  req.body.assign(buf.substr(he + 4, clen));
  req.expect_continue = false;
  return Parse::Ok;
}

bool url_decode(std::string_view in, std::string& out) {
  out.clear();
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%') {
      if (i + 2 >= in.size()) return false;
      int a = hexval(in[i + 1]), b = hexval(in[i + 2]);
      if (a < 0 || b < 0) return false;
      out.push_back(char(a * 16 + b));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return true;
}

bool query_param(const Request& req, std::string_view key, std::string& out) {
  std::string_view q = req.query;
  size_t p = 0;
  while (p <= q.size()) {
    size_t e = q.find('&', p);
    if (e == std::string_view::npos) e = q.size();
    std::string_view kv = q.substr(p, e - p);
    size_t eq = kv.find('=');
    std::string_view k = kv.substr(0, eq);
    std::string dk;
    if (url_decode(k, dk) && dk == key) {
      if (eq == std::string_view::npos) {
        out.clear();
        return true;
      }
      return url_decode(kv.substr(eq + 1), out);
    }
    p = e + 1;
  }
  return false;
}

const char* reason_phrase(int status) {
  switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "Status";
  }
}

std::string format_response(const Response& r) {
  std::string s = "HTTP/1.1 " + std::to_string(r.status) + " " + reason_phrase(r.status) + "\r\nContent-Type: " + r.content_type +
                  "\r\nContent-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n";
  s += r.body;
  return s;
}

// ---- server ---------------------------------------------------------------------------------------------------------

namespace {

using SteadyClock = std::chrono::steady_clock;

int ms_left(SteadyClock::time_point end) {
  auto d = std::chrono::duration_cast<std::chrono::milliseconds>(end - SteadyClock::now()).count();
  return d < 0 ? 0 : int(d);
}

// Sends everything or gives up at `end`.
bool send_all(int fd, const char* p, size_t n, SteadyClock::time_point end) {
  while (n > 0) {
    ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
    if (w > 0) {
      p += w;
      n -= size_t(w);
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      int left = ms_left(end);
      if (left <= 0) return false;
      pollfd pf{fd, POLLOUT, 0};
      ::poll(&pf, 1, left);
      continue;
    }
    return false;
  }
  return true;
}

Response error_response(int status, const char* msg) {
  Response r;
  r.status = status;
  r.content_type = "application/json";
  r.body = std::string("{\"error\":\"") + msg + "\"}";
  return r;
}

}  // namespace

bool Server::start(uint16_t port, Handler handler, std::string& err, const std::string& addr, Limits limits) {
  if (th_.joinable()) {
    err = "already running";
    return false;
  }
  handler_ = std::move(handler);
  limits_ = limits;
  lfd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (lfd_ < 0) {
    err = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  int one = 1;
  ::setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (::inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) {
    err = "bad address " + addr;
    ::close(lfd_);
    lfd_ = -1;
    return false;
  }
  if (::bind(lfd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || ::listen(lfd_, 8) != 0) {
    err = std::string("bind/listen ") + addr + ":" + std::to_string(port) + ": " + std::strerror(errno);
    ::close(lfd_);
    lfd_ = -1;
    return false;
  }
  socklen_t sl = sizeof sa;
  if (::getsockname(lfd_, reinterpret_cast<sockaddr*>(&sa), &sl) == 0) port_ = ntohs(sa.sin_port);
  efd_ = ::eventfd(0, EFD_CLOEXEC);
  if (efd_ < 0) {
    err = std::string("eventfd: ") + std::strerror(errno);
    ::close(lfd_);
    lfd_ = -1;
    return false;
  }
  th_ = std::thread([this] { loop(); });
  return true;
}

void Server::stop() {
  if (!th_.joinable()) return;
  uint64_t v = 1;
  if (::write(efd_, &v, sizeof v) < 0) {
  }
  th_.join();
  ::close(lfd_);
  ::close(efd_);
  lfd_ = efd_ = -1;
}

void Server::loop() {
  // Signals (SIGHUP, SIGTERM) belong to the main thread: it must be the one interrupted.
  sigset_t all;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, nullptr);
  pollfd p[2] = {{lfd_, POLLIN, 0}, {efd_, POLLIN, 0}};
  for (;;) {
    int r = ::poll(p, 2, -1);  // sleeps for ever: no wake-ups without a client
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (p[1].revents) break;
    if (p[0].revents & POLLIN) {
      int c = ::accept4(lfd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (c >= 0) {
        serve(c);
        ::close(c);
      }
    }
  }
}

void Server::serve(int fd) {
  SteadyClock::time_point end = SteadyClock::now() + std::chrono::milliseconds(limits_.read_deadline_ms);
  std::string buf;
  buf.reserve(1024);
  Request req;
  Parse st = Parse::NeedMore;
  bool sent100 = false;
  bool timed_out = false;
  for (;;) {
    st = parse_request(buf, req);
    if (st != Parse::NeedMore) break;
    if (req.expect_continue && !sent100) {
      sent100 = true;
      static const char k100[] = "HTTP/1.1 100 Continue\r\n\r\n";
      send_all(fd, k100, sizeof k100 - 1, end);
    }
    int left = ms_left(end);
    if (left <= 0) {
      timed_out = true;
      break;
    }
    pollfd pf{fd, POLLIN, 0};
    int r = ::poll(&pf, 1, left);
    if (r < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (r == 0) continue;  // the deadline check above ends it
    char tmp[2048];
    ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
    if (n == 0) return;  // the client left without finishing: nothing to answer
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      return;
    }
    buf.append(tmp, size_t(n));
  }
  Response resp;
  if (timed_out) resp = error_response(408, "request timeout");
  else if (st == Parse::Bad) resp = error_response(400, "bad request");
  else if (st == Parse::TooLarge) resp = error_response(413, "request too large");
  else if (st == Parse::NotImplemented) resp = error_response(501, "not implemented");
  else resp = handler_ ? handler_(req) : error_response(404, "not found");
  std::string out = format_response(resp);
  send_all(fd, out.data(), out.size(), SteadyClock::now() + std::chrono::milliseconds(2000));
  // Close politely: if we answered before reading everything, closing with unread data would send an RST and the client
  // could lose the answer. Half-close, then swallow what is still coming for a moment.
  ::shutdown(fd, SHUT_WR);
  SteadyClock::time_point drain_end = SteadyClock::now() + std::chrono::milliseconds(150);
  char tmp[2048];
  for (;;) {
    int left = ms_left(drain_end);
    if (left <= 0) break;
    pollfd pf{fd, POLLIN, 0};
    int r = ::poll(&pf, 1, left);
    if (r <= 0) break;
    ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) break;
  }
}

}  // namespace http
}  // namespace zf
