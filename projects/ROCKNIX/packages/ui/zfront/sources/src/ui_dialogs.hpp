// Dialogs of the menu framework: YES/NO question, message, option list and on-screen keyboard. Each helper returns the
// `Transition` that shows it, so an action can end with `return ui::confirm(...)`.
//
// The callbacks run when the user accepts; the dialog then closes by itself. A callback may return a Transition
// (`Transition::open("menu.x")`, `Transition::quit()`...): it replaces the dialog on the stack, so B afterwards goes back
// to the menu that asked. Buttons: A accepts, B cancels (the NO / cancel result), Left/Right move between buttons.
// Controls of the keyboard: d-pad moves, A types, B deletes (cancels when the field is empty), X space, Y shift,
// L shows/hides a password, Start accepts, Select cancels.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "menu.hpp"

namespace zf::ui {

struct ConfirmOpts {
  std::string title;      // optional heading
  std::string yes, no;    // button texts; default tr("YES") / tr("NO")
  bool danger = false;    // YES is drawn red
  bool default_yes = false;
};

/// YES/NO question. The focus starts on NO (Zenith rule) unless `default_yes`. `on_yes` runs on YES only.
Transition confirm(std::string message, Act on_yes, ConfirmOpts opts = {});
/// Message with an OK button; `on_ok` optional.
Transition message(std::string text, Act on_ok = nullptr, std::string title = {});
/// Popup list; the focus starts on the entry whose value is `current`. B cancels (no callback).
Transition choose(std::string title, std::vector<Option> options, std::string current, std::function<void(Ctx&, const std::string&)> on_pick);

struct KeyboardOpts {
  std::string title;
  bool password = false;
  size_t max_len = 0;  // in characters; 0 = 128
};
/// On-screen keyboard (full screen). `on_done` gets the text when OK is pressed.
Transition keyboard(KeyboardOpts opts, std::string initial, std::function<void(Ctx&, const std::string&)> on_done);

/// Turns the result of a dialog callback into the transition the dialog returns: none -> pop, push/open -> replace,
/// pop/launch/quit unchanged.
Transition leave_with(Transition t, Ctx& cx);

// ---- the screens (public for tests and for menus that want their own) --------------------------------------------------

class ConfirmDialog : public Screen {
 public:
  ConfirmDialog(std::string message, Act on_yes, ConfirmOpts opts, bool single_button);
  Transition handle(Action a, Ctx& cx) override;
  void draw(Ctx& cx, std::vector<Op>& out) override;
  bool overlay() const override { return true; }
  bool is_menu() const override { return true; }
  /// 0 = left button (YES / OK), 1 = right button (NO).
  int focus() const { return focus_; }

 private:
  std::string msg_;
  Act on_yes_;
  ConfirmOpts o_;
  bool single_;
  int focus_ = 1;
  Shapes shapes_;
  Txt t_msg_, t_title_, t_yes_, t_no_;
  float lay_w_ = 0, lay_h_ = 0;
  ui::Metrics m_;
  ui::Style st_;
};

class ChoiceDialog : public Screen {
 public:
  ChoiceDialog(std::string title, std::vector<Option> options, std::string current, std::function<void(Ctx&, const std::string&)> on_pick);
  Transition handle(Action a, Ctx& cx) override;
  void draw(Ctx& cx, std::vector<Op>& out) override;
  bool overlay() const override { return true; }
  bool is_menu() const override { return true; }
  int cursor() const { return cur_; }

 private:
  void layout(Ctx& cx);
  std::string title_;
  std::vector<Option> opts_;
  std::vector<Txt> t_;
  std::function<void(Ctx&, const std::string&)> pick_;
  int cur_ = 0, cur_value_ = -1;
  float scroll_ = 0, scroll_t_ = 0, sel_y_ = 0;
  bool placed_ = false;
  ui::Metrics m_;
  ui::Style st_;
  float lay_w_ = 0, lay_h_ = 0;
  float card_x_ = 0, card_y_ = 0, card_w_ = 0, card_h_ = 0, rows_y_ = 0, rows_h_ = 0;
  int vis_ = 0;
  Shapes shapes_;
  Txt t_title_;

 public:
  bool tick(float dt, Ctx& cx) override;
};

class KeyboardScreen : public Screen {
 public:
  KeyboardScreen(KeyboardOpts opts, std::string initial, std::function<void(Ctx&, const std::string&)> on_done);
  Transition handle(Action a, Ctx& cx) override;
  void draw(Ctx& cx, std::vector<Op>& out) override;
  bool is_menu() const override { return true; }
  /// The caret blinks on the wall clock. `tick` returns false (no animation: the loop must not spin); the next phase is
  /// announced through `wake_in` and the redraw through `take_redraw`, so only a visible keyboard has a deadline.
  bool tick(float dt, Ctx& cx) override;
  std::optional<float> wake_in() const override;
  bool take_redraw() override;
  bool caret_visible() const { return caret_on_; }

  const std::string& text() const { return text_; }
  /// 0 lower case, 1 upper case, 2 symbols.
  int layer() const { return layer_; }
  bool revealed() const { return reveal_; }

 private:
  struct Key {
    enum Type : uint8_t { Char, Shift, Sym, Space, Del, Ok } type = Char;
    std::string label;
    float x0 = 0, w = 1;  // in tenths of the grid width
  };
  void build_layer();
  Transition press(const Key& k, Ctx& cx);
  Transition accept(Ctx& cx);
  void erase_last();
  void move_v(int dir);
  const Key& key_at(int r, int c) const { return rows_[size_t(r)][size_t(c)]; }

  KeyboardOpts o_;
  std::string text_;
  std::function<void(Ctx&, const std::string&)> done_;
  int layer_ = 0, row_ = 1, col_ = 0;
  bool reveal_ = false;
  TimePoint next_flip_ = Clock::now() + std::chrono::milliseconds(530);  // next caret phase (>= 0.5 s per phase)
  bool caret_on_ = true, redraw_ = false;
  std::vector<std::vector<Key>> rows_;
  // drawing
  ui::Metrics m_;
  ui::Style st_;
  Shapes shapes_;
  HelpBar help_;
  Txt t_title_, t_text_;
  std::vector<Txt> t_keys_;
  int keys_layer_ = -1;
  std::string shown_, shown_src_;  // what the field shows / the text it was made from
  bool shown_rev_ = false, shown_valid_ = false;
  float lay_w_ = 0, lay_h_ = 0;
};

}  // namespace zf::ui
