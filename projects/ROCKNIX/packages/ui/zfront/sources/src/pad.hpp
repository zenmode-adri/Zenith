// MODULE `pad` (owner: pad agent). Gamepads: es_input.cfg mapping, dead zone, hat/axis decoding, key repeat, hot-plug.
// Contract with the rest: the SDL loop (sdlr.cpp) forwards every SDL event it does not handle itself to
// `Pad::on_event` and appends the returned `Action`s to the frame's input; it sleeps until `next_deadline()` (nothing
// held => nullopt => the process sleeps on SDL events alone) and calls `repeat_due(now)` after waking.
//
// `PadCore` is the pure logic (no SDL, testable on the PC). The SDL glue (SDL_Joystick open/close, GUID strings,
// event decoding) is `SdlPad` inside pad.cpp, so no other file includes SDL for input.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "input.hpp"
#include "util.hpp"

namespace zf {

/// A joystick as SDL sees it; `launch::build_command` turns the list into `-pNindex/-pNguid` arguments.
struct JoyInfo {
  uint32_t index = 0;
  std::string guid;
  std::string name;
  /// Counts SDL reports for pads that are open (-1: unknown). ES's mapping wizard uses them to decide which rows may be skipped.
  int buttons = -1, hats = -1, axes = -1;
};

/// Key-repeat schedule: stages of (duration_ms, delay_ms); the first step is immediate, duration UINT32_MAX = forever.
struct Stage {
  uint32_t duration_ms;
  uint32_t delay_ms;
};
struct Profile {
  const Stage* stages;
  size_t count;
};
/// ES game lists and grids: 0.5 s, then every 114 ms, 32 ms after 2.5 s, 16 ms after 6.5 s.
extern const Profile kQuick;
/// ES system carousel, menus and tabs: 0.5 s, then every 200 ms.
extern const Profile kSlow;

enum class InKind : uint8_t { Button, Hat, Axis };
using DevId = size_t;

/// A joystick event before any mapping, as the mapping wizard needs it (ES's `Input`): Button 1 down / 0 up; Hat the SDL
/// mask (up 1, right 2, down 4, left 8; 0 = centred); Axis -1 / 0 / +1 once the resting value and the dead zone are applied
/// (only the crossings of the dead zone are reported). `guid`/`name` outlive the call.
struct CaptureEvent {
  DevId dev;
  InKind kind;
  uint16_t id;
  int value;
  const std::string* guid;
  const std::string* name;
};
/// Returns true when it consumed the event (no `Action` is produced for it), false to let the normal mapping run.
using CaptureSink = std::function<bool(const CaptureEvent&)>;

class Pad {
 public:
  virtual ~Pad() = default;
  /// Called once SDL's joystick subsystem is initialised: open the devices already plugged in.
  virtual void attach() {}
  /// `sdl_event` points to an `SDL_Event`. Returns true if the event was a joystick event (consumed).
  virtual bool on_event(const void* sdl_event, TimePoint now, std::vector<Action>& out) {
    (void)sdl_event, (void)now, (void)out;
    return false;
  }
  /// When a held direction repeats next; nullopt when nothing is held.
  virtual std::optional<TimePoint> next_deadline() const { return std::nullopt; }
  /// Appends the repeat steps due at `now` (at most 4: a stalled loop resynchronises instead of bursting).
  virtual void repeat_due(TimePoint now, std::vector<Action>& out) { (void)now, (void)out; }
  /// Close every open joystick handle: called right before the game runs (SDL is then shut down);  reopens.
  virtual void detach() {}
  /// Forget held buttons (after a game, when the window comes back).
  virtual void clear() {}
  /// Devices currently open.
  virtual std::vector<JoyInfo> joysticks() const { return {}; }
  /// Repeat schedule for the current screen (default kQuick; carousel and menus ask for kSlow).
  virtual void set_profile(Profile p) { (void)p; }
  /// ES `InvertButtons` (swaps A/B) changed in the settings menu: applies at once.
  virtual void set_invert_buttons(bool invert) { (void)invert; }
  /// Raw-event tap for the controller mapping wizard (menu_controllers_*): while a sink is set, every decoded joystick event
  /// is offered to it first. An empty function removes it. Installing or removing it forgets any held direction.
  virtual void set_capture(CaptureSink sink) { (void)sink; }
  /// Re-read es_input.cfg (after the wizard saved it) and recompute the mapping of the open pads.
  virtual void reload_config() {}
  /// True once after a button/hat press from a pad that has no mapping at all (neither es_input.cfg nor the SDL reserve
  /// one) while no capture sink was set: the loop opens the controller detection screen, as ES does.
  virtual bool take_unconfigured() { return false; }
  /// Hot-plug tap (ES `ShowControllerNotifications`): called on the main thread with (true, name) when a pad is plugged in
  /// and (false, name) when one is removed, only for events seen while running (not for the pads open at start-up or
  /// reopened after a game). The name is trimmed. An empty function removes it.
  using PlugSink = std::function<void(bool added, const std::string& name)>;
  virtual void set_plug_sink(PlugSink sink) { (void)sink; }
};

// ---------------------------------------------------------------------------------------------------------------
// Pure logic

/// SDL axis dead zone used by ES (InputManager.h): an axis event is produced only when it crosses it.
constexpr int kDeadZone = 23000;

/// A decoded joystick event. Hat: SDL mask (up 1, right 2, down 4, left 8; 0 = centred). Axis: raw -32768..32767.
struct RawEvent {
  enum Kind : uint8_t { Button, Hat, Axis } kind;
  DevId dev;
  uint16_t id;
  int32_t value;  // Button: 1 down / 0 up
};

/// All `<inputConfig type="joystick">` entries in lookup priority order (user file before the system one).
class PadConfig {
 public:
  struct Input {
    std::string name;  // lower case
    InKind kind;
    uint16_t id;
    int value;
  };
  struct Device {
    std::string name, guid;  // guid lower case
    std::vector<Input> inputs;
  };

  /// Parse es_input.cfg text and append its joystick entries; broken entries are skipped. False on malformed XML.
  bool parse(const std::string& xml);
  /// User config first, then the one next to the executable (ES's order).
  static PadConfig load_default();
  bool load_file(const std::string& path);
  size_t size() const { return devices_.size(); }
  const Device& device(size_t i) const { return devices_[i]; }
  /// ES order: GUID+name, GUID(bytes 4..7 zeroed)+name, GUID, name. -1 when nothing matches.
  int find(const std::string& guid, const std::string& name) const;

  /// Reserve mapping for a pad without an es_input.cfg entry: converts an SDL game controller mapping string
  /// (`guid,name,a:b0,dpup:h0.1,lefttrigger:a2,...`, from `SDL_GameControllerMappingForDeviceIndex`) into the same
  /// structure as a cfg entry, with ES's own SDL -> ES name table (`InputManager::loadFromSdlMapping`): SDL a/b and x/y
  /// are crossed, shoulders become pageup/pagedown, triggers l2/r2, stick clicks l3/r3, select doubles as hotkey. The
  /// left stick becomes leftanalog* (ES leaves it unmapped; here it also navigates). `+aN`, `-aN` (half axes of a
  /// d-pad) and `aN~` (inverted axis) are understood. False when nothing in the string is usable.
  static bool from_sdl_mapping(std::string_view mapping, Device& out);

 private:
  std::vector<Device> devices_;
};

class PadCore : public Pad {
 public:
  explicit PadCore(PadConfig cfg, bool invert_buttons = true);

  void set_profile(Profile p) override { profile_ = p; }
  void set_invert_buttons(bool invert) override;
  void set_capture(CaptureSink sink) override;
  void set_plug_sink(PlugSink sink) override { plug_ = std::move(sink); }
  void reload_config() override { set_config(PadConfig::load_default()); }
  /// Replaces the es_input.cfg data and recompiles the mapping of every open pad.
  void set_config(PadConfig cfg);
  /// Register a plugged-in device. `axis_init`: resting value of each axis when it was opened (triggers rest at
  /// -32767 and must not read as pressed).
  DevId add_device(const std::string& guid, const std::string& name, const int16_t* axis_init, size_t n_axes);
  void remove_device(DevId d);
  /// True when the pad has an es_input.cfg entry (not just the SDL reserve mapping).
  bool is_configured(DevId d) const;
  /// Gives a pad without a cfg entry the reserve mapping built from its SDL mapping string; true if it is usable.
  /// Does nothing (false) when the pad has a cfg entry. Survives `set_invert_buttons`.
  bool set_sdl_mapping(DevId d, std::string_view mapping);
  /// 0: no mapping (the pad does nothing), 1: es_input.cfg, 2: SDL reserve mapping.
  int mapping_source(DevId d) const;

  /// Hot path: never allocates once `out` has grown.
  void event(const RawEvent& e, TimePoint now, std::vector<Action>& out);
  std::optional<TimePoint> next_deadline() const override { return hold_ ? std::optional<TimePoint>(hold_->next) : std::nullopt; }
  void repeat_due(TimePoint now, std::vector<Action>& out) override;
  void clear() override;
  bool take_unconfigured() override {
    bool u = unconfigured_;
    unconfigured_ = false;
    return u;
  }

 private:
  struct Bind {
    InKind kind;
    uint16_t id;
    int value;  // hat: SDL mask; axis: sign; button: unused
    Action action;
  };
  static constexpr size_t kMaxAxes = 16, kMaxHats = 4;
  struct Dev {
    bool used = false;
    std::string guid, name;  // as given to add_device (the capture hook reports them)
    int cfg = -1;
    bool has_sdl = false;
    PadConfig::Device sdl;  // reserve mapping, used only while `cfg < 0`
    std::vector<Bind> binds;
    int32_t axis_init[kMaxAxes] = {};
    int32_t axis_prev[kMaxAxes] = {};
    uint8_t hat_prev[kMaxHats] = {};
  };
  struct Src {
    DevId dev;
    InKind kind;
    uint16_t id;
    bool operator==(const Src& o) const { return dev == o.dev && kind == o.kind && id == o.id; }
  };
  struct Hold {
    Src src;
    Action action;
    TimePoint t0, next;
  };

  void compile(Dev& d) const;
  void press(Src s, Action a, TimePoint now, std::vector<Action>& out);
  void release(Src s, Action a);

  bool capture_event(const RawEvent& e, Dev& d);

  PadConfig cfg_;
  bool invert_;
  CaptureSink capture_;
  Profile profile_;
  std::vector<Dev> devs_;
  std::optional<Hold> hold_;
  bool unconfigured_ = false;

 protected:
  PlugSink plug_;
  /// Name of an open device ("" when unknown), for the removal notice.
  const std::string& device_name(DevId id) const;
};

/// Delay until the next repeat step given the time since the first press (stage boundaries are cumulative).
uint32_t repeat_delay_at(Profile p, uint32_t elapsed_ms);

/// `es_settings_path`: where `InvertButtons` is read from (A/B swap), `es_input.cfg` is found by the pad itself.
/// Without SDL (PC build) the result is a `PadCore` with no devices.
std::unique_ptr<Pad> make_pad(const std::string& es_settings_path);

}  // namespace zf
