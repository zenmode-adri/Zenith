#include "idlewait.hpp"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace zf {

// eventfd shared with whoever holds a `wake_function()`, so a late wake-up from a worker thread never hits a closed or
// reused descriptor.
struct IdleWaiter::WakeFd {
  int fd = -1;
  WakeFd() : fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}
  ~WakeFd() {
    if (fd >= 0) ::close(fd);
  }
  void signal() const {
    uint64_t one = 1;
    if (fd >= 0) (void)!::write(fd, &one, sizeof one);
  }
};

IdleWaiter::IdleWaiter() : wake_(std::make_shared<WakeFd>()) { rebuild(); }
IdleWaiter::~IdleWaiter() { close_pads(); }

void IdleWaiter::wake() const { wake_->signal(); }

std::function<void()> IdleWaiter::wake_function() const {
  std::shared_ptr<WakeFd> w = wake_;
  return [w] { w->signal(); };
}

void IdleWaiter::rebuild() {
  fds_.clear();
  fds_.push_back({wake_->fd, POLLIN, 0});
  for (int fd : pads_) fds_.push_back({fd, POLLIN, 0});
  fds_.push_back({display_fd_, POLLIN, 0});  // poll ignores a negative descriptor
}

void IdleWaiter::close_pads() {
  for (int fd : pads_) ::close(fd);
  pads_.clear();
  rebuild();
}

void IdleWaiter::add_pad_fd(int fd) {
  pads_.push_back(fd);
  rebuild();
}

void IdleWaiter::set_pad_paths(const std::vector<std::string>& paths) {
  for (int fd : pads_) ::close(fd);
  pads_.clear();
  for (const std::string& p : paths) {
    int fd = ::open(p.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) pads_.push_back(fd);
  }
  rebuild();
}

void IdleWaiter::set_display_fd(int fd) {
  display_fd_ = fd;
  spins_ = 0;
  spin_off_ = false;
  logged_drop_ = false;
  rebuild();
}

int IdleWaiter::cap_ms() const {
  int c = display_active() ? 1000 : 100;
  return keys_held_ ? std::min(c, 30) : c;
}

void IdleWaiter::sleep(int ms) {
  ms = std::max(1, std::min(ms, cap_ms()));
  if (spin_off_ && Clock::now() >= spin_until_) {  // penalty over: try the display descriptor again
    spin_off_ = false;
    spins_ = 0;
  }
  pollfd& disp = fds_.back();
  disp.fd = display_active() ? display_fd_ : -1;
  for (pollfd& p : fds_) p.revents = 0;
  int r = ::poll(fds_.data(), nfds_t(fds_.size()), ms);
  if (r <= 0) {  // timeout (or a signal)
    spins_ = 0;
    return;
  }
  if (fds_.front().revents & POLLIN) {
    uint64_t v;
    (void)!::read(fds_.front().fd, &v, sizeof v);
  }
  char buf[512];
  for (size_t i = 1; i + 1 < fds_.size(); ++i) {
    pollfd& p = fds_[i];
    if (!p.revents) continue;
    if (p.revents & POLLIN) {
      while (::read(p.fd, buf, sizeof buf) > 0) {
      }
    }
    if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) p.fd = -1;  // unplugged: SDL reports it; do not spin on it meanwhile
  }
  if (disp.fd >= 0 && disp.revents) {
    if (disp.revents & (POLLHUP | POLLERR | POLLNVAL)) {
      if (!logged_drop_) std::fprintf(stderr, "zfront: wayland display descriptor lost (revents 0x%x): polling every 100 ms\n", unsigned(disp.revents));
      logged_drop_ = true;
      display_fd_ = -1;
      disp.fd = -1;
    } else if (++spins_ > spin_limit) {  // SDL is not draining it: ignore it for a while
      std::fprintf(stderr, "zfront: wayland display descriptor wakes the loop without events: ignored for %d ms\n", retry_ms);
      spin_off_ = true;
      spin_until_ = Clock::now() + std::chrono::milliseconds(retry_ms);
      spins_ = 0;
    }
  } else {
    spins_ = 0;
  }
}

}  // namespace zf
