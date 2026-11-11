#include "httpd.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <vector>
#include <atomic>
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

Parse parse_request(std::string_view buf, Request& req, const StreamPred& stream) {
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
  const std::string_view tpath = target.substr(0, target.find('?'));
  const bool stream_route = stream ? stream(method, tpath) : (method == "POST" && tpath == kStreamPath);

  // ---- headers
  bool have_len = false, expect = false;
  size_t clen = 0;
  std::vector<std::pair<std::string, std::string>> hdrs;
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
      if (value.empty() || value.size() > 12) return Parse::Bad;
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
    std::string lname(name);
    for (char& c : lname)
      if (c >= 'A' && c <= 'Z') c = char(c + 32);
    hdrs.emplace_back(std::move(lname), std::string(value));
  }
  if (stream_route) {
    req.method.assign(method);
    size_t q0 = target.find('?');
    req.path.assign(target.substr(0, q0));
    req.query.assign(q0 == std::string_view::npos ? std::string_view() : target.substr(q0 + 1));
    std::string_view rest = buf.substr(he + 4);
    req.body.assign(rest.substr(0, std::min<size_t>(rest.size(), clen)));
    req.content_length = clen;
    req.expect_continue = expect;
    req.headers = std::move(hdrs);
    return Parse::Stream;
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
  req.content_length = clen;
  req.headers = std::move(hdrs);
  req.expect_continue = false;
  return Parse::Ok;
}

bool header_value(const Request& req, std::string_view name, std::string& out) {
  for (const auto& h : req.headers)
    if (h.first == name) {
      out = h.second;
      return true;
    }
  return false;
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
    case 302: return "Found";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 429: return "Too Many Requests";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "Status";
  }
}

std::string format_head(const Response& r, uint64_t len) {
  std::string s = "HTTP/1.1 " + std::to_string(r.status) + " " + reason_phrase(r.status) + "\r\nContent-Type: " + r.content_type +
                  "\r\nContent-Length: " + std::to_string(len) + "\r\n";
  bool cache = false;
  for (const auto& h : r.headers) {
    if (h.first.empty() || h.first.find_first_of("\r\n: ") != std::string::npos || h.second.find_first_of("\r\n") != std::string::npos) continue;
    if (ieq(h.first, "cache-control")) cache = true;
    s += h.first + ": " + h.second + "\r\n";
  }
  // what the answers carry: a browser must not guess another type for them, and nothing of the PIN-protected API (games, pictures, saves)
  // stays in a phone's cache for the next person to pick up the phone
  s += "X-Content-Type-Options: nosniff\r\n";
  if (!cache) s += "Cache-Control: no-store\r\n";
  return s + "Connection: close\r\n\r\n";
}

std::string format_response(const Response& r) { return format_head(r, r.body.size()) + r.body; }

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

/// The worker answers before it lets go of `busy` (a few instructions, or the scheduler's whim on a loaded machine): a client
/// that sends its next transfer the moment the last one ended must not get 409 for that. Waits at most 200 ms.
bool still_busy(const std::atomic<bool>& busy) {
  for (int i = 0; i < 40 && busy.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return busy.load();
}

/// 409 while the transfer worker is taken: `busy` lets a client wait and retry without reading the message.
Response busy_response(const char* msg) {
  Response r = error_response(409, msg);
  r.body = std::string("{\"error\":\"") + msg + "\",\"busy\":true}";
  return r;
}

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch()).count();
}

// Sends the answer and closes politely: if we answered before reading everything, closing with unread data would send an
// RST and the client could lose the answer. Half-close, then swallow what is still coming for a moment.
// `answered`: set as soon as the answer is out, before the drain (an upload worker is free for the next file at that point: the next
// request of a phone sending several files came within those 150 ms and got 409).
// Half-close and read what the client still sends for a moment, so the answer is not lost to a reset.
void drain(int fd) {
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

void finish_connection(int fd, const Response& resp, std::atomic<bool>* answered = nullptr) {
  std::string out = format_response(resp);
  send_all(fd, out.data(), out.size(), SteadyClock::now() + std::chrono::milliseconds(2000));
  if (answered) *answered = false;
  drain(fd);
}

}  // namespace

BodyStream::BodyStream(int fd, std::string prefix, uint64_t total, bool expect_continue, int idle_ms, int total_ms, const std::atomic<bool>* stop)
    : fd_(fd), prefix_(std::move(prefix)), total_(total), idle_ms_(idle_ms), end_ms_(now_ms() + total_ms), stop_(stop) {
  if (prefix_.size() > total_) prefix_.resize(size_t(total_));
  expect_ = expect_continue && prefix_.size() < total_;
  got_ = 0;
}

void BodyStream::accept() {
  if (!expect_) return;
  expect_ = false;
  static const char k100[] = "HTTP/1.1 100 Continue\r\n\r\n";
  send_all(fd_, k100, sizeof k100 - 1, SteadyClock::now() + std::chrono::milliseconds(2000));
}

ptrdiff_t BodyStream::read(char* buf, size_t cap) {
  if (got_ >= total_) return 0;
  if (cap == 0) return -1;
  if (prefix_pos_ < prefix_.size()) {
    size_t n = std::min(cap, prefix_.size() - prefix_pos_);
    std::memcpy(buf, prefix_.data() + prefix_pos_, n);
    prefix_pos_ += n;
    got_ += n;
    return ptrdiff_t(n);
  }
  long long silent_until = now_ms() + idle_ms_;
  for (;;) {
    if (stop_ && stop_->load()) return -1;
    long long t = now_ms();
    if (t >= end_ms_ || t >= silent_until) {
      timed_out_ = true;
      return -1;
    }
    int slice = int(std::min<long long>({silent_until - t, end_ms_ - t, 200}));
    pollfd pf{fd_, POLLIN, 0};
    int r = ::poll(&pf, 1, slice);
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (r == 0) continue;
    size_t want = size_t(std::min<uint64_t>(cap, total_ - got_));
    ssize_t n = ::recv(fd_, buf, want, 0);
    if (n > 0) {
      got_ += uint64_t(n);
      return ptrdiff_t(n);
    }
    if (n == 0) return -1;  // the client left before the end
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
    return -1;
  }
}

bool Server::start(uint16_t port, Handler handler, std::string& err, const std::string& addr, Limits limits) {
  if (th_.joinable()) {
    err = "already running";
    return false;
  }
  handler_ = std::move(handler);
  limits_ = limits;
  stopping_ = false;
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
  stopping_ = true;  // a streamed upload in progress notices within 200 ms
  uint64_t v = 1;
  if (::write(efd_, &v, sizeof v) < 0) {
  }
  th_.join();
  if (worker_.joinable()) worker_.join();
  ::close(lfd_);
  ::close(efd_);
  lfd_ = efd_ = -1;
}

namespace {

/// A client that connected and has not finished its request. The loop reads from all of them at once, so a phone that opens a
/// connection and says nothing (browsers open spare ones ahead of time) never holds up the one that does.
struct Conn {
  int fd = -1;
  std::string buf;
  Request req;
  SteadyClock::time_point end;
  bool sent100 = false;
};

constexpr size_t kMaxPending = 32;  // more than that at once: the newest are refused (a phone never needs so many)

}  // namespace

void Server::loop() {
  // Signals (SIGHUP, SIGTERM) belong to the main thread: it must be the one interrupted.
  sigset_t all;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, nullptr);
  std::vector<Conn> conns;
  std::vector<pollfd> p;
  for (;;) {
    p.assign({{lfd_, POLLIN, 0}, {efd_, POLLIN, 0}});
    int wait = -1;  // sleeps for ever while nobody is connected: no wake-ups without a client
    for (const Conn& c : conns) {
      p.push_back({c.fd, POLLIN, 0});
      const int left = ms_left(c.end);
      wait = wait < 0 ? left : std::min(wait, left);
    }
    int r = ::poll(p.data(), p.size(), wait);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (p[1].revents) break;
    // the clients already connected: read what came, answer the ones that are complete or out of time
    std::vector<Conn> keep;
    const size_t known = conns.size();
    for (size_t i = 0; i < known; ++i) {
      Conn& c = conns[i];
      bool drop = false, ready = false, timed_out = false;
      Parse st = Parse::NeedMore;
      if (p[i + 2].revents & (POLLIN | POLLHUP | POLLERR)) {
        for (int rounds = 0; rounds < 40 && !drop && !ready; ++rounds) {  // everything that is there now: a long request needs no more wake-ups
          char tmp[2048];
          const ssize_t n = ::recv(c.fd, tmp, sizeof tmp, 0);
          if (n == 0) {
            drop = true;  // the client left without finishing: nothing to answer
          } else if (n < 0) {
            drop = !(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
            break;  // nothing more for now
          } else {
            c.buf.append(tmp, size_t(n));
            st = parse_request(c.buf, c.req, stream_pred_);
            ready = st != Parse::NeedMore;
            if (!ready && c.req.expect_continue && !c.sent100) {
              c.sent100 = true;
              static const char k100[] = "HTTP/1.1 100 Continue\r\n\r\n";
              send_all(c.fd, k100, sizeof k100 - 1, c.end);
            }
          }
        }
      }
      if (!drop && !ready && ms_left(c.end) <= 0) ready = timed_out = true;
      if (drop) {
        ::close(c.fd);
      } else if (ready) {
        {  // who is asking: the control API trusts only the machine itself
          sockaddr_in pa;
          socklen_t pl = sizeof pa;
          c.req.local = ::getpeername(c.fd, reinterpret_cast<sockaddr*>(&pa), &pl) == 0 && pa.sin_family == AF_INET && (ntohl(pa.sin_addr.s_addr) >> 24) == 127;
        }
        if (!dispatch(c.fd, st, c.req, timed_out)) ::close(c.fd);
      } else {
        keep.push_back(std::move(c));
      }
    }
    conns = std::move(keep);
    if (p[0].revents & POLLIN) {
      for (;;) {
        int fd = ::accept4(lfd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) break;
        if (conns.size() >= kMaxPending) {
          ::close(fd);
          continue;
        }
        Conn c;
        c.fd = fd;
        c.buf.reserve(1024);
        c.end = SteadyClock::now() + std::chrono::milliseconds(limits_.read_deadline_ms);
        conns.push_back(std::move(c));
      }
    }
  }
  for (const Conn& c : conns) ::close(c.fd);
}

bool Server::dispatch(int fd, Parse st, Request& req, bool timed_out) {
  Response resp;
  if (st == Parse::Stream && !timed_out) {
    if (!stream_handler_) {
      resp = error_response(404, "not found");
    } else if (still_busy(worker_busy_)) {
      resp = busy_response("another upload is running");
    } else {
      if (worker_.joinable()) worker_.join();  // the previous one has finished (worker_busy_ is false)
      worker_busy_ = true;
      worker_ = std::thread([this, fd, r = std::move(req)]() mutable {
        run_stream(fd, std::move(r));
        ::close(fd);
        worker_busy_ = false;
      });
      return true;
    }
  } else if (timed_out) {
    resp = error_response(408, "request timeout");
  } else if (st == Parse::Bad) {
    resp = error_response(400, "bad request");
  } else if (st == Parse::TooLarge) {
    resp = error_response(413, "request too large");
  } else if (st == Parse::NotImplemented) {
    resp = error_response(501, "not implemented");
  } else {
    resp = handler_ ? handler_(req) : error_response(404, "not found");  // built with -fno-exceptions: handlers never throw
    if (!resp.file.empty() && start_download(fd, resp)) return true;
  }
  finish_connection(fd, resp);
  return false;
}

bool Server::start_download(int fd, Response& resp) {
  int ffd = ::open(resp.file.c_str(), O_RDONLY | O_CLOEXEC);
  struct stat sb;
  if (ffd < 0 || ::fstat(ffd, &sb) != 0 || !S_ISREG(sb.st_mode)) {
    if (ffd >= 0) ::close(ffd);
    resp = error_response(404, "not found");
    return false;
  }
  if (still_busy(worker_busy_)) {
    ::close(ffd);
    resp = busy_response("another transfer is running");
    return false;
  }
  if (worker_.joinable()) worker_.join();  // the previous one has finished (worker_busy_ is false)
  worker_busy_ = true;
  worker_ = std::thread([this, fd, ffd, size = uint64_t(sb.st_size), r = std::move(resp)] {
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, nullptr);
    const std::string head = format_head(r, size);
    const SteadyClock::time_point total_end = SteadyClock::now() + std::chrono::milliseconds(limits_.stream_total_ms);
    bool ok = send_all(fd, head.data(), head.size(), SteadyClock::now() + std::chrono::milliseconds(limits_.stream_idle_ms));
    std::vector<char> buf(64 * 1024);
    uint64_t left = size;
    while (ok && left > 0 && !stopping_.load()) {
      ssize_t n = ::read(ffd, buf.data(), size_t(std::min<uint64_t>(left, buf.size())));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;  // the file shrank meanwhile: the client gets a short body and knows it
      ok = send_all(fd, buf.data(), size_t(n), std::min(total_end, SteadyClock::now() + std::chrono::milliseconds(limits_.stream_idle_ms)));
      left -= uint64_t(n);
    }
    ::close(ffd);
    worker_busy_ = false;  // the answer is out: the next transfer may start (as finish_connection does for uploads)
    drain(fd);
    ::close(fd);
  });
  return true;
}

void Server::run_stream(int fd, Request req) {
  sigset_t all;
  sigfillset(&all);
  pthread_sigmask(SIG_BLOCK, &all, nullptr);
  BodyStream body(fd, std::move(req.body), req.content_length, req.expect_continue, limits_.stream_idle_ms, limits_.stream_total_ms, &stopping_);
  Response resp = stream_handler_(req, body);
  finish_connection(fd, resp, &worker_busy_);
}

}  // namespace http
}  // namespace zf
