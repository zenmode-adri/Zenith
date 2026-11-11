#include "homemenu.hpp"

#include "launch.hpp"
#include "oscontract.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>

extern char** environ;

namespace zf::homemenu {

Combo combo_from_setting(std::string_view v) {
  if (v == "r2") return Combo::StartR2;
  if (v == "l2") return Combo::StartL2;
  return Combo::StartR1;
}

std::vector<int> combo_codes(Combo c) {
  switch (c) {
    case Combo::StartR2: return {kBtnStart, kBtnTR2};
    case Combo::StartL2: return {kBtnStart, kBtnTL2};
    case Combo::StartR1: break;
  }
  return {kBtnStart, kBtnTR};
}

// ---- ComboDetector ------------------------------------------------------------------------------------------------------------------

ComboDetector::ComboDetector(std::vector<int> codes, int hold_ms) : codes_(std::move(codes)), hold_ms_(hold_ms) {}

void ComboDetector::key(int code, int value, TimePoint now) {
  if (std::find(codes_.begin(), codes_.end(), code) == codes_.end()) return;
  if (value == 2) return;  // autorepeat of a button that is already down
  if (value == 0) {
    held_.erase(code);
    complete_ = false;
    fired_ = false;  // re-armed: a chord button went up
    return;
  }
  held_.insert(code);
  if (!complete_ && held_.size() == codes_.size()) {
    complete_ = true;
    since_ = now;
  }
}

void ComboDetector::dropped() {
  held_.clear();
  complete_ = false;
  fired_ = false;
}

bool ComboDetector::poll(TimePoint now) {
  if (!complete_ || fired_) return false;
  if (now - since_ < std::chrono::milliseconds(hold_ms_)) return false;
  fired_ = true;
  return true;
}

std::optional<TimePoint> ComboDetector::deadline() const {
  if (!complete_ || fired_) return std::nullopt;
  return since_ + std::chrono::milliseconds(hold_ms_);
}

void feed_events(const void* buf, size_t bytes, ComboDetector& d, TimePoint now) {
  const unsigned char* p = static_cast<const unsigned char*>(buf);
  for (size_t off = 0; off + sizeof(input_event) <= bytes; off += sizeof(input_event)) {
    input_event ev;
    std::memcpy(&ev, p + off, sizeof ev);
    if (ev.type == EV_KEY)
      d.key(ev.code, ev.value, now);
    else if (ev.type == EV_SYN && ev.code == SYN_DROPPED)
      d.dropped();
  }
}

// ---- PadWatcher ---------------------------------------------------------------------------------------------------------------------

PadWatcher::~PadWatcher() {
  for (int fd : fds_) ::close(fd);
}

size_t PadWatcher::open(const std::vector<std::string>& paths) {
  size_t n = 0;
  for (const std::string& p : paths) {
    int fd = ::open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    fds_.push_back(fd);
    ++n;
  }
  return n;
}

bool PadWatcher::wait(int max_ms) {
  int ms = std::max(1, max_ms);
  TimePoint now = Clock::now();
  if (det_.poll(now)) return true;
  if (std::optional<TimePoint> dl = det_.deadline()) {
    long left = std::chrono::duration_cast<std::chrono::milliseconds>(*dl - now).count() + 1;
    ms = int(std::max(1L, std::min<long>(ms, left)));
  }
  std::vector<pollfd> p;
  for (int fd : fds_) p.push_back({fd, POLLIN, 0});
  int r = ::poll(p.data(), nfds_t(p.size()), ms);
  if (r > 0) {
    for (size_t i = 0; i < p.size(); ++i) {
      if (p[i].revents & POLLIN) {
        unsigned char buf[sizeof(input_event) * 64];
        for (;;) {  // drain what is there (it is only read, nobody else is affected)
          ssize_t n = ::read(p[i].fd, buf, sizeof buf);
          if (n <= 0) break;
          feed_events(buf, size_t(n), det_, Clock::now());
          if (size_t(n) < sizeof buf) break;
        }
      } else if (p[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {  // unplugged: stop polling it
        ::close(p[i].fd);
        fds_.erase(std::remove(fds_.begin(), fds_.end(), p[i].fd), fds_.end());
        det_.dropped();
      }
    }
  }
  return det_.poll(Clock::now());
}

bool PadWatcher::wait_released(int max_ms) {
  TimePoint end = Clock::now() + std::chrono::milliseconds(max_ms);
  while (det_.any_held()) {
    TimePoint now = Clock::now();
    if (now >= end) return false;
    wait(int(std::max<long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(end - now).count())));
  }
  return true;
}

// ---- the sequence ---------------------------------------------------------------------------------------------------------------------

PanelResult panel_result_from_exit(int st) {
  if (st < 0 || !WIFEXITED(st)) return PanelResult::Failed;
  switch (WEXITSTATUS(st)) {
    case kExitResume: return PanelResult::Resume;
    case kExitQuit: return PanelResult::Quit;
    case kExitSave: return PanelResult::SaveState;
    case kExitLoad: return PanelResult::LoadState;
    default: break;
  }
  return PanelResult::Failed;
}

int exit_code_of(PanelResult r) {
  switch (r) {
    case PanelResult::Quit: return kExitQuit;
    case PanelResult::SaveState: return kExitSave;
    case PanelResult::LoadState: return kExitLoad;
    default: break;
  }
  return kExitResume;
}

namespace {
PanelResult g_panel_result = PanelResult::Resume;
}
void set_panel_result(PanelResult r) { g_panel_result = r; }
namespace {
bool g_panel_mode = false;
}
void set_panel_mode(bool on) { g_panel_mode = on; }
bool panel_mode() { return g_panel_mode; }
PanelResult panel_result() { return g_panel_result; }

Outcome trigger(const Io& io) {
  std::vector<int> pids = io.find_pids ? io.find_pids() : std::vector<int>();
  if (pids.empty()) return Outcome::Nothing;
  if (io.record) io.record(pids);  // before the first SIGSTOP: if we die from here on, the next start resumes them
  std::vector<int> stopped;
  bool failed = false;
  for (int pid : pids) {
    if (io.kill(pid, SIGSTOP) != 0) {
      failed = true;
      break;
    }
    stopped.push_back(pid);
    set_stopped(stopped);
  }
  // `resume` is the only way out from here on
  auto resume = [&] {
    for (size_t i = stopped.size(); i-- > 0;) io.kill(stopped[i], SIGCONT);
    set_stopped({});
    if (io.record) io.record({});
  };
  if (failed) {
    resume();
    return Outcome::PauseFailed;
  }
  PanelResult r = io.panel ? io.panel(stopped) : PanelResult::Failed;
  if (io.term_requested && io.term_requested()) r = PanelResult::Resume;  // a SIGTERM is pending for the game: let it through
  if (io.wait_buttons_up) io.wait_buttons_up();  // so the game does not resume with the chord still down
  resume();
  if (io.refocus) io.refocus(stopped);
  switch (r) {
    case PanelResult::Quit:
      if (io.quit_game) io.quit_game();
      return Outcome::Quit;
    case PanelResult::SaveState:
      if (io.send_state) io.send_state("SAVE_STATE");
      return Outcome::StateSaved;
    case PanelResult::LoadState:
      if (io.send_state) io.send_state("LOAD_STATE");
      return Outcome::StateLoaded;
    default: break;
  }
  return Outcome::Resumed;
}

// ---- helpers --------------------------------------------------------------------------------------------------------------------------

std::vector<int> find_pids_by_name(const std::vector<std::string>& names, int self, const std::string& proc_root) {
  std::vector<int> out;
  DIR* d = ::opendir(proc_root.c_str());
  if (!d) return out;
  while (const dirent* e = ::readdir(d)) {
    char* end = nullptr;
    long pid = std::strtol(e->d_name, &end, 10);
    if (!end || *end || pid <= 1 || pid == self) continue;
    std::string comm;
    if (!read_file(proc_root + "/" + e->d_name + "/comm", comm)) continue;
    while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r')) comm.pop_back();
    for (const std::string& n : names)
      if (comm == n.substr(0, 15)) {
        out.push_back(int(pid));
        break;
      }
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

NetCmd parse_retroarch_net(std::string_view text) {
  NetCmd c;
  size_t p = 0;
  while (p < text.size()) {
    size_t e = text.find('\n', p);
    if (e == std::string_view::npos) e = text.size();
    std::string_view line = trim(text.substr(p, e - p));
    p = e + 1;
    size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    std::string_view k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    if (k == "network_cmd_enable") c.enabled = (v == "true" || v == "1");
    if (k == "network_cmd_port") {
      long n = 0;
      if (parse_int(v, n) && n > 0 && n < 65536) c.port = int(n);
    }
  }
  return c;
}

bool send_udp_local(int port, std::string_view text) {
  int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  sockaddr_in a;
  std::memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(uint16_t(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ssize_t n = ::sendto(fd, text.data(), text.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
  ::close(fd);
  return n == ssize_t(text.size());
}

bool is_retroarch(const std::vector<std::string>& names) {
  for (const std::string& n : names)
    if (starts_with(n, "retroarch")) return true;
  return false;
}

bool enabled(const SystemConf& conf) { return conf.get_bool(kKeyEnable, false); }
Combo combo(const SystemConf& conf) { return combo_from_setting(conf.get_or(kKeyCombo, "r1")); }

namespace {

void swaymsg_focus(int pid) {
  std::string cmd = "[pid=" + std::to_string(pid) + "] focus";
  std::vector<std::string> env_s;
  bool has_sock = false;
  for (char** e = environ; e && *e; ++e) {
    if (starts_with(*e, "SWAYSOCK=")) has_sock = true;
    env_s.push_back(*e);
  }
  if (!has_sock) {  // started by systemd: look for the socket sway made in the runtime dir
    const char* rd = std::getenv("XDG_RUNTIME_DIR");
    std::string dir = rd && *rd ? rd : "/var/run/0-runtime-dir";
    if (DIR* d = ::opendir(dir.c_str())) {
      while (const dirent* e = ::readdir(d))
        if (starts_with(e->d_name, "sway-ipc.") && ends_with(e->d_name, ".sock")) {
          env_s.push_back("SWAYSOCK=" + dir + "/" + e->d_name);
          break;
        }
      ::closedir(d);
    }
  }
  std::vector<char*> envp;
  for (std::string& s : env_s) envp.push_back(s.data());
  envp.push_back(nullptr);
  std::string a0 = "swaymsg", a1 = cmd;
  char* argv[] = {a0.data(), a1.data(), nullptr};
  pid_t c = -1;
  if (::posix_spawnp(&c, "swaymsg", nullptr, nullptr, argv, envp.data()) != 0 || c <= 0) return;
  for (int i = 0; i < 75; ++i) {  // 1.5 s at most
    int st = 0;
    if (::waitpid(c, &st, WNOHANG) == c) return;
    timespec ts{0, 20 * 1000 * 1000};
    ::nanosleep(&ts, nullptr);
  }
  ::kill(c, SIGKILL);
  ::waitpid(c, nullptr, 0);
}

bool states_available(const std::vector<std::string>& names, int* port) {
  if (!is_retroarch(names)) return false;
  std::string cfg;
  if (!read_file(kRetroarchCfg, cfg)) return false;
  NetCmd n = parse_retroarch_net(cfg);
  if (port) *port = n.port;
  return n.enabled;
}

}  // namespace

Io real_io(PadWatcher& w) {
  Io io;
  auto names = std::make_shared<std::vector<std::string>>();
  io.kill = [](int pid, int sig) { return ::kill(pid_t(pid), sig); };
  io.find_pids = [names] {
    *names = oscontract::kill_names();
    if (names->empty()) return std::vector<int>();
    return find_pids_by_name(*names, int(::getpid()));
  };
  io.record = [](const std::vector<int>& p) { write_stopped_file(stopped_file_path(), p); };
  // the chord held again while the panel is up closes it (the game resumes): the way out if the panel ever stops answering
  io.panel = [names, &w](const std::vector<int>&) {
    w.wait_released(2000);
    return run_panel_child(states_available(*names, nullptr), [&w] { return launch::term_requested() || w.wait(1); });
  };
  io.term_requested = [] { return launch::term_requested(); };
  io.quit_game = [] { oscontract::emukill(); };
  io.send_state = [names](const char* cmd) {
    int port = 55355;
    if (states_available(*names, &port)) send_udp_local(port, cmd);
  };
  io.refocus = [](const std::vector<int>& pids) {
    for (int p : pids) swaymsg_focus(p);
  };
  io.wait_buttons_up = [&w] { w.wait_released(2000); };
  return io;
}

// ---- stopped pids: signal handlers and crash recovery ---------------------------------------------------------------------------------

namespace {
constexpr int kMaxStopped = 16;
volatile sig_atomic_t g_stopped[kMaxStopped];
volatile sig_atomic_t g_nstopped = 0;
struct sigaction g_old_term, g_old_segv, g_old_abrt;
bool g_safety = false;

void resume_all_from_handler() {
  for (int i = int(g_nstopped); i-- > 0;)
    if (g_stopped[i] > 1) ::kill(pid_t(g_stopped[i]), SIGCONT);
}

void on_fatal(int sig) {
  resume_all_from_handler();
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}

void on_term_stopped(int sig) {
  // the game's own SIGTERM (launch.cpp) stays pending on a stopped process: resume it so it can be delivered, then behave as the
  // handler we replaced (which forwards the signal to the game)
  resume_all_from_handler();
  if (g_old_term.sa_handler != SIG_DFL && g_old_term.sa_handler != SIG_IGN && g_old_term.sa_handler) g_old_term.sa_handler(sig);
}
}  // namespace

void set_stopped(const std::vector<int>& pids) {
  int n = int(std::min<size_t>(pids.size(), kMaxStopped));
  for (int i = 0; i < n; ++i) g_stopped[i] = pids[size_t(i)];
  g_nstopped = n;
}

void install_safety() {
  if (g_safety) return;
  g_safety = true;
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = on_term_stopped;
  ::sigaction(SIGTERM, &sa, &g_old_term);
  sa.sa_handler = on_fatal;
  sa.sa_flags = SA_NODEFER;
  ::sigaction(SIGSEGV, &sa, &g_old_segv);
  ::sigaction(SIGABRT, &sa, &g_old_abrt);
}

void remove_safety() {
  if (!g_safety) return;
  g_safety = false;
  ::sigaction(SIGTERM, &g_old_term, nullptr);
  ::sigaction(SIGSEGV, &g_old_segv, nullptr);
  ::sigaction(SIGABRT, &g_old_abrt, nullptr);
}

const char* stopped_file_path() { return "/run/zfront/home-stopped"; }

void write_stopped_file(const std::string& path, const std::vector<int>& pids) {
  if (pids.empty()) {
    ::unlink(path.c_str());
    return;
  }
  size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0) ::mkdir(path.substr(0, slash).c_str(), 0755);
  std::string text;
  for (int p : pids) text += std::to_string(p) + "\n";
  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  bool ok = ::write(fd, text.data(), text.size()) == ssize_t(text.size());
  ::close(fd);
  if (ok)
    ::rename(tmp.c_str(), path.c_str());
  else
    ::unlink(tmp.c_str());
}

size_t recover_stopped(const std::string& path, const std::function<int(int, int)>& kill_fn) {
  std::string t;
  if (!read_file(path, t)) return 0;
  size_t n = 0, p = 0;
  while (p < t.size()) {
    size_t e = t.find('\n', p);
    if (e == std::string::npos) e = t.size();
    long pid = 0;
    if (parse_int(trim(std::string_view(t).substr(p, e - p)), pid) && pid > 1 && kill_fn(int(pid), SIGCONT) == 0) ++n;
    p = e + 1;
  }
  ::unlink(path.c_str());
  return n;
}

// ---- the child panel --------------------------------------------------------------------------------------------------------------------

namespace {
std::string g_exe;
std::vector<std::string> g_args;
}  // namespace

void set_panel_args(std::string exe, std::vector<std::string> args) {
  g_exe = std::move(exe);
  g_args = std::move(args);
}
bool panel_args_set() { return !g_exe.empty(); }

PanelResult run_panel_child(bool with_states, const std::function<bool()>& term_requested) {
  if (g_exe.empty()) return PanelResult::Failed;
  std::vector<std::string> argv_s;
  argv_s.push_back(g_exe);
  for (const std::string& a : g_args) argv_s.push_back(a);
  argv_s.push_back("--home-panel");
  if (with_states) argv_s.push_back("--home-states");
  std::vector<char*> argv;
  for (std::string& s : argv_s) argv.push_back(s.data());
  argv.push_back(nullptr);
  // own Wayland app_id: the sway rule for "emulationstation" reloads the whole sway config when a window with that id appears
  std::vector<std::string> env_s;
  for (char** e = environ; e && *e; ++e)
    if (!starts_with(*e, "SDL_VIDEO_WAYLAND_WMCLASS=")) env_s.push_back(*e);
  env_s.push_back("SDL_VIDEO_WAYLAND_WMCLASS=zfront-home");
  std::vector<char*> envp;
  for (std::string& s : env_s) envp.push_back(s.data());
  envp.push_back(nullptr);
  pid_t pid = -1;
  if (::posix_spawn(&pid, g_exe.c_str(), nullptr, nullptr, argv.data(), envp.data()) != 0 || pid <= 0) return PanelResult::Failed;
  bool killed = false;
  for (;;) {
    int st = 0;
    pid_t r = ::waitpid(pid, &st, WNOHANG);
    if (r == pid) return killed ? PanelResult::Resume : panel_result_from_exit(st);
    if (r < 0 && errno != EINTR) return PanelResult::Failed;
    if (!killed && term_requested && term_requested()) {
      ::kill(pid, SIGKILL);
      killed = true;
    }
    timespec ts{0, 50 * 1000 * 1000};
    ::nanosleep(&ts, nullptr);
  }
}

}  // namespace zf::homemenu
