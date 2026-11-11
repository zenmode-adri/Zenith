// ES's PLAYER ASSIGNMENTS (GuiControllersSettings, non-Windows branch; InputManager::computePlayersConfigs): which connected pad is
// player 1 to 8 when a game starts. es_settings.cfg keeps, for player n, the strings `INPUT P<n>NAME`, `INPUT P<n>GUID` and
// `INPUT P<n>PATH` ("DEFAULT" and empty strings when the player has no pad of its own).
//
// The launch (`launch::controllers_config`) passes the pads in this order; the table of the process is set at start-up and by the
// menu (`set`), so no launch path needs the settings. With no assignment the order is the SDL connection order (ES sorts by the udev device path, which zfront does not have).
// The `INPUT P<n>PATH` that zfront stores is SDL's device path; one saved by ES (a sysfs parent path) does not match it, and the pad is then found by name.
#pragma once
#include <array>
#include <string>
#include <vector>

#include "pad.hpp"
#include "settings.hpp"

namespace zf::players {

constexpr size_t kSlots = 8;  // ES MAX_PLAYERS

struct Slot {
  std::string name, guid, path;
};
using Slots = std::array<Slot, kSlots>;

/// The `INPUT P<n>*` strings of es_settings.cfg.
Slots read(const EsSettings& es);
/// Table used by `launch::controllers_config` (main thread). Empty until `set`.
void set(const Slots& s);
const Slots& current();

/// ES `computePlayersConfigs`: `joys` as players 1..n. Order of preference per player: the saved device path, the saved name + GUID,
/// the saved name, then any pad that is left (SDL index order); a player left without a pad takes the pad of the highest
/// player after it.
std::vector<JoyInfo> order(const std::vector<JoyInfo>& joys, const Slots& slots);

/// What the player rows show as chosen: for each of the 8 players the index in `joys` of the pad that matches its saved values and
/// is not already taken by an earlier player, or -1 (default).
std::vector<int> chosen(const std::vector<JoyInfo>& joys, const Slots& slots);

/// Stores player `player` (0-based): the pad `pad`, or `nullptr` for the default ("DEFAULT", "", ""). Returns false when nothing changed.
bool assign(EsSettings& es, size_t player, const JoyInfo* pad);

}  // namespace zf::players
