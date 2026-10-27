// Shared services every menu / dialog needs, owned by `Ctx` (`cx.sv`). Menu modules NEVER spawn processes, write the
// settings files or start threads on their own: they go through these, so dry-run, locking and wake-ups work the same
// everywhere.
#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace zf {

// ---- system.cfg lock ------------------------------------------------------------------------------------------------
/// Same lock as `wait_lock` in /etc/profile.d/001-functions: `/tmp/.system.cfg.lock` created with O_EXCL, holding the
/// owner pid. runemu.sh, setsettings.sh and the tuner edit system.cfg with `sed -i` under it, so we must hold it for
/// every read-modify-write. A lock whose owner is dead (or older than 10 s) is stolen.
class ConfLock {
 public:
  explicit ConfLock(const std::string& path = "/tmp/.system.cfg.lock", int timeout_ms = 3000);
  ~ConfLock();
  ConfLock(const ConfLock&) = delete;
  ConfLock& operator=(const ConfLock&) = delete;
  bool ok() const { return held_; }

 private:
  std::string path_;
  bool held_ = false;
};

// ---- commands -------------------------------------------------------------------------------------------------------
struct CmdResult {
  int exit_code = -1;     // -1: could not start; 0 in dry-run
  std::string out;        // stdout (stderr merged), capped at 64 KB; empty in dry-run
  bool dry = false;
};

/// Runs `sh -c`. In dry-run mode (harness, tests, `--dry-run`) nothing is executed: the command is only logged.
/// NEVER build a command from unchecked text: pass user/network text through `shell_quote`.
class CmdRunner {
 public:
  bool dry_run = false;
  /// Blocking, with a timeout (0 = none). Use `Jobs` for anything that can take long.
  CmdResult run(const std::string& cmd, int timeout_ms = 10000);
  /// Fire and forget (no output kept).
  void spawn(const std::string& cmd);
  /// Commands seen in dry-run (or all of them, `keep_log`), for tests and `--dry-run` reports.
  std::vector<std::string> log;
  bool keep_log = false;
};

/// 'text' quoted for sh -c.
std::string shell_quote(std::string_view s);

// ---- toasts ---------------------------------------------------------------------------------------------------------
/// How long a toast takes to come in and to go out (`cx.ui_motion`), seconds.
constexpr float kToastIn = 0.16f, kToastOut = 0.14f;

struct Toast {
  std::string text;
  std::chrono::steady_clock::time_point until;
  std::chrono::steady_clock::time_point since{};  // when it was pushed
};

/// Short messages ("Saved", "WiFi connected"). Drawn by the app on top of every screen (toast.cpp).
class Toasts {
 public:
  void push(std::string text, int ms = 2000);
  /// Drops expired ones; true when the set changed (redraw needed).
  bool expire(std::chrono::steady_clock::time_point now);
  /// Earliest expiry, to sleep until.
  std::optional<std::chrono::steady_clock::time_point> next_deadline() const;
  const std::deque<Toast>& items() const { return items_; }
  /// True while some toast is coming in or going out (the loop keeps drawing frames for it).
  bool moving(std::chrono::steady_clock::time_point now) const;

 private:
  std::deque<Toast> items_;
};

// ---- background jobs ------------------------------------------------------------------------------------------------
/// One low-priority worker thread (started on the first `post`). `work` runs there and must not touch `Ctx`; `done`
/// runs later on the MAIN thread (inside `App::drain_jobs`) with whatever `work` produced captured by a shared_ptr.
/// Use it for cover decoding, wifictl, scans, network. Order is FIFO.
class Jobs {
 public:
  ~Jobs();
  void post(std::function<void()> work, std::function<void()> done = nullptr);
  /// Main thread: run the `done` callbacks of finished jobs; true if any ran.
  bool drain();
  /// Called from the worker when a job finished (the event loop's wake-up). Thread-safe to set before the first post.
  void set_wake(std::function<void()> wake);
  bool idle() const;

 private:
  struct Item {
    std::function<void()> work, done;
  };
  void loop();
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<Item> todo_;
  std::deque<std::function<void()>> finished_;
  std::function<void()> wake_;
  std::thread th_;
  bool stop_ = false, started_ = false, busy_ = false;
};

// ---- text -----------------------------------------------------------------------------------------------------------
/// Translation hook: every user-visible string goes through `tr("ENGLISH TEXT")`. Identity until the i18n module loads
/// the catalogue (`set_translator`). The English text is the key, as in ES's gettext catalogs.
const char* tr(const char* english);
/// `f` returns the translation or nullptr; returned pointers must stay valid until the next `set_translator`.
void set_translator(std::function<const char*(const char*)> f);
/// Marks a string that is translated later with `tr(variable)` (tables, titles, prompts): identity at run time, but
/// `tools/extract-strings.sh` collects it like a `tr("...")` call. Use it wherever a literal is stored before it is shown.
#define N_(s) s
/// Same with a gettext context (`ctr(context, text)` at run time): `NC_("joystick", "D-PAD UP")`.
#define NC_(c, s) s

struct Services {
  CmdRunner cmd;
  Toasts toasts;
  Jobs jobs;
};

}  // namespace zf
