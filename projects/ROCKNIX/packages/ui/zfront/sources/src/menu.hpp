// Menu framework: a scrollable list of entries (action, toggle, slider, choice, text, info, header, separator) with the
// Zenith look, and a small declarative builder. A section menu is a few lines:
//
//   ZF_REGISTER_SCREEN("menu.sound", [](zf::Ctx& cx, const std::string&) {
//     zf::MenuSpec s;
//     s.title = zf::tr("SOUND SETTINGS");
//     s.build = [](zf::MenuBuilder& m, zf::Ctx& cx) {
//       m.slider(zf::tr("SYSTEM VOLUME"), 0, 100, 1, [&cx] { return vol(cx); }, [&cx](int v) { set_vol(cx, v); })
//           .unit("%").commit([&cx](int v) { cx.conf.persist("audio.volume", std::to_string(v)); });
//       m.toggle(zf::tr("SHOW CLOCK"), [&cx] { return cx.es.get_bool("DrawClock", true); },
//                [&cx](bool b) { cx.es.set_bool("DrawClock", b); cx.es.save(); });
//       m.choice(zf::tr("SHOW BATTERY STATUS"), {{"no", tr("NO")}, {"icon", tr("ICON")}, {"text", tr("TEXT")}},
//                [&cx] { return get(cx); }, [&cx](const std::string& v) { set(cx, v); });
//       m.submenu(zf::tr("AUDIO OUTPUT"), "menu.sound.output");
//       m.action(zf::tr("RESET AUDIO"), [&cx] { cx.sv->toasts.push(zf::tr("Done")); }).danger().confirm(zf::tr("RESET AUDIO?"));
//     };
//     return zf::make_menu(std::move(s));
//   });
//
// Rules for authors of menus:
//  * Getters are read when the menu is built, after every interaction and after `refresh()`; never per frame.
//  * Setters run on the main thread; do the slow part with `cx.sv->jobs.post` and `cx.sv->toasts.push` the result.
//  * A menu that fills in later (wifi scan) keeps its data in a shared_ptr, builds from it, starts the work in
//    `on_open` and calls the `refresh` it was given from the job's `done` callback (main thread). `refresh` is safe to
//    call after the menu was closed (it does nothing).
//  * The references returned by the builder stay valid only until the next builder call: chain on the same line.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "app.hpp"
#include "ui_style.hpp"

namespace zf {

/// What an entry does when accepted. May return a Transition (open another screen, quit...).
using Act = std::function<Transition(Ctx&)>;
/// Rebuilds the menu on its next tick. Main thread only.
using Refresh = std::function<void()>;

namespace ui_detail {
/// Accepts `[]{...}`, `[](Ctx&){...}`, each returning void or Transition.
template <class F>
Act make_act(F&& f) {
  using D = std::decay_t<F>;
  if constexpr (std::is_invocable_v<D, Ctx&>) {
    if constexpr (std::is_void_v<std::invoke_result_t<D, Ctx&>>) {
      return [g = D(std::forward<F>(f))](Ctx& c) {
        g(c);
        return Transition::none();
      };
    } else {
      return Act(std::forward<F>(f));
    }
  } else {
    if constexpr (std::is_void_v<std::invoke_result_t<D>>) {
      return [g = D(std::forward<F>(f))](Ctx&) {
        g();
        return Transition::none();
      };
    } else {
      return [g = D(std::forward<F>(f))](Ctx&) { return g(); };
    }
  }
}
}  // namespace ui_detail

struct Option {
  std::string value;  // what the setter receives / the getter returns
  std::string label;  // what is shown (already translated)
};

enum class EntryKind : uint8_t { Action, Toggle, Slider, Choice, Text, Info, Header, Separator };

/// Speed of a switch knob (it closes this fraction of its way per second: ~90 % in 100 ms).
constexpr float kKnobRate = 22.0f;

struct Entry {
  EntryKind kind = EntryKind::Action;
  std::string label, id, hint_text, icon_name, value_text;
  bool enabled = true;      // false: dimmed, skipped by the cursor
  bool is_danger = false;
  bool hidden = false;      // dropped after the build (a submenu whose screen is not registered)
  bool show_arrow = false;  // chevron at the right (opens a screen)
  std::string confirm_msg;  // non-empty: YES/NO question before the action runs
  Act on_a;
  // toggle
  std::function<bool()> get_b;
  std::function<void(bool)> set_b;
  bool b = false;
  float kn = -1.0f;  // position of the knob while it slides, 0 = off .. 1 = on (-1: not animated: follows `b`)
  // slider
  int min = 0, max = 100, step = 1, i = 0;
  std::string unit_text;
  std::function<int()> get_i;
  std::function<void(int)> set_i, commit_i;
  std::function<std::string(int)> fmt;
  // choice / text
  std::vector<Option> options;
  std::function<std::string()> get_s;
  std::function<void(const std::string&)> set_s;
  bool cycle = true;  // left/right step through the options
  int sel = -1;       // index in `options` of the current value
  bool password = false;
  // images kept between frames (ui_style.hpp `Txt`)
  ui::Txt t_label, t_value, t_icon;
  float label_w = 0, value_w = 0;  // the max widths the cached images were made for

  // ---- chaining (builder)
  Entry& hint(std::string s) { hint_text = std::move(s); return *this; }
  Entry& icon(std::string name) { icon_name = std::move(name); return *this; }
  Entry& danger(bool on = true) { is_danger = on; return *this; }
  Entry& disabled(bool on = true) { enabled = !on; return *this; }
  /// Ask "<msg>" with YES/NO before running (Action entries).
  Entry& confirm(std::string msg) { confirm_msg = std::move(msg); return *this; }
  /// Right-hand text of an Action or Info entry.
  Entry& value(std::string s) { value_text = std::move(s); t_value.reset(); return *this; }
  Entry& with_id(std::string s) { id = std::move(s); return *this; }
  /// Slider: text after the number ("%", " min").
  Entry& unit(std::string s) { unit_text = std::move(s); return *this; }
  /// Slider: custom text for a value (replaces number + unit).
  Entry& format(std::function<std::string(int)> f) { fmt = std::move(f); return *this; }
  /// Slider: called once when the user stops adjusting (after 0.5 s) or leaves the menu. `set` already ran for each step.
  Entry& commit(std::function<void(int)> f) { commit_i = std::move(f); return *this; }
  /// Choice: left/right do nothing (only A opens the list); use when the setter is slow or has side effects.
  Entry& no_cycle() { cycle = false; return *this; }
  /// Text: shown as dots, the keyboard hides the characters.
  Entry& secret() { password = true; return *this; }
};

class MenuBuilder {
 public:
  MenuBuilder(Ctx& c, std::vector<Entry>& out, Refresh r) : cx(c), refresh(std::move(r)), out_(out) {}

  /// Runs `f` (see `make_act`) when accepted.
  template <class F>
  Entry& action(std::string label, F&& f) {
    return add_action(std::move(label), ui_detail::make_act(std::forward<F>(f)));
  }
  /// Opens the registered screen `screen` (`Transition::open`). While the screen is not registered the entry is left
  /// out altogether (never drawn, never focused), so a menu only lists what exists.
  Entry& submenu(std::string label, std::string screen, std::string arg = {});
  Entry& toggle(std::string label, std::function<bool()> get, std::function<void(bool)> set);
  Entry& slider(std::string label, int min, int max, int step, std::function<int()> get, std::function<void(int)> set);
  Entry& choice(std::string label, std::vector<Option> options, std::function<std::string()> get, std::function<void(const std::string&)> set);
  /// Edited with the on-screen keyboard.
  Entry& text(std::string label, std::function<std::string()> get, std::function<void(const std::string&)> set);
  /// Read-only text. Not selectable.
  Entry& info(std::string label, std::string value = {});
  /// Section title. Not selectable.
  Entry& header(std::string label);
  Entry& separator();

  /// Rows added so far (a group of menus can tell whether a part showed anything).
  size_t count() const { return out_.size(); }

  Ctx& cx;
  Refresh refresh;

 private:
  Entry& add_action(std::string label, Act a);
  Entry& push(EntryKind k, std::string label);
  std::vector<Entry>& out_;
};

struct MenuSpec {
  std::string title;
  /// Optional: the untranslated key (`"SOUND SETTINGS"`, a string literal) of a fixed `title`. A menu that has it re-translates
  /// the title itself when the interface language changes while the menu is open (and rebuilds its entries).
  const char* title_key = nullptr;
  /// Adds the entries. Runs when the menu opens and again on every `refresh()` (keep it free of side effects).
  std::function<void(MenuBuilder&, Ctx&)> build;
  /// Optional: runs once after the first build (start a scan with `cx.sv->jobs`, then call `refresh`).
  std::function<void(Ctx&, const Refresh&)> on_open;
  /// Optional: runs when the menu is left with B.
  std::function<void(Ctx&)> on_close;
};

class Menu : public Screen {
 public:
  explicit Menu(MenuSpec spec);
  ~Menu() override;

  Transition handle(Action a, Ctx& cx) override;
  bool tick(float dt, Ctx& cx) override;
  std::optional<float> wake_in() const override;
  void draw(Ctx& cx, std::vector<Op>& out) override;
  /// Rows built and drawn, `on_open` NOT run (it starts commands and jobs): for a throw-away instance only.
  void warm(Ctx& cx, std::vector<Op>& out) override;
  bool is_menu() const override { return true; }
  /// Start closed the whole menu stack: flushes a pending slider commit and runs `on_close`, like leaving with B.
  void on_dismiss(Ctx& cx) override;

  // ---- introspection (tests, other screens)
  size_t size() const { return e_.size(); }
  const Entry& entry(size_t i) const { return e_[i]; }
  /// Index of the entry under the cursor, or -1 (no selectable entry).
  int cursor() const { return cur_; }
  /// Slide position of the knob of toggle entry `i` (0 off .. 1 on; -1 when it is not animated).
  float knob(size_t i) const { return e_[i].kn; }
  const std::string& title() const { return spec_.title; }
  /// Replaces the title of an open menu (language change, a name that is known later). Drops the `title_key`.
  void set_title(std::string t);
  /// Rebuilds now (keeps the cursor on the same entry by id/label).
  void rebuild(Ctx& cx);
  /// Safe handle to ask for a rebuild from jobs.
  Refresh refresher();
  /// Rows that fit in the viewport (needs a layout: after the first handle/draw).
  int visible_rows() const { return m_.lines; }

 protected:
  /// Hook for subclasses with extra buttons (X, Y, Select...). Default: nothing.
  virtual Transition on_other(Action a, Ctx& cx) {
    (void)a, (void)cx;
    return Transition::none();
  }

 private:
  struct Token;
  bool sync(Ctx& cx);  // build, layout, pending rebuild / value refresh; true when something changed
  void layout(Ctx& cx);
  void refresh_values();
  void refresh_entry(Entry& e);
  void move(int dir, bool wrap);
  void retarget(bool snap);
  void adjust(Entry& e, int dir, Ctx& cx);
  void page(int dir);
  Transition leave(Ctx& cx);
  void flush_commit();
  Transition activate(Entry& e, Ctx& cx);
  bool focusable(size_t i) const;
  void draw_row(Ctx& cx, std::vector<Op>& out, size_t i, float y, bool sel, float vis = 1.0f);

  MenuSpec spec_;
  std::vector<Entry> e_;
  std::shared_ptr<Token> tok_;
  int cur_ = -1;
  bool opened_ = false;
  // layout (pixels)
  ui::Metrics m_;
  ui::Style st_;
  bool have_layout_ = false;
  float lay_w_ = 0, lay_h_ = 0;
  std::vector<float> top_, hgt_;
  float content_h_ = 0;
  bool has_icons_ = false;
  // animation
  float scroll_ = 0, scroll_t_ = 0, sel_y_ = 0, sel_y_t_ = 0;
  bool placed_ = false;
  // debounced slider commit
  std::function<void()> pending_;
  float pending_left_ = 0;
  // drawing state
  ui::Shapes shapes_;
  ui::HelpBar help_;
  ui::Txt t_title_, t_hint_;
  int hint_for_ = -2;
  std::string lang_;  // i18n::language() the entries and the title were made for
};

/// A Menu from a spec (the normal way to register a section).
std::unique_ptr<Screen> make_menu(MenuSpec spec);

}  // namespace zf
