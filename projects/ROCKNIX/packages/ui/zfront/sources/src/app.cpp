#include "app.hpp"

#include "sound.hpp"

#include <cstdio>

#include "collections.hpp"
#include "motion.hpp"
#include "registry.hpp"
#include "screensaver.hpp"
#include "syslist.hpp"
#include "ui_prefs.hpp"

namespace zf {

Transition Transition::push(std::unique_ptr<Screen> s) {
  Transition t = of(Push);
  t.screen = std::move(s);
  return t;
}
Transition Transition::replace(std::unique_ptr<Screen> s) {
  Transition t = of(Replace);
  t.screen = std::move(s);
  return t;
}
Transition Transition::open(std::string name, std::string arg) {
  Transition t;
  t.kind = Open;
  t.name = std::move(name);
  t.arg = std::move(arg);
  return t;
}

long Ctx::item_of(size_t sys) const {
  if (sys < items.size() && system_of(sys) == sys) return long(sys);  // 1:1 (the usual case)
  for (size_t k = 0; k < items.size(); ++k)
    if (system_of(k) == sys) return long(k);
  return -1;
}

void Ctx::open_system(size_t i) {
  if (i >= systems.size()) return;
  systems[i].open();
  const long k = item_of(i);
  if (k >= 0) items[size_t(k)].games = uint32_t(systems[i].count);
}

App::App(Ctx c, std::unique_ptr<Screen> root, std::unique_ptr<StatusSource> status) : cx(std::move(c)), status_(std::move(status)) {
  ui_prefs::on_status(cx, status_->status());  // clock / battery preferences
  stack_.push_back(std::move(root));
}

Outcome App::handle(Action a) {
  Transition t = stack_.back()->handle(a, cx);
  Outcome o = apply(std::move(t));
  if (o.kind == Outcome::None) maybe_reload_systems();  // the menus just closed: apply what they changed
  return o;
}

bool App::open_controller_detect() {
  static const char* const kName = "menu.controllers.detect";
  if (stack_.back()->is_menu() || stack_.back()->overlay() || !Registry::instance().has(kName)) return false;
  apply(Transition::open(kName));
  return true;
}

namespace {
// Motion of windows and screens (seconds / fraction of the screen height). Entering is eased out and a little longer than leaving:
// leaving must feel immediate.
constexpr float kEnterMenu = 0.18f, kEnterPlain = 0.14f, kLeaveMenu = 0.11f, kLeavePlain = 0.12f;
constexpr float kRise = 0.017f, kSink = 0.008f;
}  // namespace

void App::begin_enter() {
  if (!cx.ui_motion || stack_.empty()) return;
  const Screen& s = *stack_.back();
  enter_ = Enter{true, s.is_menu() || s.overlay(), stack_.size() - 1, 0.0f};
}

void App::begin_leave(std::vector<std::unique_ptr<Screen>> gone) {
  enter_.active = false;
  if (!cx.ui_motion || gone.empty()) return;
  leave_.menu = false;
  for (const auto& s : gone) leave_.menu = leave_.menu || s->is_menu() || s->overlay();
  leave_.screens = std::move(gone);
  leave_.t = 0.0f;
}

// Fades and slides the ops drawn since `from`, except the status bar among them: it is the same bar as the one of the screen
// below, so a moving, half transparent copy over it shows as a ghost (the clock doubling) instead of a bar that stays put.
static void animate_ops(std::vector<Op>& ops, size_t from, float alpha, float dy, float w, float h) {
  const motion::OpRange sr = motion::status_range();
  std::vector<Op> bar;
  if (sr.to > sr.from && sr.from >= from && sr.to <= ops.size()) bar.assign(ops.begin() + long(sr.from), ops.begin() + long(sr.to));
  if (dy != 0.0f) motion::shift_y(ops, from, dy, w, h);
  motion::fade(ops, from, alpha);
  if (!bar.empty()) std::copy(bar.begin(), bar.end(), ops.begin() + long(sr.from));
}

Outcome App::apply(Transition t) {
  switch (t.kind) {
    case Transition::None:
      break;
    case Transition::Push:
      stack_.push_back(std::move(t.screen));
      begin_enter();
      break;
    case Transition::Pop:
      if (stack_.size() > 1) {
        std::vector<std::unique_ptr<Screen>> gone;
        gone.push_back(std::move(stack_.back()));
        stack_.pop_back();
        begin_leave(std::move(gone));
      }
      break;
    case Transition::Replace:
      stack_.pop_back();
      stack_.push_back(std::move(t.screen));
      enter_.active = false;
      break;
    case Transition::Open: {
      std::unique_ptr<Screen> s = Registry::instance().make(t.name, cx, t.arg);
      if (s) {
        if (t.name == "menu.main" || t.name == "menu.game_options") sound::play(cx, sound::Cue::MenuOpen);  // ES: the menus of the list
        stack_.push_back(std::move(s));
        begin_enter();
      } else {
        cx.sv->toasts.push(tr("Not available yet"));
      }
      break;
    }
    case Transition::CloseMenus: {
      std::vector<std::unique_ptr<Screen>> gone;
      while (stack_.size() > 1 && stack_.back()->is_menu()) {
        stack_.back()->on_dismiss(cx);
        gone.insert(gone.begin(), std::move(stack_.back()));  // bottom first: that is the drawing order
        stack_.pop_back();
      }
      begin_leave(std::move(gone));
      break;
    }
    case Transition::Launch:
      sound::play(cx, sound::Cue::Launch);  // the player keeps the device open until it has played (see sdlr.cpp), the launch does not wait
      enter_.active = false;
      leave_.screens.clear();
      // the screens the game was started from (savestate manager, confirmation dialog) close first: when the game ends the user
      // is back on the list with the same cursor, as in ES
      while (stack_.size() > 1 && (stack_.back()->is_menu() || stack_.back()->closes_on_launch())) {
        if (stack_.back()->is_menu()) stack_.back()->on_dismiss(cx);
        stack_.pop_back();
      }
      return Outcome{Outcome::Launch, std::move(t.cmd), t.sys, t.game};
    case Transition::Quit:
      return Outcome{Outcome::Quit, {}, -1, -1};
  }
  return {};
}

std::optional<TimePoint> App::screen_deadline() const {
  std::optional<float> s = stack_.back()->wake_in();
  std::optional<TimePoint> t = cx.sv->toasts.next_deadline();
  if (t && cx.ui_motion) *t -= std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(kToastOut));  // wake for the exit
  if (s) {
    TimePoint ts = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(*s));
    if (!t || ts < *t) t = ts;
  }
  return t;
}

bool App::tick(float dt) {
  bool moving = stack_.back()->tick(dt, cx);
  const float mdt = std::min(dt, 0.05f);  // the first tick after a long sleep must not swallow the whole entry
  if (enter_.active) {
    enter_.t += mdt;
    if (enter_.t >= (enter_.menu ? kEnterMenu : kEnterPlain)) enter_.active = false;
    moving = true;
  }
  if (cx.ui_motion && cx.sv->toasts.moving(std::chrono::steady_clock::now())) moving = true;
  if (!leave_.screens.empty()) {
    leave_.t += mdt;
    if (leave_.t >= (leave_.menu ? kLeaveMenu : kLeavePlain)) leave_.screens.clear();
    moving = true;
  }
  bool redraw = moving || animating_ || stack_.back()->take_redraw();
  animating_ = moving;
  Transition t = stack_.back()->take_transition(cx);
  if (t.kind != Transition::None) {
    apply(std::move(t));  // Launch / Quit are not meant to come from here
    redraw = true;
  }
  if (maybe_reload_systems()) redraw = true;
  return redraw;
}

bool App::poll_status(TimePoint now) {
  if (!status_->poll(now)) return false;
  ui_prefs::on_status(cx, status_->status());
  return true;
}

void App::freeze_status(const Status& s) {
  ui_prefs::on_status(cx, s);
  status_->freeze(s);
}

void App::set_count(size_t i, size_t n) {
  if (i < cx.systems.size() && !cx.systems[i].games.empty()) return;  // already opened: its count is exact
  const long k = cx.item_of(i);
  if (k >= 0) cx.items[size_t(k)].games = uint32_t(n);
  if (i < cx.systems.size()) cx.systems[i].count = n;
}

void App::start_counts(int threads, size_t first) {
  std::vector<CountJob> jobs;
  count_map_.clear();
  size_t first_job = 0;
  for (size_t i = 0; i < cx.systems.size(); ++i) {
    if (cx.systems[i].count != kCountPending) continue;
    if (i == first) first_job = jobs.size();
    jobs.push_back({cx.systems[i].rom_dir, Exts(cx.systems[i].exts)});
    count_map_.push_back(i);
  }
  if (jobs.empty()) return;
  counts_ = std::make_unique<CountWorker>(std::move(jobs), threads, first_job, count_wake_);
}

void App::set_count_wake(std::function<void()> wake) {
  count_wake_ = wake;
  if (counts_) counts_->set_wake(std::move(wake));
}

bool App::drain_counts() {
  if (!counts_) return false;
  bool finished = counts_->done();  // read BEFORE draining: every result is queued by the time done() is true
  count_tmp_.clear();
  bool any = counts_->drain(count_tmp_);
  for (const auto& r : count_tmp_) set_count(count_map_[r.first], r.second);
  if (finished) counts_.reset();  // threads have exited: free them
  return any;
}

void App::set_wake(std::function<void()> wake) {
  cx.sv->jobs.set_wake(wake);
  set_count_wake(std::move(wake));
}

bool App::drain_jobs(TimePoint now) {
  bool a = cx.sv->jobs.drain();
  bool b = cx.sv->toasts.expire(now);
  bool c = maybe_reload_systems();  // a reload that waited for the last job
  return a || b || c;
}

bool App::maybe_reload_systems() {
  if (!cx.system_loader || stack_.size() != 1 || !collections::reload_needed() || !cx.sv->jobs.idle()) return false;
  collections::set_reload_needed(false);
  std::vector<System> fresh;
  std::string err;
  if (!cx.system_loader(cx.es.get_bool("LoadEmptySystems", false), fresh, err)) {
    std::fprintf(stderr, "zfront: system list not reloaded: %s\n", err.c_str());  // the carousel stays as it was
    return false;
  }
  // nothing the carousel shows changed (a collection edit, a setting that only matters to collections): keep cursor and state
  std::vector<SystemItem> items = make_items(fresh, cx.es);
  if (same_systems(cx.systems, cx.items, fresh, items)) return false;

  const long old_cursor = stack_[0]->system_cursor(cx);
  std::vector<std::string> old_names = item_names(cx.systems, cx.items);
  // The count workers hold copies of the folders and only queue results, so joining them is all it takes before `systems` changes.
  counts_.reset();
  count_map_.clear();
  carry_over(cx.systems, fresh);  // games already read and counts already known are kept
  cx.systems = std::move(fresh);
  cx.items = make_items(cx.systems, cx.es);
  long cursor = -1;
  if (old_cursor >= 0) {
    cursor = nearest_item(old_names, old_cursor, item_names(cx.systems, cx.items));
    stack_[0]->reset_system_cursor(cx, cursor);
  }
  start_counts(2, cursor >= 0 && !cx.items.empty() ? cx.system_of(size_t(cursor)) : 0);
  return true;
}

const std::vector<Op>& App::warm(const std::string& name) {
  warm_.clear();
  if (Registry::instance().has(name))
    if (std::unique_ptr<Screen> s = Registry::instance().make(name, cx, "")) s->warm(cx, warm_);
  return warm_;
}

const std::vector<Op>& App::render() {
  size_t base = 0;
  for (size_t i = stack_.size(); i-- > 0;) {
    if (!stack_[i]->overlay()) {
      base = i;
      break;
    }
  }
  ops_.clear();
  if (screensaver::mode() == screensaver::Mode::Black) {  // nothing of the UI shows through: do not even build it
    screensaver::draw(cx, ops_);
    return ops_;
  }
  // an opaque screen that is entering shows what was under it through its own fading background
  if (enter_.active && enter_.depth == base && !stack_[base]->overlay()) {
    for (size_t i = base; i-- > 0;) {
      if (!stack_[i]->overlay()) {
        base = i;
        break;
      }
    }
  }
  for (size_t i = base; i < stack_.size(); ++i) {
    const size_t from = ops_.size();
    motion::status_range() = {};
    stack_[i]->draw(cx, ops_);
    if (enter_.active && i == enter_.depth) {
      const float p = motion::ease_out(enter_.t / (enter_.menu ? kEnterMenu : kEnterPlain));
      animate_ops(ops_, from, p, enter_.menu ? (1.0f - p) * kRise * cx.h : 0.0f, cx.w, cx.h);
    }
  }
  if (!leave_.screens.empty()) {
    const float q = std::clamp(leave_.t / (leave_.menu ? kLeaveMenu : kLeavePlain), 0.0f, 1.0f);
    for (const auto& s : leave_.screens) {
      const size_t from = ops_.size();
      motion::status_range() = {};
      s->draw(cx, ops_);
      animate_ops(ops_, from, 1.0f - q, leave_.menu ? q * kSink * cx.h : 0.0f, cx.w, cx.h);
    }
  }
  if (screensaver::mode() == screensaver::Mode::Dim)
    screensaver::draw(cx, ops_);  // ES draws no toasts over the screensaver
  else
    draw_toasts(cx, ops_);
  cx.assets.end_frame(ops_, kAssetBudget);
  return ops_;
}

}  // namespace zf
