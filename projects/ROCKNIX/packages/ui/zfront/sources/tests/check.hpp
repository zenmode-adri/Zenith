// Tiny test helper: CHECK/CHECK_EQ count failures, `done()` is the exit code. No framework dependency.
#pragma once
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace zt {
/// Where a test keeps the files it writes. The program saves files with fsync (on the console's card that is the point), and
/// on a PC busy with a build the disk answers an fsync in tens of seconds (measured: up to 43 s, while a build ran): a test whose
/// timeouts are seconds then fails for no fault of the code. RAM (/dev/shm) has no such wait; /tmp when there is none.
inline std::string tmp_base() {
  if (const char* o = std::getenv("ZF_TEST_TMP")) return o;  // to put the files on the disk on purpose (measuring)
  return ::access("/dev/shm", W_OK | X_OK) == 0 ? "/dev/shm" : "/tmp";
}
inline int& failures() {
  static int n = 0;
  return n;
}
inline int done() {
  if (failures()) std::fprintf(stderr, "%d check(s) failed\n", failures());
  return failures() ? 1 : 0;
}
}  // namespace zt

#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
      ++zt::failures();                                                  \
    }                                                                    \
  } while (0)

#define CHECK_EQ(a, b)                                                                                              \
  do {                                                                                                              \
    auto va_ = (a);                                                                                                 \
    auto vb_ = (b);                                                                                                 \
    if (!(va_ == vb_)) {                                                                                            \
      std::fprintf(stderr, "%s:%d: CHECK_EQ(%s, %s) failed\n", __FILE__, __LINE__, #a, #b);                         \
      ++zt::failures();                                                                                             \
    }                                                                                                               \
  } while (0)
