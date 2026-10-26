// MODULE `menu-collections` (savestates, data and launch side). The same files, names and RetroArch arguments as ES's
// SaveStateRepository / SaveState / SaveStateConfigFile (default configuration, `es_savestates.cfg` absent on the console):
//
//   dir      <ROM folder's parent>/savestates/<system>          (/storage/roms/savestates/<system>)
//   state    <rom stem>.state<slot>   slot 0 -> "<stem>.state", slot n -> "<stem>.state<n>"
//   autosave <rom stem>.state.auto
//   shot     <state file>.png
//
// Launch arguments appended to the runemu.sh command (runemu.sh only reads `-state_slot N` and `-autosave 0|1`; `-state_file`
// is ignored by the OS, so it is never produced):
//   plain launch      autosave setting "1": " -autosave 1 -state_slot N"; else " -state_slot N" when states exist (incremental)
//   START NEW GAME    " -autosave 0 -state_slot N"
//   AUTO SAVE         " -autosave 1 -state_slot N"
//   a state           " -state_slot N -autosave 1" (the state is copied over "<stem>.state.auto" first; "<stem>.state<N>" gets a
//                     copy too, so the game can be saved without overwriting it)
// N = highest slot + 1 (0 when there is none). Deviation from ES: when the folder holds only an autosave ES computes
// N = -99 (its loop never finds a slot); here N is 0.
//
// `begin` does the file preparation (a few MB of copying) and remembers what `after_game` must undo once the game ended
// (called from `launch::apply_after`, one line there): restore the previous autosave, drop the extra copy if the game did not
// change it, and renumber the slots without holes.
//
// Directory listings are cached by (inode, mtime) of the folder: `plain_args` runs for every cursor move (the launch
// preparation asks for the command), so it costs one `stat` unless the folder changed.
#pragma once
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "app.hpp"

namespace zf::savestates {

struct State {
  int slot = 0;       // -1 = autosave, >= 0 = slot
  std::string path;   // state file
  std::string shot;   // "<path>.png" when it exists, else empty
  time_t mtime = 0;   // filled by `with_times` (the cached listing has no times)
};

/// "<stem>.state", "<stem>.state3", "<stem>.state.auto" -> stem and slot; false for anything else (screenshots...).
bool parse_name(std::string_view file, std::string& stem, int& slot);

std::string state_dir(const System& sys);
std::string state_path(const std::string& dir, const std::string& stem, int slot);

/// A game gets the savestate machinery when it runs on RetroArch (ES `SaveStateRepository::isEnabled`) and the console has no
/// custom `es_savestates.cfg`.
bool enabled(const System& sys, const Game& g, const ConfMap& conf);
/// Game, then system, then global setting (ES `getCurrentGameSetting`): `<sys>["<file>"].<k>`, `<sys>.<k>`, `global.<k>`.
std::string setting(const ConfMap& conf, const System& sys, const Game& g, const std::string& key);
/// `global.incrementalsavestates` not "0" and not "2".
bool incremental(const ConfMap& conf);
/// Highest slot + 1, or 0 when there is none.
int next_free_slot(const std::vector<State>& states);

/// Savestates of a game, slots ascending (autosave first). Cached listing.
std::vector<State> list(const System& sys, const Game& g);
/// Same with the modification times (one `stat` per file): what the manager shows.
void with_times(std::vector<State>& v);
void invalidate();

enum class Pick { None, NewGame, Auto, Slot };

/// What `begin` leaves for `after_game`.
struct Plan {
  bool active = false;
  std::string dir, stem;
  bool from_slot = false;  // a state was loaded: renumber when done
  std::string auto_file, auto_png;
  bool auto_saved = false, png_saved = false;  // a ".bak" was made
  std::string new_slot_file;                   // extra copy of the loaded state
  uint64_t new_slot_hash = 0;
};

/// Arguments to append to the launch command (leading space, or empty) for `pick` (`slot` for Pick::Slot). Does the file
/// preparation of loading a state and stores the plan for `after_game`.
std::string begin(const System& sys, const Game& g, const ConfMap& conf, Pick pick, int slot);
/// Same arguments without touching any file (plain launches and the idle-time preparation). Empty for games without savestates.
std::string plain_args(const System& sys, const Game& g, const ConfMap& conf);
/// Undo of `begin` after the game exited (idempotent; nothing when no plan is active).
void after_game();
bool plan_active();

/// Deletes the state and its screenshot, then renumbers. `copy_to_free_slot` copies it to the next free slot.
bool remove_state(const System& sys, const Game& g, const State& s);
bool copy_to_free_slot(const System& sys, const Game& g, const State& s);
/// Moves slots down to 0..n-1 without holes (ES `renumberSlots`).
void renumber(const std::string& dir, const std::string& stem);
/// A ".bak" left by a game that never reported back (crash, power cut) is put back.
void recover(const System& sys, const Game& g);

/// Bumped every time files changed behind a screen's back (after_game, delete, copy): the manager reloads when it differs.
unsigned generation();

// ---- screen glue ---------------------------------------------------------------------------------------------------------------
/// Screen argument: "<system name>|<game path>".
std::string make_arg(const System& sys, const Game& g);
bool parse_arg(Ctx& cx, const std::string& arg, size_t& sys, size_t& game);
/// What the Y/A/L3 hooks of the list call. `launch_request`: the manager when the "savestates" setting asks for it ("1"; "2"
/// when the game has states), otherwise `Transition::none()`. `open_manager`: the manager if the game supports it, else none.
Transition launch_request(Ctx& cx, size_t sys, size_t game);
Transition open_manager(Ctx& cx, size_t sys, size_t game);
/// `savestates::plain_args` for game `game` of system `sys` of `cx` (empty on a bad index).
std::string plain_args(Ctx& cx, size_t sys, size_t game);

}  // namespace zf::savestates
