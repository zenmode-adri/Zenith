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

 private:
  int target_;  // unwrapped cursor
  float cam_;
  bool anim_ = false;
  float anim_t_ = 0, anim_from_ = 0;
  /// System index the `system-selected` script event was last sent for (-1: none yet, also after coming back from a list).
  long announced_ = -1;
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

/// Duration of one carousel step (ES `slide` transition is of this order; tuned against a real capture later).
constexpr float kStepSeconds = 0.200f;
/// Cursor rest time before the heavy parts of the right column (cover decode, description wrap) are drawn.
constexpr float kSettleSeconds = 0.12f;

}  // namespace zf
