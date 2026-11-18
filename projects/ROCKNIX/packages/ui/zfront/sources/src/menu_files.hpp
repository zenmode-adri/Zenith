// The file manager (menu_files.cpp): the one thing the rest of zfront needs to know about it.
#pragma once
#include <string>
#include <string_view>

namespace zf {

class Screen;
struct Ctx;

/// The path of a game of the TOOLS system is the script that starts the file manager of the image (`fileman.sh`, or `commander.sh` on builds that
/// carry the other one): A on it opens `menu.files` instead of running a program. The whole file name has to match.
bool is_file_manager_script(std::string_view path);

/// Test seam (tests/test_files_screen.cpp), `s` being a `menu.files`: `what` = "confirm" (text of the last question about deleting or renaming),
/// "state" ("ok", "empty", "unreadable"), "sort", "hidden", "clip" (how many paths are on the clipboard), "cursor", "refill" (reads the folder again),
/// "busy:1" / "busy:0" (pretends another job is or is not running).
std::string files_debug_for_test(Screen& s, Ctx& cx, const std::string& what);

}  // namespace zf
