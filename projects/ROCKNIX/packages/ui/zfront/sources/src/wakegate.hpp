// Lets worker threads push an SDL event only while SDL is up. During a game SDL is shut down (`SDL_Quit`); a worker that
// finishes meanwhile (covers, counts, video) used to call SDL_PushEvent on a library that is gone. `notify` runs `push` under
// the lock only when the gate is open, and `set_up(false)` (called before SDL_Quit) waits for a push in flight, so none can
// start after it returns. A wake-up lost while closed is harmless: the loop drains jobs and counts right after the game.
#pragma once
#include <functional>
#include <mutex>

namespace zf {

class WakeGate {
 public:
  void set_up(bool up) {
    std::lock_guard<std::mutex> g(m_);
    up_ = up;
  }
  /// True when `push` ran.
  bool notify(const std::function<void()>& push) {
    std::lock_guard<std::mutex> g(m_);
    if (!up_) return false;
    push();
    return true;
  }

 private:
  std::mutex m_;
  bool up_ = false;
};

}  // namespace zf
