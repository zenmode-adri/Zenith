#include "sound.hpp"

#include <map>
#include <utility>

#include "util.hpp"

namespace zf {
namespace sound {

namespace {

Player g_player;

// theme folder + cue -> absolute path of the wav ("" when the theme does not have it): the files are looked for once
std::map<std::pair<std::string, int>, std::string>& cache() {
  static std::map<std::pair<std::string, int>, std::string> c;
  return c;
}

}  // namespace

void set_player(Player p) { g_player = std::move(p); }

bool enabled(const Ctx& cx) { return cx.es.get_bool("EnableSounds", false); }

const char* cue_file(Cue cue) {
  switch (cue) {
    case Cue::Scroll: return "assets/sounds/scroll.wav";
    case Cue::Back: return "assets/sounds/back.wav";
    case Cue::MenuOpen: return "assets/sounds/menuOpen.wav";
    case Cue::Launch: return "assets/sounds/launch.wav";
  }
  return "";
}

void play(const Ctx& cx, Cue cue) {
  if (!g_player || !enabled(cx)) return;
  auto key = std::make_pair(cx.th.dir(), int(cue));
  auto it = cache().find(key);
  if (it == cache().end()) {
    std::string p = cx.th.path(cue_file(cue));
    it = cache().emplace(std::move(key), file_exists(p) ? p : std::string()).first;
  }
  if (!it->second.empty()) g_player(cue, it->second);
}

}  // namespace sound
}  // namespace zf
