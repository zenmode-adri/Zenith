// Checks that ES does just before a game starts (ViewController::checkLaunchOptions): with CheckBiosesAtLaunch on, a system
// that has missing BIOS files asks "launch anyway?". Called by the game list and the savestate manager with the launch they
// are about to return.
//
// Like ES it runs `rocknix-systems --filter <system>` synchronously (a python script, tenths of a second on the console), but
// only when the setting is on and the script exists; with the setting off (the default) it costs one settings lookup.
#pragma once
#include <functional>
#include <string>
#include <string_view>

#include "app.hpp"

namespace zf::launchcheck {

/// `rocknix-systems` output (`> <system>` then `STATUS md5 path`): true when `system` has a line with status MISSING.
bool bios_missing(std::string_view output, std::string_view system);

/// False when CheckBiosesAtLaunch is on, `rocknix-systems` exists and `sys` (index in `cx.systems`) has MISSING BIOS files.
/// Synchronous (runs the script), so call it only when about to launch.
bool bios_ok(Ctx& cx, size_t sys);
/// The YES/NO question; YES runs `go` (which returns the launch).
Transition ask(std::function<Transition(Ctx&)> go);

/// `launch` unchanged when the game may start; otherwise the question whose YES returns the same launch. For launches that have
/// done nothing yet: a caller that prepares files for the launch (savestate manager) must prepare them inside `go` of `ask`.
Transition guard(Ctx& cx, size_t sys, Transition launch);

}  // namespace zf::launchcheck
