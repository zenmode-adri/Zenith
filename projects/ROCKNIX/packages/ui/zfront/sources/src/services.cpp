#include "services.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace zf {

using SteadyClock = std::chrono::steady_clock;

// ---- ConfLock -------------------------------------------------------------------------------------------------------
ConfLock::ConfLock(const std::string& path, int timeout_ms) : path_(path) {
  auto end = SteadyClock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd >= 0) {
      char buf[32];
      int n = std::snprintf(buf, sizeof buf, "%d\n", int(::getpid()));
      if (::write(fd, buf, size_t(n)) < 0) {
      }
      ::close(fd);
      held_ = true;
      return;
    }
    if (errno != EEXIST) return;  // cannot create the lock (read-only dir...): give up, the caller decides
    // steal it when the owner is gone or the lock is ancient
    struct stat st;
    bool stale = false;
    if (::stat(path_.c_str(), &st) == 0) {
      if (std::time(nullptr) - st.st_mtime > 10) stale = true;
      FILE* f = std::fopen(path_.c_str(), "r");
      if (f) {
        int pid = 0;
        if (std::fscanf(f, "%d", &pid) == 1 && pid > 0 && ::kill(pid, 0) != 0 && errno == ESRCH) stale = true;
        std::fclose(f);
      }
    }
    if (stale) {
      ::unlink(path_.c_str());
      continue;
    }
    if (SteadyClock::now() >= end) return;
    ::usleep(20 * 1000);
  }
}

ConfLock::~ConfLock() {
  if (held_) ::unlink(path_.c_str());
}

// ---- CmdRunner ------------------------------------------------------------------------------------------------------
std::string shell_quote(std::string_view s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r.push_back(c);
  }
  r.push_back('\'');
  return r;
}

CmdResult CmdRunner::run(const std::string& cmd, int timeout_ms) {
  CmdResult r;
  if (dry_run || keep_log) log.push_back(cmd);
  if (dry_run) {
    r.exit_code = 0;
    r.dry = true;
    return r;
  }
  int pfd[2];
  if (::pipe2(pfd, O_CLOEXEC) != 0) return r;
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pfd[0]);
    ::close(pfd[1]);
    return r;
  }
  if (pid == 0) {
    ::dup2(pfd[1], 1);
    ::dup2(pfd[1], 2);
    int dn = ::open("/dev/null", O_RDONLY);
    if (dn >= 0) ::dup2(dn, 0);
    ::execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
  }
  ::close(pfd[1]);
  auto end = timeout_ms > 0 ? SteadyClock::now() + std::chrono::milliseconds(timeout_ms) : SteadyClock::time_point::max();
  char buf[4096];
  bool killed = false;
  for (;;) {
    int wait_ms = 100;
    if (timeout_ms > 0) {
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - SteadyClock::now()).count();
      if (left <= 0) {
        ::kill(pid, SIGKILL);
        killed = true;
        break;
      }
      wait_ms = int(std::min<long long>(left, 100));
    }
    pollfd p{pfd[0], POLLIN, 0};
    int rs = ::poll(&p, 1, wait_ms);
    if (rs > 0) {
      ssize_t n = ::read(pfd[0], buf, sizeof buf);
      if (n <= 0) break;  // EOF: the child closed its end
      if (r.out.size() < 65536) r.out.append(buf, size_t(n));
    }
  }
  ::close(pfd[0]);
  int st = 0;
  ::waitpid(pid, &st, 0);
  if (killed) r.exit_code = -2;
  else r.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1);
  return r;
}

void CmdRunner::spawn(const std::string& cmd) {
  if (dry_run || keep_log) log.push_back(cmd);
  if (dry_run) return;
  pid_t pid = ::fork();
  if (pid < 0) return;
  if (pid > 0) {
    int st = 0;
    ::waitpid(pid, &st, 0);  // the intermediate child exits at once (double fork): no zombie is left
    return;
  }
  ::setsid();
  if (::fork() != 0) ::_exit(0);
  int dn = ::open("/dev/null", O_RDWR);
  if (dn >= 0) {
    ::dup2(dn, 0);
    ::dup2(dn, 1);
    ::dup2(dn, 2);
  }
  ::execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
  ::_exit(127);
}

// ---- Toasts ---------------------------------------------------------------------------------------------------------
void Toasts::push(std::string text, int ms) {
  if (items_.size() >= 4) items_.pop_front();
  items_.push_back({std::move(text), SteadyClock::now() + std::chrono::milliseconds(ms)});
}

bool Toasts::expire(SteadyClock::time_point now) {
  bool ch = false;
  for (size_t i = 0; i < items_.size();) {
    if (items_[i].until <= now) {
      items_.erase(items_.begin() + long(i));
      ch = true;
    } else {
      ++i;
    }
  }
  return ch;
}

std::optional<SteadyClock::time_point> Toasts::next_deadline() const {
  if (items_.empty()) return std::nullopt;
  auto t = items_.front().until;
  for (const Toast& x : items_) t = std::min(t, x.until);
  return t;
}

// ---- Jobs -----------------------------------------------------------------------------------------------------------
Jobs::~Jobs() {
  {
    std::lock_guard<std::mutex> g(m_);
    stop_ = true;
    todo_.clear();
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

void Jobs::set_wake(std::function<void()> wake) {
  std::lock_guard<std::mutex> g(m_);
  wake_ = std::move(wake);
}

void Jobs::post(std::function<void()> work, std::function<void()> done) {
  std::lock_guard<std::mutex> g(m_);
  todo_.push_back({std::move(work), std::move(done)});
  if (!started_) {
    started_ = true;
    th_ = std::thread([this] { loop(); });
  }
  cv_.notify_one();
}

void Jobs::loop() {
  // low priority: the UI thread always wins
  if (::nice(10) == -1) {
  }
  for (;;) {
    Item it;
    {
      std::unique_lock<std::mutex> g(m_);
      cv_.wait(g, [this] { return stop_ || !todo_.empty(); });
      if (stop_) return;
      it = std::move(todo_.front());
      todo_.pop_front();
      busy_ = true;
    }
    if (it.work) it.work();
    std::function<void()> wake;
    {
      std::lock_guard<std::mutex> g(m_);
      busy_ = false;
      if (it.done) finished_.push_back(std::move(it.done));
      wake = wake_;
    }
    if (wake) wake();
  }
}

bool Jobs::drain() {
  std::deque<std::function<void()>> run;
  {
    std::lock_guard<std::mutex> g(m_);
    run.swap(finished_);
  }
  for (auto& f : run) f();
  return !run.empty();
}

bool Jobs::idle() const {
  std::lock_guard<std::mutex> g(m_);
  return todo_.empty() && finished_.empty() && !busy_;
}

// ---- tr -------------------------------------------------------------------------------------------------------------
static std::function<const char*(const char*)>& translator() {
  static std::function<const char*(const char*)> f;
  return f;
}

const char* tr(const char* english) {
  auto& f = translator();
  if (!f) return english;
  const char* t = f(english);
  return t ? t : english;
}

void set_translator(std::function<const char*(const char*)> f) { translator() = std::move(f); }

}  // namespace zf
