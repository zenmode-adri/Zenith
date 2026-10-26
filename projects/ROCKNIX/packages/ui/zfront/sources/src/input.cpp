#include "input.hpp"

#include <string>

namespace zf {

static const struct {
  const char* name;
  Action a;
} kNames[] = {
    {"up", Action::Up},       {"down", Action::Down}, {"left", Action::Left}, {"right", Action::Right}, {"a", Action::A},
    {"b", Action::B},         {"x", Action::X},       {"y", Action::Y},       {"start", Action::Start}, {"select", Action::Select},
    {"l", Action::L},         {"r", Action::R},       {"hotkey", Action::Hotkey}, {"l2", Action::L2},  {"r2", Action::R2},
    {"l3", Action::L3},       {"r3", Action::R3},
};

std::optional<Action> action_from_name(std::string_view s) {
  std::string l(s);
  for (char& c : l)
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
  for (const auto& n : kNames)
    if (l == n.name) return n.a;
  return std::nullopt;
}

const char* action_name(Action a) {
  for (const auto& n : kNames)
    if (n.a == a) return n.name;
  return "?";
}

bool action_repeats(Action a) {
  switch (a) {
    case Action::Up:
    case Action::Down:
    case Action::Left:
    case Action::Right:
    case Action::L:
    case Action::R:
      return true;
    default:
      return false;
  }
}

}  // namespace zf
