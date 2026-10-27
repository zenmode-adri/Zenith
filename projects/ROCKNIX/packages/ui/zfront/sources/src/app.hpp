// Application core: a stack of `Screen`s driven by `Action`s. Backend independent, so the SDL loop and the headless
// harness (`--script`) run exactly the same code.
//
// To add a screen (menu, dialog, OSD...): derive from `Screen` in its own .hpp/.cpp, return
// `Transition::push(std::make_unique<MyScreen>(...))` from the screen that opens it. A screen with
// `overlay() == true` is drawn on top of the one below it instead of replacing it.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gfx.hpp"
#include "input.hpp"
#include "pad.hpp"
#include "scan.hpp"
#include "scene.hpp"
#include "services.hpp"
#include "settings.hpp"
#include "status.hpp"
#include "systems.hpp"
#include "theme.hpp"

namespace zf {

struct Ctx {
  Theme th;
  Assets assets;
  Common common;
  std::vector<System> systems;
  std::vector<SystemItem> items;  // carousel items, same order as `systems`
  SystemConf conf;
  /// es_settings.cfg (UI options). Menus change it with set_* and `save()`.
  EsSettings es;
  /// Command runner (dry-run aware), toasts, background jobs. Shared so `Ctx` stays movable.
  std::shared_ptr<Services> sv = std::make_shared<Services>();
  /// Clock, battery and WiFi shown in the status bar (kept fresh by `App::poll_status`).
  Status status;
  /// Gamepad of the loop (may be null in the harness); `launch` asks it for the joystick list.
  Pad* pad = nullptr;
  float w = 0, h = 0;
  /// Device loop: covers are decoded on the job thread (covers.hpp). Harness and tests decode inline (deterministic).
  bool async_covers = false;
  /// Device loop: screens fade / slide in when pushed and fade out when popped (`App`). Off in the harness and the tests, so a
  /// frame built right after a key press is the final one.
  bool ui_motion = false;

  /// Hot reload of the system list (`App::maybe_reload_systems`): installed by whoever loaded the systems (main). Re-reads
  /// es_systems.cfg and the ROM folders into `out`, keeping the systems without games when `load_empty` (LoadEmptySystems). It must
  /// not capture the `Ctx` (it is moved into the `App`) nor read the settings itself: the caller passes what it needs. Empty: no
  /// hot reload (demo systems, tests), and the collections menu asks for a restart instead.
  std::function<bool(bool load_empty, std::vector<System>& out, std::string& err)> system_loader;

  /// Index in `systems` of carousel card `item` (cards and systems differ when some are hidden or sorted).
  size_t system_of(size_t item) const { return item < items.size() && items[item].sys != kItemIsSystem ? items[item].sys : item; }
  /// Carousel card of system `sys`, or -1 when it is not shown (hidden).
  long item_of(size_t sys) const;

  /// Loads the gamelist of system `i` (no-op when already loaded) and refreshes its carousel count.
  void open_system(size_t i);
};

class Screen;

struct Transition {
  enum Kind { None, Push, Pop, Replace, Launch, Quit, Open, CloseMenus } kind = None;
  std::unique_ptr<Screen> screen;  // Push / Replace
  std::string cmd;                 // Launch: shell command of the game, the loop blocks until it ends
  int sys = -1, game = -1;         // Launch: system index / game index (hooks, playcount); -1 when unknown
  std::string name, arg;           // Open: registered screen name (registry.hpp) and its argument

  static Transition none() { return {}; }
  static Transition of(Kind k) {
    Transition t;
    t.kind = k;
    return t;
  }
  static Transition push(std::unique_ptr<Screen> s);
  static Transition pop() { return of(Pop); }
  static Transition replace(std::unique_ptr<Screen> s);
  static Transition launch(std::string cmd) {
    Transition t = of(Launch);
    t.cmd = std::move(cmd);
    return t;
  }
  static Transition quit() { return of(Quit); }
  /// Pops every menu on top of the stack (screens with `is_menu()`), stopping at the first screen that is not one
  /// (carousel, game list...). Each popped screen gets `on_dismiss` first. Start inside a menu returns it.
  static Transition close_menus() { return of(CloseMenus); }
  /// Push the screen registered under `name` (ZF_REGISTER_SCREEN); a toast appears if it does not exist.
  static Transition open(std::string name, std::string arg = {});
};

class Screen {
 public:
  virtual ~Screen() = default;
  virtual Transition handle(Action a, Ctx& cx) = 0;
  /// Key-repeat schedule of a held direction on this screen: ES lists and grids use the quick one (it accelerates), its system
  /// carousel the slow one (a step every 200 ms after the first 0.5 s, never faster).
  virtual Profile repeat_profile() const { return kQuick; }
  /// Advance animations by `dt` seconds; return true while something is still moving (the loop keeps drawing).
  virtual bool tick(float dt, Ctx& cx) {
    (void)dt, (void)cx;
    return false;
  }
  /// Seconds until this screen needs `tick` again although nothing else happens (a timer); nullopt = sleep until an
  /// event. The loop sleeps that long instead of polling.
  virtual std::optional<float> wake_in() const { return std::nullopt; }
  /// True (once) when a timer fired inside `tick` and the frame must be redrawn without being an animation.
  virtual bool take_redraw() { return false; }
  /// A transition the screen wants WITHOUT a key press (a job finished, a hold completed, a pad event consumed by the mapping
  /// wizard): asked right after `tick`, only Push / Pop / Replace / Open are honoured. `Transition::none()` by default.
  virtual Transition take_transition(Ctx& cx) {
    (void)cx;
    return Transition::none();
  }
  /// Append this frame's operations to `out`.
  virtual void draw(Ctx& cx, std::vector<Op>& out) = 0;
  /// Draws the screen into `out` only to have its images made (`App::warm`): it must not start anything (jobs, commands). A screen
  /// whose `draw` does more than draw overrides it.
  virtual void warm(Ctx& cx, std::vector<Op>& out) { draw(cx, out); }
  /// Drawn above the screen below it instead of replacing it.
  virtual bool overlay() const { return false; }
  /// True for the screens `Transition::close_menus` removes (the menu framework's `Menu` and its subclasses).
  virtual bool is_menu() const { return false; }
  /// Called on each screen `close_menus` pops, top first, before it is destroyed (flush pending edits, `on_close`).
  virtual void on_dismiss(Ctx& cx) { (void)cx; }
  /// True for a non-menu overlay that must be gone when a game starts from it (the savestate manager: ES closes it before the
  /// game). `Transition::Launch` pops every screen on top of the stack that is a menu (dialogs included) or says so here.
  virtual bool closes_on_launch() const { return false; }
  /// The command a launch would run for the item under the cursor, or empty (idle-time preparation, ).
  virtual std::string launch_target(Ctx& cx) {
    (void)cx;
    return {};
  }
  /// Root screens only (system hot reload): the card the system cursor is on, an index in `cx.items`, or -1 when the screen has none.
  virtual long system_cursor(const Ctx& cx) const {
    (void)cx;
    return -1;
  }
  /// `cx.systems` / `cx.items` were replaced: put the cursor on card `item` at once (no glide) and forget everything derived from the
  /// old list (text under the carousel, "continue playing" row, announced system).
  virtual void reset_system_cursor(Ctx& cx, long item) { (void)cx, (void)item; }
};

struct Outcome {
  enum Kind { None, Launch, Quit } kind = None;
  std::string cmd;
  int sys = -1, game = -1;
};

class App {
 public:
  App(Ctx cx, std::unique_ptr<Screen> root, std::unique_ptr<StatusSource> status);

  Ctx cx;

  Outcome handle(Action a);
  /// A pad without a mapping was pressed (`Pad::take_unconfigured`): like ES, opens the controller detection screen
  /// (`menu.controllers.detect`) unless a menu or an overlay is already up. True when it was opened.
  bool open_controller_detect();
  ///  of the top screen.
  std::string launch_target() { return stack_.back()->launch_target(cx); }
  /// Repeat schedule of the screen on top (the loop hands it to the gamepad).
  Profile repeat_profile() const { return stack_.back()->repeat_profile(); }
  /// Advance animations; returns true if a redraw is needed (something moved).
  bool tick(float dt);
  bool animating() const { return animating_; }
  /// When the top screen's own timer (`Screen::wake_in`) fires, if it has one.
  std::optional<TimePoint> screen_deadline() const;

  /// Re-read the status sources that are due; true when the bar must be redrawn.
  bool poll_status(TimePoint now);
  /// When the event loop must wake up for the status bar.
  std::optional<TimePoint> status_deadline() const { return status_->next_deadline(); }
  /// Pin the status (harness `--status`).
  void freeze_status(const Status& s);
  /// A background count finished: update the carousel text of system `i`.
  void set_count(size_t i, size_t n);
  /// Starts the background count of every system whose count is `kCountPending` (<= 2 low-priority threads),
  /// `first` (the system under the cursor) before the rest. The wake-up of `set_count_wake` is kept for later calls (a reload starts a
  /// new worker).
  void start_counts(int threads, size_t first);
  /// The event loop's wake-up: called from the worker threads when results are ready (must be thread-safe).
  void set_count_wake(std::function<void()> wake);
  /// Applies finished counts; true when the carousel must be redrawn.
  bool drain_counts();
  /// Wake-up for the event loop from ANY worker thread (counts and `cx.sv->jobs`). Thread-safe.
  void set_wake(std::function<void()> wake);
  /// Main thread: runs finished job callbacks and expires toasts; true when a redraw is needed.
  bool drain_jobs(TimePoint now);

  /// Operations of one frame: from the lowest non-overlay screen up to the top one. The buffer is reused.
  const std::vector<Op>& render();
  /// Draws screen `name` (a registered one) into a scratch buffer, not shown (`Screen::warm`): the text and icon images it needs end
  /// up in `cx.assets`, so the first time the person opens it costs no more than the next ones. Empty when it is not registered.
  /// For the device loop to call once per screen, at an idle moment.
  const std::vector<Op>& warm(const std::string& name);

 private:
  Outcome apply(Transition t);
  /// Hot reload of the system list: when the stack is down to the root screen, `collections::reload_needed()` is set, a system
  /// loader is installed and no background job is pending (its callback may hold a system index), re-reads the systems with the
  /// current settings (HiddenSystems, SortSystems, LoadEmptySystems) and puts the carousel on the same system (by name; the nearest
  /// one when it is gone). Systems whose games were already read keep them. Nothing changes when the list of systems and cards is the
  /// same. True when the carousel changed (redraw).
  bool maybe_reload_systems();
  /// Starts the entry motion of the screen just pushed (no-op without `cx.ui_motion`).
  void begin_enter();
  /// Keeps the popped screens alive for the exit motion (bottom first), instead of destroying them.
  void begin_leave(std::vector<std::unique_ptr<Screen>> gone);

  std::vector<std::unique_ptr<Screen>> stack_;
  std::unique_ptr<StatusSource> status_;
  std::unique_ptr<CountWorker> counts_;
  std::vector<size_t> count_map_;  // worker job -> system index
  std::function<void()> count_wake_;  // kept to hand it to the next worker
  std::vector<std::pair<size_t, size_t>> count_tmp_;
  std::vector<Op> ops_;
  bool animating_ = false;
  std::vector<Op> warm_;
  // entry motion of the screen at stack index `depth` (seconds since it was pushed); `menu`: windows also slide a little
  struct Enter {
    bool active = false, menu = false;
    size_t depth = 0;
    float t = 0;
  } enter_;
  // screens popped a moment ago, still drawn (fading out) above the stack
  struct Leave {
    std::vector<std::unique_ptr<Screen>> screens;
    bool menu = false;
    float t = 0;
  } leave_;
};

/// Toast overlay (toast.cpp), appended after every frame.
void draw_toasts(Ctx& cx, std::vector<Op>& out);

/// Decoded pixels we keep before evicting the least recently used (the whole 1 GB console has ~700 MB free).
constexpr size_t kAssetBudget = 24u * 1024u * 1024u;

}  // namespace zf
