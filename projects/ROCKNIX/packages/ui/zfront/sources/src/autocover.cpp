#include "autocover.hpp"

#include <algorithm>
#include <string>

#include "i18n.hpp"
#include "menu_scraper.hpp"
#include "oscontract.hpp"

namespace zf::autocover {

Block gate(const Conditions& c) {
  if (!c.enabled) return Block::Off;
  if (c.idle < kIdle) return Block::Idle;
  if (c.game_running) return Block::Game;
  if (c.busy) return Block::Busy;
  if (!c.network) return Block::Network;
  if (c.battery && !c.battery->charging && c.battery->percent < kMinBattery) return Block::Battery;
  if (!c.source_usable) return Block::Source;
  if (!c.systems) return Block::NoSystems;
  return Block::None;
}

bool source_usable(const Ctx& cx) {
  using scraper::Source;
  const Source s = scraper::source_of(cx.es);
  if (!scraper::available(s)) return false;
  if (s == Source::IGDB) {
    const std::string* id = cx.es.get(scraper::key::kIgdbId);
    const std::string* secret = cx.es.get(scraper::key::kIgdbSecret);
    return id && secret && !id->empty() && !secret->empty();
  }
  return true;
}

scraper::Options make_options(Ctx& cx) {
  scraper::Options o = scraper_run_options(cx);  // ES's settings, the user's choice of systems
  o.filter = scraper::Options::Filter::MissingAny;
  o.overwrite_media = o.overwrite_names = o.overwrite_desc = false;
  o.wish.video = o.wish.fanart = o.wish.boxback = o.wish.manual = o.wish.map = o.wish.titleshot = o.wish.ratings = false;
  if (o.skip_days < 1) o.skip_days = 1;  // "IGNORE RECENTLY SCRAPED: NO" must not mean one look per idle period
  o.only_game.clear();
  o.background = true;
  return o;
}

Controller::Controller(Ctx& cx, TimePoint now) : cx_(cx), last_input_(now), next_check_(now + kIdle) {
  game_running = [] { return oscontract::game_running(); };
  busy = [] { return oscontract::busy(); };
}

Controller::~Controller() {
  if (run_ && run_->working()) run_->cancel();
}

void Controller::input(TimePoint now) {
  last_input_ = now;
  next_check_ = now + kIdle;
  armed_ = true;
  if (run_ && run_->working()) run_->cancel();
}

void Controller::game_started() {
  if (run_ && run_->working()) run_->cancel();
  played_ = true;  // a game is activity: the next poll restarts the idle wait (it started a new run at once after a game)
}

Conditions Controller::conditions(TimePoint now) const {
  Conditions c;
  c.enabled = cx_.es.get_bool(kKeyEnabled, false);
  c.idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_);
  c.game_running = game_running && game_running();
  c.busy = busy && busy();
  c.network = cx_.status.wifi;
  c.battery = cx_.status.battery;
  c.source_usable = source_usable(cx_);
  c.systems = !scraper_run_options(cx_).systems.empty();
  return c;
}

bool Controller::finish() {
  const scraper::Stats& st = run_->stats();
  const size_t n = st.media_games;
  last_phase_ = run_->phase();
  // a run that ended by itself (queue done, or the source refused) waits for a new idle period; one stopped by an input is already re-armed
  if (last_phase_ != scraper::Phase::Cancelled && last_input_ <= run_started_) armed_ = false;
  run_.reset();
  if (n == 0) return false;
  cx_.sv->toasts.push(std::to_string(n) + " " + tr("COVERS ADDED"), 3000);
  return true;
}

bool Controller::poll(TimePoint now) {
  bool toast = false;
  if (played_) {
    played_ = false;
    last_input_ = now;
    next_check_ = now + kIdle;
    armed_ = true;
  }
  if (run_) {
    run_->tick();
    if (!run_->finished()) return false;
    toast = finish();
  }
  if (!cx_.es.get_bool(kKeyEnabled, false)) {
    block_ = Block::Off;
    return toast;
  }
  if (!armed_ || now < next_check_) return toast;
  block_ = gate(conditions(now));
  if (block_ == Block::Idle) {
    next_check_ = last_input_ + kIdle;
    return toast;
  }
  if (block_ != Block::None) {
    next_check_ = now + kRecheck;
    return toast;
  }
  scraper::Options o = make_options(cx_);
  if (tune) tune(o);
  run_ = scraper::Run::create(cx_, std::move(o));
  run_->set_watched(true);  // quiet: the run shows no toast of its own, `finish` decides
  run_started_ = now;
  ++started_;
  run_->start();
  return toast;
}

std::optional<TimePoint> Controller::deadline() const {
  if (run_) {
    std::optional<float> w = run_->wake_in();
    if (run_->wait_seconds() > 0) w = w ? std::min(*w, 1.0f) : 1.0f;
    if (!w) return std::nullopt;  // a finished job wakes the loop
    return Clock::now() + std::chrono::milliseconds(long(*w * 1000.0f) + 1);
  }
  if (!armed_ || !cx_.es.get_bool(kKeyEnabled, false)) return std::nullopt;
  return next_check_;
}

}  // namespace zf::autocover
