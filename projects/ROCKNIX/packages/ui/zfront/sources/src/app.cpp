#include "app.hpp"

#include "registry.hpp"
#include "screensaver.hpp"
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

void Ctx::open_system(size_t i) {
  if (i >= systems.size()) return;
  systems[i].open();
  if (i < items.size()) items[i].games = uint32_t(systems[i].count);
}

App::App(Ctx c, std::unique_ptr<Screen> root, std::unique_ptr<StatusSource> status) : cx(std::move(c)), status_(std::move(status)) {
  ui_prefs::on_status(cx, status_->status());  // clock / battery preferences
  stack_.push_back(std::move(root));
}

Outcome App::handle(Action a) {
  Transition t = stack_.back()->handle(a, cx);
  return apply(std::move(t));
}

bool App::open_controller_detect() {
  static const char* const kName = "menu.controllers.detect";
  if (stack_.back()->is_menu() || stack_.back()->overlay() || !Registry::instance().has(kName)) return false;
  apply(Transition::open(kName));
  return true;
}

Outcome App::apply(Transition t) {
  switch (t.kind) {
    case Transition::None:
      break;
    case Transition::Push:
      stack_.push_back(std::move(t.screen));
      break;
    case Transition::Pop:
      if (stack_.size() > 1) stack_.pop_back();
      break;
    case Transition::Replace:
      stack_.pop_back();
      stack_.push_back(std::move(t.screen));
      break;
    case Transition::Open: {
      std::unique_ptr<Screen> s = Registry::instance().make(t.name, cx, t.arg);
      if (s) stack_.push_back(std::move(s));
      else cx.sv->toasts.push(tr("Not available yet"));
      break;
    }
    case Transition::CloseMenus:
      while (stack_.size() > 1 && stack_.back()->is_menu()) {
        stack_.back()->on_dismiss(cx);
        stack_.pop_back();
      }
      break;
    case Transition::Launch:
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
  if (s) {
    TimePoint ts = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(*s));
    if (!t || ts < *t) t = ts;
  }
  return t;
}

bool App::tick(float dt) {
  bool moving = stack_.back()->tick(dt, cx);
  bool redraw = moving || animating_ || stack_.back()->take_redraw();
  animating_ = moving;
  Transition t = stack_.back()->take_transition(cx);
  if (t.kind != Transition::None) {
    apply(std::move(t));  // Launch / Quit are not meant to come from here
    redraw = true;
  }
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
  if (i < cx.items.size()) cx.items[i].games = uint32_t(n);
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
  counts_ = std::make_unique<CountWorker>(std::move(jobs), threads, first_job);
}

void App::set_count_wake(std::function<void()> wake) {
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
  return a || b;
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
  for (size_t i = base; i < stack_.size(); ++i) stack_[i]->draw(cx, ops_);
  if (screensaver::mode() == screensaver::Mode::Dim)
    screensaver::draw(cx, ops_);  // ES draws no toasts over the screensaver
  else
    draw_toasts(cx, ops_);
  cx.assets.end_frame(ops_, kAssetBudget);
  return ops_;
}

}  // namespace zf
