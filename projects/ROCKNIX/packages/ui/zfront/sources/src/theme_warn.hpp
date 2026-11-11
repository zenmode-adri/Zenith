// `Theme::f` is asked for every frame: a missing variable is reported once per name for the whole run (it was ~3 KB/s of log).
#pragma once
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace zf {
namespace theme_warn {

struct State {
  std::mutex m;
  std::unordered_set<std::string> seen;
};
inline State& state() {
  static State s;
  return s;
}

/// Logs "theme: variable 'name' missing or not a number" the first time `name` is reported (at most 256 names).
inline void once(std::string_view name) {
  State& s = state();
  std::lock_guard<std::mutex> g(s.m);
  if (s.seen.size() >= 256 || !s.seen.emplace(name).second) return;
  std::fprintf(stderr, "theme: variable '%.*s' missing or not a number\n", int(name.size()), name.data());
}
/// Names reported so far (tests).
inline size_t count() {
  State& s = state();
  std::lock_guard<std::mutex> g(s.m);
  return s.seen.size();
}

}  // namespace theme_warn
}  // namespace zf
