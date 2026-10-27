// "Continue playing": the most recent savestate screenshots of a system, for the row under the carousel.
//
// The data is what RetroArch already left on the card: `<state>.png` next to each savestate (see savestates.hpp), so a tile
// exists only for a game that really has a capture. Nothing is read from gamelist.xml (the carousel does not load it).
// `recent_states` lists one folder (a few `stat`s), so the carousel calls it once when it stops on a system, not per frame.
#pragma once
#include <ctime>
#include <string>
#include <vector>

#include "systems.hpp"

namespace zf {

struct RecentItem {
  std::string shot;  // absolute path of the png
  std::string name;  // rom file stem
  std::string ago;   // "12 min ago"; empty when older than 30 days
};

/// "12 min ago" / "3 h ago" / "2 d ago"; empty when `then` is in the future or more than 30 days before `now`.
std::string ago_text(time_t then, time_t now);

/// A rom file stem without its tags: "God of War (USA)" -> "God of War", "Tekken [!]" -> "Tekken"; never empty.
std::string short_title(const std::string& stem);

/// Newest first, at most `max` games (one tile per game, whatever its slot).
std::vector<RecentItem> recent_states(const System& sys, time_t now, size_t max);

}  // namespace zf
