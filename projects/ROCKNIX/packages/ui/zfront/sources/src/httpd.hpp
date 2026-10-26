// MODULE `httpd` (os-contract): a tiny, strict HTTP/1.1 server for the local control API (127.0.0.1 only).
//
// Not a general web server: GET/POST, no keep-alive, no chunked bodies, no TLS. One thread, blocked in poll(2) on the
// listening socket and an eventfd (zero wake-ups while idle), one connection at a time with a short read deadline, so a
// stuck client cannot wedge it for long. `parse_request` is pure and has the limits; the handler runs on the server thread
// (it must be thread-safe and quick: anything that touches the UI goes through `Jobs::post`).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace zf {
namespace http {

constexpr size_t kMaxHeaderBytes = 8192;   // request line + headers
constexpr size_t kMaxBodyBytes = 65536;    // Content-Length above this is refused (413)
constexpr size_t kMaxTargetBytes = 2048;

struct Request {
  std::string method;  // "GET" / "POST"
  std::string path;    // without query, as sent (never percent-decoded)
  std::string query;   // after '?', raw
  std::string body;
  bool expect_continue = false;  // "Expect: 100-continue" seen and the body is not complete yet
};

struct Response {
  int status = 200;
  std::string content_type = "text/plain";
  std::string body;
};

enum class Parse { NeedMore, Ok, Bad, TooLarge, NotImplemented };

/// Parses one request from `buf`. `Ok`: `req` is complete. `NeedMore`: read more (`req.expect_continue` tells the
/// server to send "100 Continue"). Bad = 400, TooLarge = 413/431, NotImplemented = 501 (chunked, unknown method).
/// Never throws, never reads past `buf`.
Parse parse_request(std::string_view buf, Request& req);

/// Value of `key` in the query string, percent-decoded ('+' is a space). False when absent or badly encoded.
bool query_param(const Request& req, std::string_view key, std::string& out);
/// Percent-decoding; false on a bad escape.
bool url_decode(std::string_view in, std::string& out);

const char* reason_phrase(int status);
/// Full response with `Content-Length` and `Connection: close`.
std::string format_response(const Response& r);

struct ServerLimits {
  int read_deadline_ms = 2000;  // total time a client has to send its request
};

class Server {
 public:
  using Handler = std::function<Response(const Request&)>;
  using Limits = ServerLimits;
  Server() = default;
  ~Server() { stop(); }
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  /// Binds `addr:port` (port 0 = any, see `port()`), starts the thread. False with `err` filled when it cannot listen
  /// (e.g. the port is taken). SO_REUSEADDR, every descriptor CLOEXEC (an emulator must not inherit the port).
  bool start(uint16_t port, Handler handler, std::string& err, const std::string& addr = "127.0.0.1", Limits limits = Limits());
  uint16_t port() const { return port_; }
  bool running() const { return th_.joinable(); }
  /// Wakes the thread and joins it; safe to call twice.
  void stop();

 private:
  void loop();
  void serve(int fd);
  int lfd_ = -1, efd_ = -1;
  uint16_t port_ = 0;
  Handler handler_;
  Limits limits_;
  std::thread th_;
};

}  // namespace http
}  // namespace zf
