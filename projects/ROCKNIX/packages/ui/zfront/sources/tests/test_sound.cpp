// Navigation sounds: nothing plays unless ES's EnableSounds is on and a player is installed; each cue maps to the wav of the Zenith
// theme; the carousel / list / menus fire the cues of ES (scroll on a real move only, back, menu open, launch).
#include <cstdlib>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "screens.hpp"
#include "sound.hpp"

using namespace zf;

namespace {

std::vector<std::pair<sound::Cue, std::string>> g_played;

std::string theme_dir() {
  const char* e = std::getenv("ZF_THEME_DIR");
  return e ? e : "/home/epsil/zenith-frontend/theme-dark";
}

bool setup(Ctx& cx) {
  std::string err;
  Theme th;
  if (!Theme::load(theme_dir(), ratio_name(720, 720), th, err) || !cx.assets.load_font(th.path(th.str("font")), err)) {
    std::fprintf(stderr, "test_sound: theme not found (%s): test skipped\n", err.c_str());
    return false;
  }
  cx.th = th;
  cx.w = cx.h = 720;
  cx.sv->cmd.dry_run = true;
  cx.conf = SystemConf::from_text("", "/tmp/zf-sound-system.cfg");
  cx.items.push_back(SystemItem{"a", "A", 1});
  cx.items.push_back(SystemItem{"b", "B", 1});
  cx.items.push_back(SystemItem{"c", "C", 1});
  return true;
}

size_t count(sound::Cue c) {
  size_t n = 0;
  for (const auto& p : g_played)
    if (p.first == c) ++n;
  return n;
}

}  // namespace

int main() {
  CHECK_EQ(std::string(sound::cue_file(sound::Cue::Scroll)), std::string("assets/sounds/scroll.wav"));
  CHECK_EQ(std::string(sound::cue_file(sound::Cue::MenuOpen)), std::string("assets/sounds/menuOpen.wav"));

  Ctx cx;
  if (!setup(cx)) return 0;
  sound::set_player([](sound::Cue c, const std::string& p) { g_played.push_back({c, p}); });

  // off by default (ES default): no cue
  CHECK(!sound::enabled(cx));
  sound::play(cx, sound::Cue::Scroll);
  CHECK(g_played.empty());

  // on: the wav of the theme
  cx.es = EsSettings::from_text("<config><bool name=\"EnableSounds\" value=\"true\" /></config>");
  CHECK(sound::enabled(cx));
  sound::play(cx, sound::Cue::Scroll);
  CHECK_EQ(g_played.size(), size_t(1));
  CHECK(g_played.size() == 1 && g_played[0].second == cx.th.path("assets/sounds/scroll.wav"));

  // the carousel: one scroll cue per step, none when the keys are on a row or nothing moves
  g_played.clear();
  CarouselScreen c(0);
  c.handle(Action::Right, cx);
  c.handle(Action::Left, cx);
  CHECK_EQ(count(sound::Cue::Scroll), size_t(2));
  g_played.clear();
  c.handle(Action::Select, cx);  // not a move
  CHECK(g_played.empty());

  // switching it off acts at once
  cx.es.set_bool("EnableSounds", false);
  c.handle(Action::Right, cx);
  CHECK(g_played.empty());

  // the player can be removed
  cx.es.set_bool("EnableSounds", true);
  sound::set_player(nullptr);
  c.handle(Action::Right, cx);
  CHECK(g_played.empty());
  return zt::done();
}
