#include "webpair.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>

#include "app.hpp"
#include "i18n.hpp"
#include "menu.hpp"
#include "ui_dialogs.hpp"
#include "webcore.hpp"

namespace zf {
namespace webpair {

namespace {

constexpr const char* kDefaultPath = "/storage/.config/zfront/webdevices";
constexpr size_t kTokenHex = 64;
constexpr size_t kIdLen = 12;

struct Rec {
  std::string token, name;
  long long paired = 0, seen = 0;
};

struct Store {
  std::mutex m;  // everything below
  bool loaded = false;
  std::vector<Rec> recs;
  std::string path;
  std::function<long long()> clock;
  int timeout_ms = kAskTimeoutMs;
};

Store& store() {
  static Store s;
  return s;
}

// the question on the console: one at a time
struct Ask {
  std::mutex m;
  std::condition_variable cv;
  bool pending = false, done = false, yes = false;
  unsigned gen = 0;
  Asker asker;
};

Ask& ask() {
  static Ask a;
  return a;
}

long long now_s(const Store& s) { return s.clock ? s.clock() : static_cast<long long>(::time(nullptr)); }

bool is_token(const std::string& t) {
  if (t.size() != kTokenHex) return false;
  for (char c : t)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

std::string hex(const unsigned char* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string o;
  for (size_t i = 0; i < n; ++i) {
    o += d[p[i] >> 4];
    o += d[p[i] & 15];
  }
  return o;
}

std::string make_token() {
  unsigned char b[32];
  size_t got = 0;
  while (got < sizeof b) {
    const ssize_t n = ::getrandom(b + got, sizeof b - got, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return {};
    }
    got += size_t(n);
  }
  return hex(b, sizeof b);
}

std::string resolve_path() {
  const char* e = std::getenv("ZF_WEBDEVICES");
  return e && *e ? e : kDefaultPath;
}

// Lines `token<TAB>name<TAB>paired<TAB>last_seen`; a line that does not parse is skipped.
void load_locked(Store& s) {
  if (s.loaded) return;
  s.loaded = true;
  s.recs.clear();
  s.path = resolve_path();
  FILE* f = std::fopen(s.path.c_str(), "rb");
  if (!f) return;
  std::string t;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && t.size() < (1u << 20)) t.append(buf, n);
  std::fclose(f);
  size_t at = 0;
  while (at < t.size()) {
    size_t e = t.find('\n', at);
    if (e == std::string::npos) e = t.size();
    const std::string line = t.substr(at, e - at);
    at = e + 1;
    std::vector<std::string> f4;
    size_t p = 0;
    for (;;) {
      const size_t q = line.find('\t', p);
      if (q == std::string::npos || f4.size() >= 3) {
        f4.push_back(line.substr(p));
        break;
      }
      f4.push_back(line.substr(p, q - p));
      p = q + 1;
    }
    if (f4.size() != 4 || !is_token(f4[0])) continue;
    Rec r;
    r.token = f4[0];
    r.name = sanitize_name(f4[1]);
    r.paired = std::atoll(f4[2].c_str());
    r.seen = std::atoll(f4[3].c_str());
    s.recs.push_back(std::move(r));
    if (s.recs.size() >= kMaxDevices) break;
  }
}

// Written next to the file and renamed over it, so a power cut leaves the old list or the new one. Mode 0600.
bool save_locked(Store& s) {
  std::string body;
  for (const Rec& r : s.recs) body += r.token + "\t" + r.name + "\t" + std::to_string(r.paired) + "\t" + std::to_string(r.seen) + "\n";
  const size_t slash = s.path.rfind('/');
  if (slash != std::string::npos && slash > 0) ::mkdir(s.path.substr(0, slash).c_str(), 0755);  // one level; the others exist on the console
  const std::string tmp = s.path + ".tmp" + std::to_string(::getpid());
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = ::fchmod(fd, 0600) == 0;
  size_t off = 0;
  while (ok && off < body.size()) {
    const ssize_t n = ::write(fd, body.data() + off, body.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) ok = false;
    else off += size_t(n);
  }
  ok = ok && ::fsync(fd) == 0;
  ok = ::close(fd) == 0 && ok;
  if (!ok || ::rename(tmp.c_str(), s.path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

std::string add_device(const std::string& name) {
  const std::string token = make_token();
  if (token.empty()) return {};
  Store& s = store();
  std::lock_guard<std::mutex> lk(s.m);
  load_locked(s);
  while (s.recs.size() >= kMaxDevices) {  // the oldest paired goes
    size_t oldest = 0;
    for (size_t i = 1; i < s.recs.size(); ++i)
      if (s.recs[i].paired < s.recs[oldest].paired) oldest = i;
    s.recs.erase(s.recs.begin() + std::ptrdiff_t(oldest));
  }
  Rec r;
  r.token = token;
  r.name = name;
  r.paired = r.seen = now_s(s);
  s.recs.push_back(std::move(r));
  save_locked(s);  // when the disk refuses, the device still works until the next start
  return token;
}

// ---- the question on the console -----------------------------------------------------------------------------------------

/// ALLOW THIS DEVICE? with No selected. Closing it in any way but Yes is a No.
class PairDialog : public ui::ConfirmDialog {
 public:
  PairDialog(const std::string& name, unsigned gen, std::shared_ptr<bool> said, ui::ConfirmOpts o)
      : ui::ConfirmDialog(name,
                          [gen, said](Ctx& cx) {
                            *said = true;
                            if (answer(gen, true)) cx.sv->toasts.push(tr("Device paired"));
                            return Transition::none();
                          },
                          std::move(o), false),
        gen_(gen),
        said_(std::move(said)) {}
  ~PairDialog() override {
    if (!*said_) answer(gen_, false);
  }

 private:
  unsigned gen_;
  std::shared_ptr<bool> said_;
};

bool ask_on_console(const std::string& name, unsigned gen, const webtransfer::Env& env) {
  if (!env.on_main) return false;
  env.on_main([name, gen](App& app) {
    ui::ConfirmOpts o;
    o.title = tr("ALLOW THIS DEVICE?");
    app.push_screen(std::make_unique<PairDialog>(name, gen, std::make_shared<bool>(false), std::move(o)));
  });
  return true;
}

// ---- POST /files/pair ----------------------------------------------------------------------------------------------------

http::Response pair_request(const http::Request& req, http::BodyStream& body, const webtransfer::Env& env) {
  using webtransfer::err;
  using webtransfer::reply;
  http::Response deny;
  if (!webtransfer::authorize_pin(req, deny)) return deny;  // the PIN, with the lock-out of every other route
  if (body.total() > 1024) return err(413, "too long");
  body.accept();
  std::string raw;
  char buf[256];
  while (raw.size() < 1024) {
    const ptrdiff_t k = body.read(buf, sizeof buf);
    if (k <= 0) break;
    raw.append(buf, size_t(k));
  }
  const std::string name = sanitize_name(raw);
  Ask& a = ask();
  unsigned gen = 0;
  Asker asker;
  int timeout_ms = kAskTimeoutMs;
  {
    std::lock_guard<std::mutex> lk(a.m);
    if (a.pending) return err(409, "another device is asking");
    a.pending = true, a.done = false, a.yes = false;
    gen = ++a.gen;
    asker = a.asker;
  }
  {
    Store& s = store();
    std::lock_guard<std::mutex> lk(s.m);
    timeout_ms = s.timeout_ms;
  }
  const bool asked = asker ? asker(name, gen, env) : ask_on_console(name, gen, env);
  bool yes = false;
  {
    std::unique_lock<std::mutex> lk(a.m);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(asked ? timeout_ms : 0);
    while (asked && !a.done && std::chrono::steady_clock::now() < end && webtransfer::enabled())
      a.cv.wait_for(lk, std::chrono::milliseconds(100));
    yes = a.done && a.yes;
    a.pending = false;  // a late answer finds no open question
  }
  if (!asked) return err(503, "the console cannot ask now");
  if (!yes) return err(403, "not allowed");
  const std::string token = add_device(name);
  if (token.empty()) return err(500, "could not make a token");
  return reply(200, "{\"token\":\"" + token + "\"}");
}

}  // namespace

// ---- public ------------------------------------------------------------------------------------------------------------

std::string sanitize_name(std::string_view raw) {
  std::string o;
  for (char ch : raw) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c >= 0x20 && c < 0x7f) o += ch;
    else if (c == '\t' || c == '\n' || c == '\r') o += ' ';
  }
  size_t b = o.find_first_not_of(' ');
  if (b == std::string::npos) return "Unknown device";
  o = o.substr(b, o.find_last_not_of(' ') - b + 1);
  if (o.size() > kMaxNameChars) {
    o.resize(kMaxNameChars);
    const size_t e = o.find_last_not_of(' ');
    o.resize(e + 1);
  }
  return o;
}

std::string devices_path() {
  Store& s = store();
  std::lock_guard<std::mutex> lk(s.m);
  load_locked(s);
  return s.path;
}

bool token_valid(std::string_view token) {
  if (token.size() != kTokenHex) return false;
  Store& s = store();
  std::lock_guard<std::mutex> lk(s.m);
  load_locked(s);
  unsigned found = 0;
  size_t hit = 0;
  for (size_t i = 0; i < s.recs.size(); ++i) {  // every device and every digit, whatever matched: the time says nothing
    unsigned diff = 0;
    for (size_t k = 0; k < kTokenHex; ++k) diff |= unsigned(static_cast<unsigned char>(s.recs[i].token[k]) ^ static_cast<unsigned char>(token[k]));
    if (diff == 0) found = 1, hit = i;
  }
  if (!found) return false;
  const long long t = now_s(s);
  if (t - s.recs[hit].seen >= kSeenGapSeconds) {  // not on every request: the disk is a card
    s.recs[hit].seen = t;
    save_locked(s);
  }
  return true;
}

std::vector<Device> devices() {
  Store& s = store();
  std::lock_guard<std::mutex> lk(s.m);
  load_locked(s);
  std::vector<Device> v;
  for (const Rec& r : s.recs) v.push_back(Device{r.token.substr(0, kIdLen), r.name, r.paired, r.seen});
  return v;
}

bool forget(const std::string& id) {
  Store& s = store();
  std::lock_guard<std::mutex> lk(s.m);
  load_locked(s);
  for (size_t i = 0; i < s.recs.size(); ++i)
    if (s.recs[i].token.compare(0, kIdLen, id) == 0 && id.size() == kIdLen) {
      s.recs.erase(s.recs.begin() + std::ptrdiff_t(i));
      save_locked(s);
      return true;
    }
  return false;
}

void forget_all() {
  Store& s = store();
  std::lock_guard<std::mutex> lk(s.m);
  load_locked(s);
  s.recs.clear();
  save_locked(s);
}

bool answer(unsigned gen, bool yes) {
  Ask& a = ask();
  std::lock_guard<std::mutex> lk(a.m);
  if (!a.pending || a.done || a.gen != gen) return false;
  a.done = true, a.yes = yes;
  a.cv.notify_all();
  return true;
}

void set_asker_for_test(Asker f) {
  Ask& a = ask();
  std::lock_guard<std::mutex> lk(a.m);
  a.asker = std::move(f);
}

void set_ask_timeout_ms(int ms) {
  Store& s = store();
  std::lock_guard<std::mutex> lk(s.m);
  s.timeout_ms = ms;
}

void set_clock_for_test(std::function<long long()> f) {
  Store& s = store();
  std::lock_guard<std::mutex> lk(s.m);
  s.clock = std::move(f);
}

void reload_for_test() {
  Store& s = store();
  std::lock_guard<std::mutex> lk(s.m);
  s.loaded = false;
  s.recs.clear();
}

void reset_for_test() {
  reload_for_test();
  set_asker_for_test(nullptr);
  set_ask_timeout_ms(kAskTimeoutMs);
  set_clock_for_test(nullptr);
}

// ---- the module ----------------------------------------------------------------------------------------------------------

ZF_REGISTER_WEB_MODULE([] {
  webtransfer::WebModule m;
  m.name = "pair";
  m.order = 5;
  m.public_path = [](std::string_view p) { return p == "/files/pair"; };  // the PIN is checked inside, with the lock-out
  m.stream_owns = [](std::string_view meth, std::string_view p) { return meth == "POST" && p == "/files/pair"; };
  m.stream = pair_request;
  return m;
});

}  // namespace webpair
}  // namespace zf
