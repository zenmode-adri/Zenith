#include "app.hpp"
#include "addgames.hpp"  // the card's name, again in a new language

#include "power.hpp"

#include "sound.hpp"

#include <cstdio>

#include "collections.hpp"
#include "i18n.hpp"
#include "colsys.hpp"
#include "dock_screen.hpp"
#include "motion.hpp"
#include "music.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "remake.hpp"
#include "screensaver.hpp"
#include "ssmedia.hpp"
#include "syslist.hpp"
#include "playstats.hpp"
#include "uimode.hpp"
#include "util.hpp"
#include "webtransfer.hpp"
#include "ui_prefs.hpp"
#include "vpreview.hpp"

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
  if (systems[i].gamelist_damaged && !systems[i].damaged_told) {  // the games before the damage are shown; the toast says which system
    systems[i].damaged_told = true;
    sv->toasts.push(std::string(tr("DAMAGED GAME LIST")) + ": " + systems[i].fullname, 4000);
  }
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
  if (o.kind == Outcome::None) {
    maybe_reload_systems();  // the menus just closed: apply what they changed
    maybe_refresh_collections();
    maybe_apply_collections();
  }
  return o;
}

void App::push_screen(std::unique_ptr<Screen> s) {
  if (s) apply(Transition::push(std::move(s)));
}

std::optional<Outcome> App::take_pending_launch() {
  std::optional<Outcome> o = std::move(pending_launch_);
  pending_launch_.reset();
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
constexpr std::chrono::milliseconds kChargeStep{500};  // XMB: one more cell every half second while charging
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
      if (stack_.size() >= 2) {  // a list that replaced the list of another system: the carousel below follows it
        const long item = stack_.back()->shown_item(cx);
        if (item >= 0) stack_[stack_.size() - 2]->reset_system_cursor(cx, item);
      }
      break;
    case Transition::Open: {
      std::unique_ptr<Screen> s = Registry::instance().make(t.name, cx, t.arg);
      if (s) {
        if (t.name == "menu.main" || t.name == "menu.game_options") sound::play(cx, sound::Cue::MenuOpen);  // ES: the menus of the list
        stack_.push_back(std::move(s));
        begin_enter();
      } else if (!uimode::screen_allowed(uimode::of(cx.es), t.name)) {
        // locked (Kiosk / Kid): ES does not offer it either, so there is nothing to explain
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
      // is back on the list with the same cursor, as in ES. A tool opened from a menu (`keep_menus`) leaves them: back on that menu.
      menus_kept_ = t.keep_menus;
      while (!t.keep_menus && stack_.size() > 1 && (stack_.back()->is_menu() || stack_.back()->closes_on_launch())) {
        if (stack_.back()->is_menu()) stack_.back()->on_dismiss(cx);
        stack_.pop_back();
      }
      return Outcome{Outcome::Launch, std::move(t.cmd), t.sys, t.game};
    case Transition::Quit:
      return Outcome{Outcome::Quit, {}, -1, -1};
  }
  return {};
}

void App::after_launch() {
  if (!menus_kept_) return;
  menus_kept_ = false;
  if (!cx.conf.path().empty()) cx.conf = SystemConf::load(cx.conf.path());  // the tool may have changed settings behind our back
  stack_.back()->on_back_from_launch(cx);
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
  if (items_gen_ != i18n::generation()) {  // the systems named by a caption (Tools, Screenshots, Music...) take it in the language now loaded
    items_gen_ = i18n::generation();
    for (SystemItem& it : cx.items)
      if (it.sys == kItemAddGames) it.full_name = addgames::card_label();
      else if (it.sys < cx.systems.size())
        if (const char* label = system_icon_label(cx.systems[it.sys].theme)) it.full_name = label;
  }
  music::on_system(cx, stack_[0]->system_cursor(cx));  // audio.persystem: another track when the system changes
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
    if (leave_.t >= (leave_.menu ? kLeaveMenu : kLeavePlain)) {
      leave_.screens.clear();
      maybe_reload_systems();  // what waited for the leaving screens
      maybe_refresh_collections();
      maybe_apply_collections();
    }
    moving = true;
  }
  bool redraw = moving || animating_ || stack_.back()->take_redraw();
  animating_ = moving;
  Transition t = stack_.back()->take_transition(cx);
  if (t.kind != Transition::None) {
    Outcome o = apply(std::move(t));  // Quit is not meant to come from here; a Launch (a countdown that ended) goes to the loop
    if (o.kind == Outcome::Launch) pending_launch_ = std::move(o);
    redraw = true;
  }
  if (maybe_reload_systems()) redraw = true;
  return redraw;
}

// The PSP filled its three cells one after another while charging. Only XMB, only with motion on, and not under the screensaver
// (nothing of the screens shows then); a status pinned with its own frame (harness `--status frame=N`) keeps it. A full battery
// still plugged in ("Full" counts as charging, as in ES) shows its three cells still, as the PSP did.
bool App::charge_anim() const {
  const Status& s = status_->status();
  return remake::cross(cx.th) && cx.ui_motion && s.battery && s.battery->charging && s.battery->percent < 100 && s.charge_frame < 0 &&
         !oscontract::idle().active();
}

std::optional<TimePoint> App::status_deadline() const {
  std::optional<TimePoint> t = status_->next_deadline();
  if (charge_anim() && (!t || charge_next_ < *t)) t = charge_next_;
  if (const std::optional<dock::Ms> d = dock_.next_deadline(dock_clock(Clock::now()))) {  // a plug waiting to be confirmed, a frame of the notice
    const TimePoint dt = TimePoint(std::chrono::milliseconds(*d));
    if (!t || dt < *t) t = dt;
  }
  return t;
}

dock::Clock App::dock_clock(TimePoint now) {
  const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
  return dock::Clock{std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(), wall.count()};
}

// The plug-in filter and the notice (dock.hpp). True when a frame of the notice is due (or it just ended).
bool App::poll_dock(const dock::Clock& dk, TimePoint now) {
  const unsigned reads = status_->battery_reads();
  if (reads != dock_reads_) {  // a real reading of the kernel since the last call
    dock_reads_ = reads;
    dock_sample_ms_ = reads == 0 ? -1 : dk.mono_ms;
  }
  dock::Config dc;
  dc.motion = cx.ui_motion;
  dock_.configure(dc);
  const Status& rs = status_->status();  // the raw status: the preferences that hide the battery from the bar do not hide the charger
  const bool quiet = oscontract::game_running() || oscontract::idle().active();
  const bool redraw = dock_.update(dk, dock::Env{rs.battery && rs.battery->plugged, quiet, dock_sample_ms_, rs.battery && rs.battery->from_charger});
  if (dock_.take_plug_event()) oscontract::input(now);  // a plug is activity: the clock comes its minutes AFTER it, and the notice is seen
  const std::string* bc = cx.es.get("BedsideClock");
  oscontract::set_bedside(dock_.plugged() ? dock::parse_minutes(bc ? std::string_view(*bc) : std::string_view()) : 0);
  dock::PillState& ps = dock::pill_state();
  ps.on = !quiet && dock_.anim_active(dk);
  ps.t = dock_.anim_t(dk);
  ps.motion = cx.ui_motion;
  return redraw && !oscontract::idle().active();
}

bool App::snapshot_eligible() const {
  return stack_.size() == 1 && !enter_.active && leave_.screens.empty() && !animating_ && cx.sv->toasts.items().empty() &&
         stack_[0]->snapshot_ok(cx);
}

namespace {
long long webtransfer_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

bool App::poll_status(TimePoint now) {
  power::tick(cx);
  if (webtransfer::idle_expired(webtransfer_now_ms())) {  // the temporary transfer of "Add games": 20 min with no phone
    webtransfer::apply(false);
    cx.sv->toasts.push(tr("Web transfer turned off"), 3500);
  }
  if (oscontract::take_resumed()) status_->invalidate();  // back from a suspend: clock, battery, WiFi and Bluetooth are old
  if (take_status_refresh()) status_->invalidate();       // the Bluetooth switch: a service starts or stops, no kernel event says so
  const bool ss = ssmedia::poll(cx, now);  // slideshow / random video screensaver: starts, swaps, fades (one comparison when off)
  const dock::Clock dk = dock_clock(now);
  if (dock_.wants_fresh_sample(dk)) status_->reread();  // a plug / unplug waits for a fresh reading to be confirmed
  bool changed = status_->poll(now);
  const bool dr = poll_dock(dk, now);
  const int8_t was = charge_frame_;
  if (!charge_anim()) {
    charge_frame_ = -1;
  } else if (charge_frame_ < 0 || now >= charge_next_) {
    charge_frame_ = int8_t((charge_frame_ + 1) % 4);  // empty, one, two, three, empty...
    charge_next_ = now + kChargeStep;
  }
  if (!changed && charge_frame_ == was) return ss || dr;
  Status s = status_->status();
  if (s.charge_frame < 0) s.charge_frame = charge_frame_;
  ui_prefs::on_status(cx, s);
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
  maybe_refresh_collections();
  const bool d = maybe_apply_collections();  // a rebuild that waited for the carousel
  return a || b || c || d;
}

bool App::maybe_reload_systems() {
  const bool rescan = collections::rescan_needed();
  // a list that is still leaving the screen draws its system: nothing is replaced under it
  if (!cx.system_loader || stack_.size() != 1 || !leave_.screens.empty() || !(collections::reload_needed() || rescan) || !cx.sv->jobs.idle()) return false;
  collections::set_reload_needed(false);
  collections::set_rescan_needed(false);
  colsys::request();  // enabled collections, custom files or deleted games may have changed even when the systems did not
  std::vector<System> fresh;
  std::string err;
  if (!cx.system_loader(cx.es.get_bool("LoadEmptySystems", false), fresh, err)) {
    std::fprintf(stderr, "zfront: system list not reloaded: %s\n", err.c_str());  // the carousel stays as it was
    return false;
  }
  // nothing the carousel shows changed (a collection edit, a setting that only matters to collections): keep cursor and state.
  // A rescan reads the folders again whatever the list says: a game copied into a system does not change the list.
  std::vector<SystemItem> items = make_items(fresh, cx.es, cx.cols);
  if (!rescan && same_systems(cx.systems, cx.items, fresh, items, cx.cols)) return false;

  const long old_cursor = stack_[0]->system_cursor(cx);
  std::vector<std::string> old_names = item_names(cx.systems, cx.items, cx.cols);
  // The count workers hold copies of the folders and only queue results, so joining them is all it takes before `systems` changes.
  counts_.reset();
  count_map_.clear();
  if (!rescan) carry_over(cx.systems, fresh);  // games already read and counts already known are kept (a rescan reads them again)
  cx.systems = std::move(fresh);
  ++cols_epoch_;  // a collection build that started on the old systems is out of date (its result is dropped)
  cx.items = make_items(cx.systems, cx.es, cx.cols);
  long cursor = -1;
  if (old_cursor >= 0) {
    cursor = nearest_item(old_names, old_cursor, item_names(cx.systems, cx.items, cx.cols));
    stack_[0]->reset_system_cursor(cx, cursor);
  }
  start_counts(2, cursor >= 0 && !cx.items.empty() ? cx.system_of(size_t(cursor)) : 0);
  webtransfer::publish(cx.systems);
  if (rescan) cx.sv->toasts.push(tr("Game lists updated"), 2500);
  return true;
}

namespace {

// the work of one collection build (job thread, or at once): reads the games of the systems that were not read, then colsys::build
std::vector<System> build_collections(std::shared_ptr<std::vector<System>> snap, const EsSettings& es, const smartcol::Options& o, bool want_log) {
  colsys::keep_game_systems(*snap);  // screenshots, tools, music: not games (and their lists are not read)
  colsys::fill(*snap);
  std::vector<playstats::LogEntry> log;
  if (want_log) {
    std::string text;
    if (read_file(playstats::log_path(), text)) log = playstats::parse_log(text);
  }
  return colsys::build(*snap, es, log, o);
}

}  // namespace

void App::refresh_collections(bool now) {
  if (cols_busy_) {
    cols_again_ = true;
    return;
  }
  if (!colsys::any_enabled(cx.es)) {  // none wanted: the ones shown go away
    if (cx.cols.empty() && !cols_have_) return;
    cols_pending_.clear();
    cols_have_ = true;
    maybe_apply_collections();
    return;
  }
  auto snap = std::make_shared<std::vector<System>>(cx.systems);  // the job reads its own copy: `cx.systems` belongs to this thread
  smartcol::Options o;
  o.now = std::time(nullptr);
  o.mode = uimode::current();
  o.show_hidden = cx.es.get_bool("ShowHiddenFiles", false);
  const EsSettings es = cx.es;
  const bool want_log = colsys::needs_log(es);
  if (now) {
    cols_pending_ = build_collections(snap, es, o, want_log);
    cols_have_ = true;
    maybe_apply_collections();
    return;
  }
  cols_busy_ = true;
  const unsigned epoch = cols_epoch_;
  auto res = std::make_shared<std::vector<System>>();
  cx.sv->jobs.post_slow([snap, es, o, want_log, res] { *res = build_collections(snap, es, o, want_log); },
                        [this, res, epoch] {
                          cols_busy_ = false;
                          if (epoch == cols_epoch_) {
                            cols_pending_ = std::move(*res);
                            cols_have_ = true;
                          } else {
                            cols_again_ = true;  // built on systems that were replaced meanwhile
                          }
                          maybe_apply_collections();
                          if (cols_again_) {
                            cols_again_ = false;
                            refresh_collections();
                          }
                        });
}

bool App::maybe_refresh_collections() {
  if (!colsys::requested() || stack_.size() != 1) return false;
  colsys::clear_request();
  refresh_collections();
  return true;
}

bool App::maybe_apply_collections() {
  if (!cols_have_ || stack_.size() != 1 || !leave_.screens.empty()) return false;  // a leaving collection list still draws `cx.cols`
  cols_have_ = false;
  const long old_cursor = stack_[0]->system_cursor(cx);
  const std::vector<std::string> old_names = item_names(cx.systems, cx.items, cx.cols);
  cx.cols = std::move(cols_pending_);
  cols_pending_.clear();
  // the cards of the real systems stay as they are (their counts may still be arriving); the collection cards are made again
  std::vector<SystemItem> items;
  for (const SystemItem& it : cx.items)
    if (it.sys == kItemIsSystem || it.sys == kItemAddGames || it.sys < cx.systems.size()) items.push_back(it);  // the card stays first
  for (SystemItem& it : make_items(cx.systems, cx.es, cx.cols))
    if (it.sys != kItemIsSystem && it.sys != kItemAddGames && it.sys >= cx.systems.size()) items.push_back(std::move(it));
  cx.items = std::move(items);
  const std::vector<std::string> new_names = item_names(cx.systems, cx.items, cx.cols);
  if (old_cursor >= 0 && (old_names.size() != new_names.size() || old_names != new_names)) {
    // the number of cards changed: the cursor (unwrapped, taken modulo the count) would land elsewhere, so it is put on its card again
    stack_[0]->reset_system_cursor(cx, nearest_item(old_names, old_cursor, new_names));
  }
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
    vpreview::end_frame(true);
    screensaver::draw(cx, ops_);
    return ops_;
  }
  if (screensaver::mode() == screensaver::Mode::Media) {  // slideshow / random video: its own frame, nothing of the UI
    vpreview::end_frame(true);
    ssmedia::draw(cx, ops_);
    cx.assets.end_frame(ops_, kAssetBudget);
    return ops_;
  }
  if (screensaver::mode() == screensaver::Mode::Clock) {  // the bedside clock: its own frame, nothing of the UI, no toasts
    vpreview::end_frame(true);
    const int64_t minute = int64_t(std::time(nullptr)) / 60;
    dock::draw_clock(cx, dock::burn_in_offset(minute), minute, ops_);
    cx.assets.end_frame(ops_, kAssetBudget);
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
  vpreview::end_frame(screensaver::mode() != screensaver::Mode::Off);  // a list preview nobody drew this frame stops
  cx.assets.end_frame(ops_, kAssetBudget);
  return ops_;
}

}  // namespace zf
