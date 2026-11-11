// MODULE `httpd` (os-contract): a tiny, strict HTTP/1.1 server for the local control API (the owner picks the address; `Request::local` says whether a client is on 127.0.0.0/8, and the control routes refuse the rest).
//
// Not a general web server: GET/POST, no keep-alive, no chunked bodies, no TLS. One thread, blocked in poll(2) on the
// listening socket and an eventfd (zero wake-ups while idle), one connection at a time with a short read deadline, so a
// stuck client cannot wedge it for long. `parse_request` is pure and has the limits; the handler runs on the server thread
// (it must be thread-safe and quick: anything that touches the UI goes through `Jobs::post`).
//
// One exception to "small bodies": the requests the stream predicate picks (default `POST kStreamPath`, the web file transfer)
// may carry a body of any size. `parse_request` stops at the headers (`Parse::Stream`) and the connection goes to a separate
// worker thread (one at a time, a second one gets 409) that gives the handler a `BodyStream` to read the body piece by piece.
// Idle and total deadlines apply there. Downloads (`Response::file`) use the same worker: a big file is never held in memory
// and never blocks the server thread.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace zf {
namespace http {

constexpr size_t kMaxHeaderBytes = 8192;   // request line + headers
constexpr size_t kMaxBodyBytes = 65536;    // Content-Length above this is refused (413)
constexpr size_t kMaxTargetBytes = 2048;
constexpr const char* kStreamPath = "/files/upload";  // streamed by the default predicate
/// Which requests carry a streamed body: (method, path without the query). Empty = `POST kStreamPath` only.
using StreamPred = std::function<bool(std::string_view method, std::string_view path)>;

struct Request {
  std::string method;  // "GET" / "POST"
  std::string path;    // without query, as sent (never percent-decoded)
  std::string query;   // after '?', raw
  std::string body;
  bool expect_continue = false;  // "Expect: 100-continue" seen and the body is not complete yet
  bool local = true;             // the peer is on 127.0.0.0/8 (set by the server; parse_request leaves it alone)
  uint64_t content_length = 0;   // as announced (for Parse::Stream, `body` only holds what came with the headers)
  std::vector<std::pair<std::string, std::string>> headers;  // lower-case names, trimmed values
};

/// Value of header `name` (lower case). False when absent.
bool header_value(const Request& req, std::string_view name, std::string& out);

struct Response {
  int status = 200;
  std::string content_type = "text/plain";
  std::string body;
  /// Extra headers (Content-Disposition, Cache-Control...); one whose name or value holds CR or LF is dropped.
  std::vector<std::pair<std::string, std::string>> headers;
  /// When set, the body is this regular file (`body` is ignored), sent from the worker thread in pieces: downloads. A file that
  /// cannot be opened answers 404; while the worker is busy, 409.
  std::string file;
};

enum class Parse { NeedMore, Ok, Bad, TooLarge, NotImplemented, Stream };

/// Parses one request from `buf`. `Ok`: `req` is complete. `NeedMore`: read more (`req.expect_continue` tells the
/// server to send "100 Continue"). Bad = 400, TooLarge = 413/431, NotImplemented = 501 (chunked, unknown method).
/// Never throws, never reads past `buf`.
/// `Stream`: a request `stream` picks (empty: `POST kStreamPath`), headers complete; `req.body` holds the body bytes already
/// received, the rest is still on the socket.
Parse parse_request(std::string_view buf, Request& req, const StreamPred& stream = StreamPred());

/// Value of `key` in the query string, percent-decoded ('+' is a space). False when absent or badly encoded.
bool query_param(const Request& req, std::string_view key, std::string& out);
/// Percent-decoding; false on a bad escape.
bool url_decode(std::string_view in, std::string& out);

const char* reason_phrase(int status);
/// Full response with `Content-Length` and `Connection: close`.
std::string format_response(const Response& r);
/// Status line and headers only, for a body of `len` bytes sent separately (`Response::file`).
std::string format_head(const Response& r, uint64_t len);

struct ServerLimits {
  int read_deadline_ms = 2000;            // total time a client has to send its request
  int stream_idle_ms = 15000;             // streamed body: longest silence from the client
  int stream_total_ms = 2 * 3600 * 1000;  // streamed body: longest whole transfer
};

/// The body of a streamed request, read from the socket on demand. Only used on the stream worker thread.
class BodyStream {
 public:
  BodyStream(int fd, std::string prefix, uint64_t total, bool expect_continue, int idle_ms, int total_ms, const std::atomic<bool>* stop);
  uint64_t total() const { return total_; }
  uint64_t remaining() const { return total_ - got_; }
  /// Sends "100 Continue" when the client asked for it (once). Call it after the checks that may refuse the upload.
  void accept();
  /// Up to `cap` bytes: > 0 bytes read, 0 the body is complete, -1 failed (client gone, silent too long, too slow, server stopping).
  ptrdiff_t read(char* buf, size_t cap);
  bool timed_out() const { return timed_out_; }

 private:
  int fd_;
  std::string prefix_;
  size_t prefix_pos_ = 0;
  uint64_t total_, got_ = 0;
  bool expect_ = false, timed_out_ = false;
  int idle_ms_;
  long long end_ms_;
  const std::atomic<bool>* stop_;
};

class Server {
 public:
  using Handler = std::function<Response(const Request&)>;
  using StreamHandler = std::function<Response(const Request&, BodyStream&)>;
  using Limits = ServerLimits;
  Server() = default;
  ~Server() { stop(); }
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  /// Binds `addr:port` (port 0 = any, see `port()`), starts the thread. False with `err` filled when it cannot listen
  /// (e.g. the port is taken). SO_REUSEADDR, every descriptor CLOEXEC (an emulator must not inherit the port).
  bool start(uint16_t port, Handler handler, std::string& err, const std::string& addr = "127.0.0.1", Limits limits = Limits());
  /// Handler for `Parse::Stream` requests and the predicate that picks them (empty: `POST kStreamPath`); set them before
  /// `start`. Without a handler such requests get 404.
  void set_stream_handler(StreamHandler h, StreamPred pred = StreamPred()) {
    stream_handler_ = std::move(h);
    stream_pred_ = std::move(pred);
  }
  uint16_t port() const { return port_; }
  bool running() const { return th_.joinable(); }
  /// Wakes the thread and joins it; safe to call twice.
  void stop();

 private:
  void loop();
  /// The request on `fd` is complete (`st`) or the client ran out of time (`timed_out`): answers it. True: the connection went to a
  /// worker, which closes it; false: answered, the caller closes `fd`.
  bool dispatch(int fd, Parse st, Request& req, bool timed_out);
  void run_stream(int fd, Request req);
  bool start_download(int fd, Response& resp);  // true: the worker owns `fd` now; false: `resp` is the answer to send
  int lfd_ = -1, efd_ = -1;
  uint16_t port_ = 0;
  Handler handler_;
  StreamHandler stream_handler_;
  StreamPred stream_pred_;
  Limits limits_;
  std::thread th_;
  std::thread worker_;  // the one streamed upload or download at a time
  std::atomic<bool> worker_busy_{false};
  std::atomic<bool> stopping_{false};
};

}  // namespace http
}  // namespace zf
