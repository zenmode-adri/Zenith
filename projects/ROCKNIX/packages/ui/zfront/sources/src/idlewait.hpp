// Idle sleep of the device loop (no SDL here, so it is testable on the PC).
//
// SDL_WaitEvent/WaitEventTimeout poll every millisecond while a joystick is open (SDL_events_need_polling): ~1000
// wake-ups and ~3 % of a core while idle. The loop sleeps in `poll(2)` instead, on descriptors that say "something
// happened":
//   * the pad evdev nodes (our own descriptors, drained when readable: SDL reads the device through its own);
//   * a wake-up eventfd, written by `wake()` from any thread (background jobs and counts finished). SDL_PushEvent from
//     another thread does not wake a `poll` of ours, so the loop's wake-up callback must also call `wake()`;
//   * the Wayland display socket, when the caller could get it (`set_display_fd`). It is only polled: reading it would
//     steal the compositor's events from SDL, closing it would break the connection. SDL reads it in SDL_PumpEvents.
// Without the display descriptor, window and keyboard events have no descriptor to wait on, so the sleep is capped at
// 100 ms; with it the cap is 1 s (hot-plugged pads are noticed within the cap). While a keyboard key is held the cap is
// 30 ms, because SDL generates key repeats from inside SDL_PumpEvents.
//
// Safety nets for the display descriptor: POLLHUP/ERR/NVAL drop it for good (until `set_display_fd` is called again,
// after the window is recreated); more than `spin_limit` consecutive wake-ups caused by it with no SDL event in between
// mean SDL does not drain it (a busy loop), so it is ignored for `retry_ms` and the 100 ms cap applies.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <poll.h>

#include "util.hpp"

namespace zf {

class IdleWaiter {
 public:
  IdleWaiter();
  ~IdleWaiter();
  IdleWaiter(const IdleWaiter&) = delete;
  IdleWaiter& operator=(const IdleWaiter&) = delete;

  /// Thread-safe, callable after the waiter is gone: makes the current or next `sleep` return at once.
  void wake() const;
  /// A callable that does `wake()` and stays valid after the waiter is destroyed (hand it to a worker thread).
  std::function<void()> wake_function() const;

  /// Opens (O_RDONLY|O_NONBLOCK) the evdev nodes `paths` and polls them; replaces the previous set. Unreadable ones are skipped.
  void set_pad_paths(const std::vector<std::string>& paths);
  /// Takes ownership of an already open descriptor (tests): it is drained when readable and closed with the set.
  void add_pad_fd(int fd);
  void close_pads();
  /// The Wayland socket, or -1. Never read nor closed here. Re-acquire it after every window (re)creation.
  void set_display_fd(int fd);
  int display_fd() const { return display_fd_; }
  /// True while the display descriptor is part of the sleep (set, not dropped, not in a spin penalty).
  bool display_active() const { return display_fd_ >= 0 && !spin_off_; }

  /// A key is physically held: wake often enough for SDL's key repeat.
  void set_keys_held(bool held) { keys_held_ = held; }
  /// The loop got a real SDL event: the display descriptor is being drained.
  void note_event() { spins_ = 0; }

  /// Longest sleep right now.
  int cap_ms() const;
  /// Sleeps until a descriptor is readable, `wake()`, `ms` or `cap_ms()` passed; drains what it owns.
  void sleep(int ms);

  /// Spin guard tuning (tests).
  unsigned spin_limit = 64;
  int retry_ms = 5000;

 private:
  void rebuild();
  struct WakeFd;
  std::shared_ptr<WakeFd> wake_;
  std::vector<int> pads_;
  int display_fd_ = -1;
  std::vector<pollfd> fds_;  // [0] wake, [1..pads] pads, [last] display
  unsigned spins_ = 0;
  bool spin_off_ = false, keys_held_ = false, logged_drop_ = false;
  TimePoint spin_until_{};
};

}  // namespace zf
