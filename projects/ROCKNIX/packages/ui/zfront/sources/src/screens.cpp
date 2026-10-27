#include "screens.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "collections.hpp"
#include "covers.hpp"
#include "detailed.hpp"
#include "i18n.hpp"
#include "launch.hpp"
#include "launchcheck.hpp"
#include "oscontract.hpp"
#include "savestates.hpp"
#include "scene.hpp"

namespace zf {

/// "12 min ago" for a `GameMeta::lastplayed` (YYYYMMDDHHMMSS, local time); empty when never or older than 30 days.
static std::string played_ago(uint64_t lp, std::time_t now) {
  if (lp == 0) return std::string();
  std::tm t{};
  t.tm_year = int(lp / 10000000000ULL) - 1900;
  t.tm_mon = int(lp / 100000000ULL % 100) - 1;
  t.tm_mday = int(lp / 1000000ULL % 100);
  t.tm_hour = int(lp / 10000ULL % 100);
  t.tm_min = int(lp / 100ULL % 100);
  t.tm_sec = int(lp % 100);
  t.tm_isdst = -1;
  return ago_text(std::mktime(&t), now);
}

static float ease_out(float t) {
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}

Transition CarouselScreen::handle(Action a, Ctx& cx) {
  int n = int(cx.items.size());
  switch (a) {
    case Action::Right:
    case Action::R2:  // ES: the triggers step the carousel like left/right
      if (n > 0) {
        ++target_;
        anim_ = true, anim_t_ = 0, anim_from_ = cam_;
      }
      break;
    case Action::Left:
    case Action::L2:
      if (n > 0) {
        --target_;
        anim_ = true, anim_t_ = 0, anim_from_ = cam_;
      }
      break;
    case Action::A:
      if (n > 0 && !anim_) {
        size_t idx = size_t(((target_ % n) + n) % n);
        recent_for_ = -1;
        announced_ = -1;  // coming back announces the system again, as ES does when it returns to the system view
        return Transition::push(std::make_unique<ListScreen>(idx, cx));
      }
      break;
    case Action::Start:
      return Transition::open("menu.main");
    default:
      break;
  }
  return Transition::none();
}

bool CarouselScreen::tick(float dt, Ctx& cx) {
  if (!anim_ && !cx.items.empty()) {  // `system-selected` once the carousel stopped on a system (ES: CURSOR_STOPPED)
    long n = long(cx.items.size());
    long idx = ((long(target_) % n) + n) % n;
    if (idx != announced_) {
      announced_ = idx;
      if (size_t(idx) < cx.systems.size() && oscontract::selected_listening("system-selected")) oscontract::fire_system_selected(cx.systems[size_t(idx)].name);
    }
  }
  if (anim_) {
    float t = anim_t_ + dt;
    float k = std::min(t / kStepSeconds, 1.0f);
    cam_ = anim_from_ + (float(target_) - anim_from_) * ease_out(k);
    if (k >= 1.0f) {
      anim_ = false;
      cam_ = float(target_);
    } else {
      anim_t_ = t;
    }
  }
  bool changed = false;
  if (!anim_ && !cx.items.empty()) {  // the "continue playing" row is read once the carousel stopped, not while it moves
    long n = long(cx.items.size());
    long idx = ((long(target_) % n) + n) % n;
    if (idx != recent_for_) {
      recent_for_ = idx;
      recent_.items = size_t(idx) < cx.systems.size() ? recent_states(cx.systems[size_t(idx)], std::time(nullptr), 3) : std::vector<RecentItem>();
      changed = true;
    }
  }
  return anim_ || changed;
}

void CarouselScreen::draw(Ctx& cx, std::vector<Op>& out) {
  const long n = long(cx.items.size());
  const long idx = n > 0 ? ((long(target_) % n) + n) % n : -1;
  const bool show = !anim_ && idx >= 0 && idx == recent_for_;  // the row belongs to the system the carousel stopped on
  recent_.decode_inline = !cx.async_covers;
  system_view(cx.th, cx.assets, cx.common, cx.w, cx.h, cx.items, target_, cam_, cx.status, show ? &recent_ : nullptr, out);
  if (show && recent_.box_w > 0 && cx.async_covers) {
    // the decoder keeps only the newest request: ask for one screenshot per frame, the next one when this one lands
    for (const RecentItem& it : recent_.items) {
      ImgId id;
      if (covers::failed(it.shot) || covers::ready(cx.assets, it.shot, recent_.box_w, recent_.box_h, id)) continue;
      covers::request(cx.assets, cx.sv->jobs, it.shot, recent_.box_w, recent_.box_h);
      break;
    }
  }
}

ListScreen::ListScreen(size_t system_index, Ctx& cx) : si_(system_index), lines_(size_t(cx.th.f("listLines"))), settle_(kSettleSeconds) {
  cx.open_system(si_);  // the gamelist is read when the user enters the system
  const std::time_t now = std::time(nullptr);
  for (const Game& g : cx.systems[si_].games) {
    names_.push_back(g.name);
    marks_.push_back(played_ago(g.meta.lastplayed, now));
  }
  if (!names_.empty() && oscontract::selected_listening("game-selected")) sel_wait_ = 0;  // the first game counts as selected on entry
}

Transition ListScreen::handle(Action a, Ctx& cx) {
  size_t len = names_.size();
  const size_t prev = cur_;
  if (a == Action::Up || a == Action::Down || a == Action::Left || a == Action::Right || a == Action::L || a == Action::R) settle_ = 0;
  switch (a) {
    case Action::B:
      return Transition::pop();
    case Action::Down:
      if (len > 0) cur_ = (cur_ + 1) % len;
      break;
    case Action::Up:
      if (len > 0) cur_ = (cur_ + len - 1) % len;
      break;
    case Action::Right:
    case Action::R:
      if (len > 0) cur_ = std::min(cur_ + lines_, len - 1);
      break;
    case Action::Left:
    case Action::L:
      if (len > 0) cur_ = cur_ > lines_ ? cur_ - lines_ : 0;
      break;
    case Action::A:
      if (len > 0) {
        if (Transition m = savestates::launch_request(cx, si_, cur_); m.kind != Transition::None) return m;  // HOOK menu-collections: savestate manager first
        Transition t = Transition::launch(command(cx));
        t.sys = int(si_);
        t.game = int(cur_);
        return launchcheck::guard(cx, si_, std::move(t));  // CheckBiosesAtLaunch
      }
      break;
    case Action::Select:  // ES: gamelist options; here the options of this game (menu-game)
      if (len > 0) return Transition::open("menu.game_options", cx.systems[si_].name + "\t" + cx.systems[si_].games[cur_].path);
      break;
    case Action::R3:  // HOOK menu-collections: favourite on/off (ES: long X; here X is "game options")
      if (len > 0) collections::toggle_favorite(cx, si_, cur_);
      break;
    case Action::L3:  // HOOK menu-collections: savestate manager (ES: L3 / long A)
      if (len > 0) return savestates::open_manager(cx, si_, cur_);
      break;
    default:
      break;
  }
  if (cur_ != prev && oscontract::selected_listening("game-selected")) sel_wait_ = 0;  // (re)start the rest timer of the event
  return Transition::none();
}

std::string ListScreen::command(Ctx& cx) {
  const System& sys = cx.systems[si_];
  std::vector<JoyInfo> joys;
  if (cx.pad) joys = cx.pad->joysticks();
  return launch::build_command(sys, sys.games[cur_], cx.conf.map(), joys) + savestates::plain_args(cx, si_, cur_);  // HOOK menu-collections
}

std::string ListScreen::launch_target(Ctx& cx) { return cur_ < names_.size() ? command(cx) : std::string(); }

// No animation here, only timers: `tick` never asks the loop to spin; it marks a redraw when the settle delay ends or the
// description scrolls a pixel, and `wake_in` tells the loop when to wake up (it sleeps in between).
bool ListScreen::tick(float dt, Ctx& cx) {
  if (sel_wait_ >= 0) {
    sel_wait_ += double(dt);
    if (sel_wait_ >= kSettleSeconds) {
      sel_wait_ = -1;
      if (cur_ < names_.size()) {
        const Game& g = cx.systems[si_].games[cur_];
        oscontract::fire_game_selected(cx.systems[si_].name, g.path, g.name);
      }
    }
  }
  if (!detailed_enabled()) return false;
  double before = settle_;
  settle_ += double(dt);
  if (before < kSettleSeconds) {
    if (settle_ >= kSettleSeconds) redraw_ = true;  // cover and description appear
  } else if (max_scroll_ > 0 && desc_scroll_offset(before - kSettleSeconds, max_scroll_) != desc_scroll_offset(settle_ - kSettleSeconds, max_scroll_)) {
    redraw_ = true;
  }
  return false;
}

std::optional<float> ListScreen::wake_in() const {
  std::optional<float> d = detail_wake_in();
  if (sel_wait_ < 0) return d;
  float s = std::max(float(kSettleSeconds - sel_wait_), 0.0f) + 0.002f;
  return d ? std::min(*d, s) : s;
}

std::optional<float> ListScreen::detail_wake_in() const {
  if (!detailed_enabled()) return std::nullopt;
  double s;
  if (settle_ < kSettleSeconds) {
    s = kSettleSeconds - settle_;
  } else if (max_scroll_ > 0) {
    s = desc_scroll_next(settle_ - kSettleSeconds, max_scroll_);
  } else {
    return std::nullopt;
  }
  return float(s) + 0.002f;  // a hair late rather than a hair early (no double wake-up)
}

void ListScreen::draw(Ctx& cx, std::vector<Op>& out) {
  bool settled = settle_ >= kSettleSeconds;
  if (detailed_enabled() && (!d_valid_ || d_idx_ != cur_ || d_settled_ != settled)) {
    d_have_ = detailed_for(cx.systems[si_], cur_, settled, d_);
    d_valid_ = true, d_idx_ = cur_, d_settled_ = settled;
  }
  if (d_have_) {
    d_.desc_time = settled ? settle_ - kSettleSeconds : -1.0;
    d_.decode_inline = !cx.async_covers;
  }
  list_view(cx.th, cx.assets, cx.common, cx.w, cx.h, cx.systems[si_].fullname, names_, cur_, cx.status, d_have_ ? &d_ : nullptr, &marks_, out);
  max_scroll_ = d_have_ ? d_.desc_max_scroll : 0;
  // the cover is decoded on the job thread; its `done` inserts it and the loop redraws (covers.hpp)
  if (d_have_ && d_.cover_missing) covers::request(cx.assets, cx.sv->jobs, d_.image, d_.cover_w, d_.cover_h);
}

}  // namespace zf
