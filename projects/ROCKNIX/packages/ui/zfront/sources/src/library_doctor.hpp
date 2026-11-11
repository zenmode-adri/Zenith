// MODULE `library-doctor`: the read-only scan behind LIBRARY > CHECK LIBRARY (`menu.library_doctor`).
//
// `scan` looks at the systems it is given and reports, never changes anything on disk:
//   * possibly broken files: 0-byte files, .zip / .7z whose first bytes are not the archive signature;
//   * duplicates: the same name stem (lower case, no extension, no (region) / [tag]) more than once in a system;
//   * games without a picture: the gamelist <image> is missing or the file is not on disk (count per system).
// File contents are never hashed (a PSP ISO is ~1 GB): only sizes and the first bytes are read.
#pragma once
#include <string>
#include <vector>

#include "systems.hpp"

namespace zf::doctor {

struct Problem {
  std::string system;  // full name of the system
  std::string file;    // file name (duplicates: the shared stem)
  std::string detail;  // broken: "EMPTY FILE" / "NOT A VALID ARCHIVE" (translatable keys); duplicates: number of files as text
};
struct Count {
  std::string system;
  size_t n = 0;
};
struct Report {
  std::vector<Problem> broken, duplicates;
  std::vector<Count> no_image;
  size_t games = 0;  // games looked at
  std::vector<std::string> with_games;  // names of the game systems that have at least one game
};

/// Name used to compare games: lower case, extension, `(...)` and `[...]` tags and extra spaces removed. "(Disc 2)"-like tags stay.
std::string dup_stem(const std::string& file_name);
/// Systems that hold no games (screenshots viewer, tools, music player): left out of the report.
bool is_game_system(const System& sys);
/// The BIOS summary of `rocknix-systems`, kept to the systems of `with_games` (it listed every system the image knows).
std::vector<std::pair<std::string, size_t>> bios_for(const std::vector<std::pair<std::string, size_t>>& all, const std::vector<std::string>& with_games);
/// Reads the `systems` (calls `open()` on each: pass a COPY from a job thread). Does not write.
Report scan(std::vector<System> systems);

}  // namespace zf::doctor
