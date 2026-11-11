// Navigation sounds: the four cues of the Zenith theme (`assets/sounds/*.wav`), played only when ES's `EnableSounds` is on.
//
// The core knows nothing about audio: `play` resolves the wav of the cue in the current theme and hands it to the player the
// device loop installed (sdlr.cpp: SDL audio, opened on demand and closed when idle). Without a player (tests, the PC preview)
// nothing happens. `EnableSounds` is read from `cx.es` at every cue, so the switch of the sound menu acts at once.
#pragma once
#include <functional>
#include <string>

#include "app.hpp"

namespace zf {
namespace sound {

enum class Cue { Scroll, Back, MenuOpen, Launch };

using Player = std::function<void(Cue cue, const std::string& wav_path)>;

/// Installs (or, with an empty function, removes) the player. Main thread.
void set_player(Player p);

/// ES `EnableSounds`, default off like ES.
bool enabled(const Ctx& cx);

/// File of a cue inside the theme folder, relative: `assets/sounds/<name>.wav`.
const char* cue_file(Cue cue);

/// Plays the cue when sounds are on, a player exists and the theme has the file. Cheap otherwise (one settings lookup).
void play(const Ctx& cx, Cue cue);

}  // namespace sound
}  // namespace zf
