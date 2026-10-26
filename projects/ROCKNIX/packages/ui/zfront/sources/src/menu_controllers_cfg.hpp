// Pure logic of the controller section (no drawing, no SDL): the es_input.cfg writer, the mapping wizard state machine
// (ES's GuiInputConfig), the parsers of `rocknix-bluetooth` output and the small probes (rumble sysfs, tool present).
// The screens that use it are in menu_controllers*.cpp. Paths are injectable so the tests never touch /storage or /run.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pad.hpp"
#include "services.hpp"

namespace zf::ctl {

// ---- paths (tests point them at a temp directory) -------------------------------------------------------------------
struct Paths {
  /// ES: Paths::getUserEmulationStationPath() + "/es_input.cfg" (`es_last_input.cfg` goes next to it).
  std::string user_input = "/storage/.config/emulationstation/es_input.cfg";
  /// `isScriptingSupported(BLUETOOTH)` in ES: the script must exist in /usr/bin.
  std::string bt_tool = "/usr/bin/rocknix-bluetooth";
  /// Where we send the output of `rocknix-bluetooth live_devices` (the script prints the agent's device lines and its own
  /// errors there); read by the pairing screen.
  std::string bt_live_log = "/run/zfront-bt-live.log";
  /// ApiSystem::getRumblePath looks for these directories under it.
  std::string sysfs_platform = "/sys/devices/platform";
};
Paths& paths();

// ---- es_input.cfg ---------------------------------------------------------------------------------------------------
/// One `<input name type id value/>` of an `<inputConfig>`. `Key` only appears as the "no hotkey" marker (id 0).
struct Mapping {
  enum Type : uint8_t { Button, Hat, Axis, Key } type = Button;
  std::string name;  // lower case, as ES writes it
  int id = 0;
  int value = 1;
};

/// ES `InputManager::writeDeviceConfig`: edits the user file's DOM (every other device, keyboard entries, `inputAction`
/// blocks and comments stay as they are), removes the old entry of the same GUID + name, appends the new `<inputConfig
/// type="joystick">` with the inputs sorted by name, and saves atomically. A file that does not parse is kept as `<path>.bad`.
/// Also writes `es_last_input.cfg` (only this device) next to it. False with `err` on I/O failure.
bool write_device_config(const std::string& path, const std::string& guid, const std::string& name, std::vector<Mapping> maps, std::string& err);
/// Commands of `<inputAction type="onfinish">` in the user file (ES runs them after saving).
std::vector<std::string> onfinish_commands(const std::string& path);
/// The inputs of the entry of this GUID + name in `path` (exact match, ES's first lookup), empty when absent.
std::vector<Mapping> read_device_config(const std::string& path, const std::string& guid, const std::string& name);

// ---- the wizard -----------------------------------------------------------------------------------------------------
struct Slot {
  const char* name;   // es_input.cfg name
  const char* label;  // tr() key, ES's text
  bool skippable;     // table default (see `InputWizard::skippable` for the per-pad rule)
};
constexpr size_t kSlotCount = 25;
extern const Slot kSlots[kSlotCount];

enum class RowState : uint8_t { Pending, Set, Skipped };
enum class Hotkey { Select, None };

/// ES's GuiInputConfig for one pad, always in "configure everything" mode: row after row, hold a button for 1 s to skip
/// the rows that may be skipped, "ALREADY TAKEN" when an input is used by another row, the DualShock quirk (a button
/// held together with a single moving axis becomes that axis). Time arrives through `tick`, input through `input`.
class InputWizard {
 public:
  enum class Phase : uint8_t { Rows, Review };
  /// `caps` of the pad (-1: unknown) decide which rows are skippable exactly as ES does.
  explicit InputWizard(int buttons = -1, int hats = -1, int axes = -1);

  Phase phase() const { return phase_; }
  size_t row() const { return row_; }
  RowState state(size_t i) const { return st_[i]; }
  const Mapping* mapping(size_t i) const { return st_[i] == RowState::Set ? &map_[i] : nullptr; }
  bool skippable(size_t i) const { return skip_[i]; }
  /// The last input was refused because another row uses it (cleared by the next input).
  bool taken() const { return taken_; }
  /// A button/stick is down on the current row. `hold_progress` is 0..1 towards the skip, only for skippable rows.
  bool holding() const { return holding_; }
  float hold_progress() const;
  /// Anything changed since the last call (for the screen: redraw).
  bool take_changed() {
    bool c = changed_;
    changed_ = false;
    return c;
  }

  /// Raw input of the target pad (`value`: button 1/0, hat mask, axis sign; 0 = released). Ignored in Review.
  void input(InKind kind, uint16_t id, int value);
  void tick(float dt);
  /// Skips the current row when it may be skipped (keyboard shortcut). True if it did.
  bool skip_row();
  /// Goes back one row (keyboard shortcut) and forgets its mapping. True if it moved.
  bool back_row();

  /// Review: the logical name ("a", "b", "x"...) a pressed input means under the NEW mapping, or nullptr.
  const char* resolve(InKind kind, uint16_t id, int value) const;
  bool has_hotkey() const { return st_[20] == RowState::Set; }
  bool has_select() const { return st_[5] == RowState::Set; }
  /// Everything assigned so far, plus the hotkey as chosen (when row `hotkeyenable` is empty): Select copies the select
  /// input; None writes ES's disabled marker (`key` id 0). A pad that has no select gets the marker either way.
  std::vector<Mapping> result(Hotkey choice) const;
  /// How many rows are set / skipped.
  size_t count(RowState s) const;

 private:
  void row_done();
  bool assign(const Mapping& m);
  bool matches(const Mapping& have, InKind kind, uint16_t id, int value) const;

  Phase phase_ = Phase::Rows;
  size_t row_ = 0;
  RowState st_[kSlotCount];
  Mapping map_[kSlotCount];
  bool skip_[kSlotCount];
  bool taken_ = false, holding_ = false, changed_ = false;
  float held_s_ = 0;
  Mapping held_;
  struct Seen {
    InKind kind;
    uint16_t id;
    int value;
  };
  std::vector<Seen> seen_;  // everything that arrived while holding (DualShock rule)
};

constexpr float kHoldSeconds = 1.0f;  // ES HOLD_TO_SKIP_MS / HOLD_TIME

/// Text of an input like ES's `Input::string()` ("Button 3", "Axis 1-", "Hat 0 Up", "Key" for the hotkey marker).
std::string describe(const Mapping& m);

// ---- bluetooth ------------------------------------------------------------------------------------------------------
struct BtDevice {
  std::string id, name, type;  // type: "joystick", "audio", "unknown"...
  bool connected = false;
};
/// `<device id=".." name=".." type=".." connected="yes|no" />` lines of `rocknix-bluetooth list`.
std::vector<BtDevice> parse_bt_list(std::string_view text);
/// Applies the `status="added|removed"` lines of the live listing, in order, to `devs` (keyed by id).
void apply_bt_live(std::vector<BtDevice>& devs, std::string_view text);
/// The script's own check: six upper-case hex pairs separated by ':'.
bool valid_mac(std::string_view s);

// ---- probes ---------------------------------------------------------------------------------------------------------
/// ES getRumblePath: `<sysfs>/*retroid-pocket-gamepad*` or `*rocknix*` directory that has `rumble_enable`; empty when none.
std::string rumble_path();
bool bt_available();

}  // namespace zf::ctl
