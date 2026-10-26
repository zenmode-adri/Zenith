// Section `menu.controllers` ("Controller & Bluetooth"): the settings page, the Bluetooth screens and the controller mapping
// wizard. Screens registered (menu_controllers*.cpp):
//   menu.controllers            the page (ES GuiControllersSettings)
//   menu.controllers.detect     "hold a button on your device" (ES GuiDetectDevice)
//   menu.controllers.config     the mapping wizard (ES GuiInputConfig); arg = "<guid> <device name>"
//   menu.controllers.btpair     live scan + pair (ES GuiBluetoothPair)
//   menu.controllers.btdevices  paired devices (ES GuiBluetoothDevices)
//   menu.controllers.btdevice   connect / disconnect / forget; arg = "<mac> <connected 0|1> <name>" (ES GuiBluetoothDeviceOptions)
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app.hpp"
#include "menu.hpp"
#include "menu_controllers_cfg.hpp"
#include "ui_style.hpp"

namespace zf::ctl {

/// A `Menu` that can run a timer (poll something while it is open) and leave on its own (a job finished).
class CtlMenu : public Menu {
 public:
  struct Hooks {
    /// Seconds until `poll` must run next, or nullopt for no timer. Called after every tick.
    std::function<std::optional<float>()> next_poll;
    /// Runs on the main thread when the time is up (`Menu::refresher` of the menu is the way to rebuild).
    std::function<void(Ctx&, const Refresh&)> poll;
    /// Polled by `take_transition`: true pops the menu once.
    std::function<bool()> want_pop;
    /// Runs in the destructor (stop scans...).
    std::function<void()> on_destroy;
  };
  CtlMenu(MenuSpec spec, Hooks h) : Menu(std::move(spec)), h_(std::move(h)) {}
  ~CtlMenu() override {
    if (h_.on_destroy) h_.on_destroy();
  }
  bool tick(float dt, Ctx& cx) override;
  std::optional<float> wake_in() const override;
  Transition take_transition(Ctx& cx) override;

 private:
  Hooks h_;
  float left_ = -1;  // seconds to the next poll (-1: not armed)
};

// ---- the mapping screens ---------------------------------------------------------------------------------------------

/// Common bookkeeping of the two screens that tap the pad: installs the sink lazily (so a Replace that destroys the old
/// screen after building the new one cannot remove the new sink) and removes it only if it is still its own.
class PadTap {
 public:
  ~PadTap() { release(); }
  void install(Ctx& cx, CaptureSink sink);
  void release();

 private:
  Pad* pad_ = nullptr;
};

/// ES GuiDetectDevice: the first pad whose button is held for one second gets configured.
class DetectScreen : public Screen {
 public:
  explicit DetectScreen(Ctx& cx);
  ~DetectScreen() override = default;
  Transition handle(Action a, Ctx& cx) override;
  bool tick(float dt, Ctx& cx) override;
  std::optional<float> wake_in() const override;
  Transition take_transition(Ctx& cx) override;
  void draw(Ctx& cx, std::vector<Op>& out) override;

  /// The pad tap (public for tests): button events only, never consumed.
  bool on_capture(const CaptureEvent& e);
  bool holding() const { return holding_; }
  float progress() const { return held_s_ / kHoldSeconds; }

 private:
  PadTap tap_;
  bool installed_ = false;
  bool holding_ = false, picked_ = false, cancel_ = false, dirty_ = true;
  DevId held_dev_ = 0;
  std::string held_guid_, held_name_;
  float held_s_ = 0;
  // drawing
  ui::Metrics m_;
  ui::Style st_;
  ui::Shapes shapes_;
  ui::HelpBar help_;
  ui::Txt t_title_, t_count_, t_msg_, t_msg2_, t_held_;
  std::string shown_held_;
  size_t shown_count_ = size_t(-1);
  float lay_w_ = 0, lay_h_ = 0;
};

class ConfigScreen : public Screen {
 public:
  ConfigScreen(Ctx& cx, std::string guid, std::string name, int buttons = -1, int hats = -1, int axes = -1);
  Transition handle(Action a, Ctx& cx) override;
  bool tick(float dt, Ctx& cx) override;
  std::optional<float> wake_in() const override;
  Transition take_transition(Ctx& cx) override;
  void draw(Ctx& cx, std::vector<Op>& out) override;

  /// The pad tap (public for tests): consumes every event of the target pad.
  bool on_capture(const CaptureEvent& e);
  InputWizard& wizard() { return wiz_; }
  /// Writes the file, reloads the pad and fires ES's events. False (and a toast) on failure.
  bool save(Ctx& cx, Hotkey choice);
  bool saved() const { return saved_; }

 private:
  void review_press(const char* name);

  PadTap tap_;
  std::string guid_, name_;
  InputWizard wiz_;
  bool invert_ = true;
  // what the pad (in Review) or the keyboard asked for; acted on in take_transition (main thread, after tick)
  enum class Want : uint8_t { None, SaveSelect, SaveNone, Cancel } want_ = Want::None;
  bool saved_ = false;
  float idle_s_ = 0;  // seconds since the last input of the target
  // drawing
  ui::Metrics m_;
  ui::Style st_;
  ui::Shapes shapes_;
  ui::HelpBar help_;
  ui::Txt t_title_, t_dev_, t_label_, t_state_, t_note_, t_sum_;
  std::string shown_label_, shown_state_, shown_note_, shown_sum_;
  float lay_w_ = 0, lay_h_ = 0;
  bool drawn_once_ = false;
  bool needs_redraw_ = false;
};

/// Pads known to SDL right now (what `cx.pad` reports; the harness shows two fake ones when ZF_CTL_DEMO is set).
std::vector<JoyInfo> connected_pads(Ctx& cx);
/// True when `es_input.cfg` (user or system) has an entry for the pad.
bool pad_is_configured(const JoyInfo& j);

}  // namespace zf::ctl
