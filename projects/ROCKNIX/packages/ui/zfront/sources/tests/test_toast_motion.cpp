// Toasts with motion: the loop must keep drawing frames while one comes in or goes out, and only then.
#include <chrono>

#include "check.hpp"
#include "services.hpp"

using namespace zf;
using std::chrono::milliseconds;

int main() {
  Toasts t;
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(!t.moving(t0));  // nothing queued
  t.push("Saved", 2000);
  CHECK(t.moving(std::chrono::steady_clock::now()));  // just pushed: coming in
  const auto pushed = t.items().front().since;
  CHECK(t.items().front().until == pushed + milliseconds(2000));
  CHECK(t.moving(pushed + milliseconds(100)));         // still coming in (0.16 s)
  CHECK(!t.moving(pushed + milliseconds(300)));        // resting in the middle of its life: no frames needed
  CHECK(!t.moving(pushed + milliseconds(1800)));
  CHECK(t.moving(pushed + milliseconds(1900)));        // the last 0.14 s: going out
  // a hidden entry (the volume OSD hides behind \x1f) is not a toast and never keeps the loop awake
  Toasts o;
  o.push("\x1f" "osd", 2000);
  CHECK(!o.moving(o.items().front().since));
  return zt::done();
}
