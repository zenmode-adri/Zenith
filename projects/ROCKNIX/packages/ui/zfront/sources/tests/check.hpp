// Tiny test helper: CHECK/CHECK_EQ count failures, `done()` is the exit code. No framework dependency.
#pragma once
#include <cstdio>
#include <string>

namespace zt {
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
