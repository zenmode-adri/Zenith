#include <string>

#include "check.hpp"
#include "input.hpp"

using namespace zf;

int main() {
  CHECK(action_from_name("A") == Action::A);
  CHECK(action_from_name("down") == Action::Down);
  CHECK(action_from_name("DOWN") == Action::Down);
  CHECK(!action_from_name("nonsense").has_value());
  for (Action a : {Action::Up, Action::Down, Action::Left, Action::Right, Action::A, Action::B, Action::X, Action::Y, Action::Start,
                   Action::Select, Action::L, Action::R, Action::Hotkey, Action::L2, Action::R2, Action::L3, Action::R3}) {
    auto back = action_from_name(action_name(a));
    CHECK(back.has_value() && *back == a);  // names round-trip
  }
  CHECK(action_repeats(Action::Down) && action_repeats(Action::L));
  CHECK(!action_repeats(Action::A) && !action_repeats(Action::Start));
  return zt::done();
}
