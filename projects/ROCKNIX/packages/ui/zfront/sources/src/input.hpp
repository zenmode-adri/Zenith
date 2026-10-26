// Logical input: every device (keyboard, gamepad, the test harness) becomes an `Action`; screens only know actions.
// Gamepad mapping (es_input.cfg, GUIDs, hats, axes, repeat) lives in pad.hpp and ends here.
#pragma once
#include <optional>
#include <string_view>

namespace zf {

/// L2/R2 (triggers; ES: previous/next system, or letter jump) and L3/R3 (stick clicks) come from the gamepad mapping
/// only; screens that do not use them ignore them. They are appended so existing values do not change.
enum class Action { Up, Down, Left, Right, A, B, X, Y, Start, Select, L, R, Hotkey, L2, R2, L3, R3 };

/// Names used by the headless harness (`--script right,A,down`), case-insensitive.
std::optional<Action> action_from_name(std::string_view s);
const char* action_name(Action a);
/// Directions and shoulder buttons repeat while held; face buttons do not.
bool action_repeats(Action a);

}  // namespace zf
