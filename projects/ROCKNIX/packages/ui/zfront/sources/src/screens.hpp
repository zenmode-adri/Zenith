// The two screens of phase 1: the system carousel and the game list.
#pragma once
#include <string>
#include <vector>

#include "app.hpp"

namespace zf {

class CarouselScreen : public Screen {
 public:
  explicit CarouselScreen(int start) : target_(start), cam_(float(start)) {}
  Transition handle(Action a, Ctx& cx) override;
  bool tick(float dt, Ctx& cx) override;
  void draw(Ctx& cx, std::vector<Op>& out) override;
  Profile repeat_profile() const override { return kSlow; }  // ES: the system carousel never accelerates
  /// Camera position in carousel steps (it glides towards the cursor).
  float camera() const { return cam_; }
  /// System the text under the carousel shows (-1 before the first tick) and its opacity.
  long info_item() const { return info_idx_; }
  float info_alpha() const { return info_a_; }
  /// "Continue playing" row. Down gives it the keys (when it is shown under the carousel), Left/Right pick a tile, A continues that
  /// game from its savestate, B or Up give the keys back. `row_focus` = item of `recent_items` that has them (-1: the carousel);
  /// `ring_alpha` = how strongly the focus ring shows (it fades with `cx.ui_motion`).
  int row_focus() const { return focus_; }
  float ring_alpha() const { return ring_a_; }
  const std::vector<RecentItem>& recent_items() const { return recent_.items; }
  long system_cursor(const Ctx& cx) const override;
  void reset_system_cursor(Ctx& cx, long item) override;

 private:
  /// The row is on screen: the carousel stopped on the system whose row it is and its text is the one shown.
  bool row_live(const Ctx& cx) const;
  /// Items of the row that are on screen right now (same geometry as the drawing), as `recent_visible`.
  std::vector<size_t> row_tiles(Ctx& cx);
  void set_focus(int item, const Ctx& cx);
  Transition launch_recent(Ctx& cx);
  int target_;  // unwrapped cursor
  float cam_;
  bool anim_ = false;
  /// The text under the carousel: system it shows (-1 = none yet) and its opacity. With `cx.ui_motion` it fades out when the cursor
  /// moves, changes system when invisible and fades back in once the camera is nearly there (ES: the system info does the same).
  long info_idx_ = -1;
  float info_a_ = 1.0f;
  /// System index the `system-selected` script event was last sent for (-1: none yet, also after coming back from a list).
  long announced_ = -1;
  /// "Continue playing" row of the system the carousel stopped on (`recent_for_`; -1 = not read yet, also after coming back from a list).
  RecentRow recent_;
  long recent_for_ = -1;
  /// Item of the row with the keys (-1 = the carousel has them). The ring is drawn on `ring_idx_` with `ring_a_` while it fades.
  int focus_ = -1, ring_idx_ = -1;
  float ring_a_ = 0.0f;
  /// Game the row had the keys on when a game was started from it: the keys go back to its tile once the row is read again.
  std::string resume_name_;
  bool reread_ = false;  // a launch left from the row: read it again on the next tick
};

class ListScreen : public Screen {
 public:
  ListScreen(size_t system_index, Ctx& cx);
  Transition handle(Action a, Ctx& cx) override;
  bool tick(float dt, Ctx& cx) override;
  std::optional<float> wake_in() const override;
  bool take_redraw() override {
    bool r = redraw_;
    redraw_ = false;
    return r;
  }
  void draw(Ctx& cx, std::vector<Op>& out) override;
  std::string launch_target(Ctx& cx) override;

 private:
  std::string command(Ctx& cx);
  std::optional<float> detail_wake_in() const;
  size_t si_, cur_ = 0, lines_;
  std::vector<std::string> names_;
  /// Per game: "12 min ago" when played in the last 30 days, else empty (computed when the list opens).
  std::vector<std::string> marks_;
  /// Seconds since the cursor last moved; cover and description are skipped until it reaches `kSettleSeconds`, and
  /// the description scrolls with the time after that (`desc_scroll_offset`).
  double settle_;
  /// Right column of the selected game, rebuilt only when the game or the settled state changes.
  DetailedData d_;
  size_t d_idx_ = 0;
  bool d_valid_ = false, d_have_ = false, d_settled_ = false;
  /// How far the description can scroll (from the last draw); 0 = no timer needed.
  int max_scroll_ = 0;
  bool redraw_ = false;
  /// `game-selected` script event: seconds since the cursor last moved while one is pending (-1: none). Armed only when a script
  /// listens (`oscontract::selected_listening`), fired by `tick` once the cursor rested for `kSettleSeconds`, so holding a
  /// direction sends one event, at the end.
  double sel_wait_ = -1;
};

/// Carousel glide: the camera closes `kGlideRate` per second of its distance to the target (first-order, frame-rate independent).
/// It answers at full speed the instant a key is pressed and has a soft tail, 90 % of a step in ~145 ms and 99 % in ~290 ms; a new key
/// press while moving only changes the target, so the speed never restarts from zero or from the peak (ES: 350 ms easeOutQuint).
constexpr float kGlideRate = 16.0f;
/// Text under the carousel: fade-out of the old system (s), fade-in of the new one (s), and how close (carousel steps) the camera
/// must be for the new text to start coming in.
constexpr float kInfoOut = 0.07f, kInfoIn = 0.14f, kInfoInAt = 0.35f;
/// Focus ring of the "continue playing" row: fade in / out (s).
constexpr float kRingFade = 0.10f;
/// Distance (in carousel steps) under which the camera is snapped onto the target and the animation ends (about 1 px).
constexpr float kGlideEnd = 0.003f;
/// A press of A is accepted once the camera is this close to the target (about 7 px): the tail of the glide must not eat the key.
constexpr float kGlideAcceptA = 0.03f;
/// Cursor rest time before the heavy parts of the right column (cover decode, description wrap) are drawn.
constexpr float kSettleSeconds = 0.12f;

}  // namespace zf
