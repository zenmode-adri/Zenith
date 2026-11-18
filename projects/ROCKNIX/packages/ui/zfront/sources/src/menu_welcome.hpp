// First-boot welcome screen (`menu.welcome`): the screen on top of the logic in welcome.hpp.
#pragma once
#include <memory>

#include <vector>

#include "app.hpp"
#include "i18n.hpp"
#include "welcome.hpp"

namespace zf {

/// True when the welcome must appear now (see `welcome::should_show`): called once at start, before the loop.
bool welcome_due(Ctx& cx);

/// The languages the first step offers: English plus every language that has zfront's OWN catalog (`zfront.mo`) in `opts.dirs`.
/// A language that only ES translates (emulationstation2.mo) is not offered.
std::vector<welcome::LangName> welcome_languages(const i18n::Options& opts);

/// The first step of the welcome. `forced` = "Run welcome again": ignores `zfront.welcome_done` and the signs of use.
std::unique_ptr<Screen> make_welcome(Ctx& cx, bool forced);

}  // namespace zf
