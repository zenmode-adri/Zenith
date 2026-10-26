// IdleWaiter: wake-ups from pad descriptors, the eventfd and the (never drained) display descriptor, caps and safety nets.
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "check.hpp"
#include "idlewait.hpp"

using namespace zf;
using std::chrono::milliseconds;

namespace {

double ms_since(TimePoint t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct Pipe {
  int r = -1, w = -1;
  Pipe() {
    int f[2];
    CHECK(::pipe2(f, O_NONBLOCK | O_CLOEXEC) == 0);
    r = f[0];
    w = f[1];
  }
  ~Pipe() {
    if (r >= 0) ::close(r);
    if (w >= 0) ::close(w);
  }
  void put() const { (void)!::write(w, "x", 1); }
  bool readable() const {
    pollfd p{r, POLLIN, 0};
    return ::poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
  }
};

void test_caps() {
  IdleWaiter iw;
  CHECK_EQ(iw.cap_ms(), 100);  // no display descriptor: window events need the 100 ms tick
  Pipe disp;
  iw.set_display_fd(disp.r);
  CHECK(iw.display_active());
  CHECK_EQ(iw.cap_ms(), 1000);
  iw.set_keys_held(true);
  CHECK_EQ(iw.cap_ms(), 30);  // SDL generates key repeats inside SDL_PumpEvents
  iw.set_keys_held(false);
  iw.set_display_fd(-1);
  CHECK_EQ(iw.cap_ms(), 100);
}

void test_timeout_and_cap() {
  IdleWaiter iw;
  TimePoint t = Clock::now();
  iw.sleep(40);
  double d = ms_since(t);
  CHECK(d >= 38 && d < 200);
  t = Clock::now();
  iw.sleep(5000);  // capped at 100 ms
  d = ms_since(t);
  CHECK(d >= 95 && d < 400);
}

void test_wake_from_thread() {
  IdleWaiter iw;
  Pipe disp;
  iw.set_display_fd(disp.r);  // cap 1 s: only the eventfd can end the sleep early
  std::function<void()> wake = iw.wake_function();
  std::thread th([&] {
    std::this_thread::sleep_for(milliseconds(60));
    wake();
  });
  TimePoint t = Clock::now();
  iw.sleep(5000);
  double d = ms_since(t);
  th.join();
  CHECK(d >= 50 && d < 500);
  // the wake-up was consumed: the next sleep waits again
  t = Clock::now();
  iw.sleep(80);
  CHECK(ms_since(t) >= 75);
  // a wake before the sleep makes it return at once, and a wake_function outlives the waiter
  iw.wake();
  t = Clock::now();
  iw.sleep(5000);
  CHECK(ms_since(t) < 50);
  std::function<void()> late;
  {
    IdleWaiter gone;
    late = gone.wake_function();
  }
  late();  // must not crash or write to a reused descriptor
}

void test_pad_fd_is_drained() {
  IdleWaiter iw;
  Pipe pad;
  iw.add_pad_fd(pad.r);
  pad.r = -1;  // owned by the waiter now
  int w = pad.w;
  (void)!::write(w, "abc", 3);
  TimePoint t = Clock::now();
  iw.sleep(5000);
  CHECK(ms_since(t) < 50);
  t = Clock::now();
  iw.sleep(60);  // drained: nothing left to wake it
  CHECK(ms_since(t) >= 55);
}

void test_display_is_polled_not_drained() {
  IdleWaiter iw;
  Pipe disp;
  iw.set_display_fd(disp.r);
  std::thread th([&] {
    std::this_thread::sleep_for(milliseconds(50));
    disp.put();
  });
  TimePoint t = Clock::now();
  iw.sleep(5000);
  double d = ms_since(t);
  th.join();
  CHECK(d >= 40 && d < 500);
  CHECK(disp.readable());  // SDL still has to read it: the waiter must not
  CHECK_EQ(iw.display_fd(), disp.r);
}

void test_spin_guard() {
  IdleWaiter iw;
  iw.spin_limit = 8;
  iw.retry_ms = 150;
  Pipe disp;
  iw.set_display_fd(disp.r);
  disp.put();  // never drained: a busy loop unless the waiter protects itself
  TimePoint t = Clock::now();
  for (int i = 0; i < 8; ++i) iw.sleep(5000);
  CHECK(ms_since(t) < 100);
  CHECK(iw.display_active());
  iw.sleep(5000);  // 9th consecutive instant wake-up: the descriptor is ignored from now on
  CHECK(!iw.display_active());
  CHECK_EQ(iw.cap_ms(), 100);
  t = Clock::now();
  iw.sleep(5000);  // sleeps the full cap instead of spinning
  CHECK(ms_since(t) >= 95);
  // after the penalty it is tried again
  std::this_thread::sleep_for(milliseconds(100));
  iw.sleep(1);
  CHECK(iw.display_active());
}

void test_spin_counter_resets_on_events() {
  IdleWaiter iw;
  iw.spin_limit = 4;
  Pipe disp;
  iw.set_display_fd(disp.r);
  disp.put();
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 4; ++i) iw.sleep(5000);
    iw.note_event();  // SDL produced an event: the descriptor is being drained, not spinning
  }
  CHECK(iw.display_active());
}

void test_display_hangup() {
  IdleWaiter iw;
  Pipe disp;
  iw.set_display_fd(disp.r);
  ::close(disp.w);
  disp.w = -1;
  iw.sleep(5000);  // POLLHUP: dropped, not closed by the waiter
  CHECK_EQ(iw.display_fd(), -1);
  CHECK(!iw.display_active());
  CHECK(::fcntl(disp.r, F_GETFD) != -1);
  TimePoint t = Clock::now();
  iw.sleep(5000);
  CHECK(ms_since(t) >= 95);  // no spinning on the dead descriptor
}

void test_pad_hangup_does_not_spin() {
  IdleWaiter iw;
  Pipe pad;
  iw.add_pad_fd(pad.r);
  pad.r = -1;
  ::close(pad.w);
  pad.w = -1;
  iw.sleep(5000);  // HUP once...
  TimePoint t = Clock::now();
  iw.sleep(5000);  // ...then ignored until the pad set is rebuilt
  CHECK(ms_since(t) >= 95);
}

}  // namespace

int main() {
  test_caps();
  test_timeout_and_cap();
  test_wake_from_thread();
  test_pad_fd_is_drained();
  test_display_is_polled_not_drained();
  test_spin_guard();
  test_spin_counter_resets_on_events();
  test_display_hangup();
  test_pad_hangup_does_not_spin();
  return zt::done();
}
