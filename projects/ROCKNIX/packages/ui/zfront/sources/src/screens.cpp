#include "screens.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
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
#include "sound.hpp"
#include "scene.hpp"

namespace zf {

/// "12 min ago" for a `GameMeta::lastplayed` (YYYYMMDDHHMMSS, local time); empty when never or older than 30 days.
// A tile of the "continue playing" row needs its ROM: a state left behind by a deleted ROM would give an A that does nothing.
// The usual flat folder costs one stat per extension; anything else (sub folder, upper-case extension) asks the game list.
static bool rom_present(Ctx& cx, size_t si, const std::string& stem) {
  const System& sys = cx.systems[si];
  for (const std::string& e : sys.exts) {
    struct stat st;
    if (::stat(path_join(sys.rom_dir, stem + e).c_str(), &st) == 0) return true;
  }
  size_t game = 0;
  return savestates::find_game(cx, si, stem, game);
}

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

bool CarouselScreen::row_live(const Ctx& cx) const {
  const long n = long(cx.items.size());
  const long idx = n > 0 ? ((long(target_) % n) + n) % n : -1;
  return !anim_ && idx >= 0 && idx == recent_for_ && idx == info_idx_;  // the row belongs to the system the carousel stopped on
}

std::vector<size_t> CarouselScreen::row_tiles(Ctx& cx) {
  const long n = long(cx.items.size());
  if (n == 0) return {};
  const long idx = ((long(target_) % n) + n) % n;
  return recent_visible(cx.th, cx.assets, cx.w, cx.h, cx.items[size_t(idx)], recent_);
}

void CarouselScreen::set_focus(int item, const Ctx& cx) {
  focus_ = item;
  if (item >= 0) {
    ring_idx_ = item;
    if (!cx.ui_motion) ring_a_ = 1.0f;
  } else if (!cx.ui_motion) {
    ring_a_ = 0.0f;
    ring_idx_ = -1;
  }
}

Transition CarouselScreen::launch_recent(Ctx& cx) {
  if (focus_ < 0 || size_t(focus_) >= recent_.items.size() || !row_live(cx)) return Transition::none();
  const RecentItem it = recent_.items[size_t(focus_)];
  const size_t sys = size_t(recent_for_);
  size_t game = 0;
  if (sys >= cx.systems.size() || !savestates::find_game(cx, sys, it.name, game)) return Transition::none();  // the ROM is gone
  // the same launch as picking that state in the savestate manager (BIOS question included)
  Transition t = savestates::launch_state(cx, sys, game, it.slot);
  if (t.kind != Transition::None) {  // the game starts, or the BIOS question comes first (YES starts it, NO changes nothing)
    resume_name_ = it.name;          // when it ends the row is read again (new capture, new order) and the keys return to its tile
    reread_ = true;
  }
  if (t.kind == Transition::Launch) {
    focus_ = ring_idx_ = -1;
    ring_a_ = 0.0f;
    recent_for_ = -1;
  }
  return t;
}

Transition CarouselScreen::handle(Action a, Ctx& cx) {
  int n = int(cx.items.size());
  if (focus_ >= 0) {  // the keys are on the "continue playing" row
    switch (a) {
      case Action::Left:
      case Action::Right: {
        const std::vector<size_t> vis = row_tiles(cx);
        auto at = std::find(vis.begin(), vis.end(), size_t(focus_));
        if (vis.empty()) {
          set_focus(-1, cx);
        } else if (at == vis.end()) {
          set_focus(int(vis.front()), cx);
        } else if (a == Action::Left && at != vis.begin()) {  // no wrap: a short row, a stray press must not jump
          set_focus(int(*(at - 1)), cx);
        } else if (a == Action::Right && at + 1 != vis.end()) {
          set_focus(int(*(at + 1)), cx);
        }
        return Transition::none();
      }
      case Action::A:
        return launch_recent(cx);
      case Action::B:
      case Action::Up:
        set_focus(-1, cx);
        return Transition::none();
      case Action::L2:
      case Action::R2:  // the triggers keep stepping the carousel: the keys go back to it
        set_focus(-1, cx);
        break;
      default:  // Start (menu) and the rest as on the carousel
        break;
    }
  }
  switch (a) {
    case Action::Down:  // the row is under the carousel
      if (focus_ < 0 && row_live(cx)) {
        const std::vector<size_t> vis = row_tiles(cx);
        if (!vis.empty()) set_focus(int(vis.front()), cx);
      }
      break;
    case Action::Right:
    case Action::R2:  // ES: the triggers step the carousel like left/right
      if (n > 0) {
        ++target_;
        anim_ = true;
        sound::play(cx, sound::Cue::Scroll);
      }
      break;
    case Action::Left:
    case Action::L2:
      if (n > 0) {
        --target_;
        anim_ = true;
        sound::play(cx, sound::Cue::Scroll);
      }
      break;
    case Action::A:
      if (n > 0 && std::fabs(float(target_) - cam_) < kGlideAcceptA) {
        size_t idx = size_t(((target_ % n) + n) % n);
        recent_for_ = -1;
        announced_ = -1;  // coming back announces the system again, as ES does when it returns to the system view
        return Transition::push(std::make_unique<ListScreen>(cx.system_of(idx), cx));
      }
      break;
    case Action::Start:
      return Transition::open("menu.main");
    default:
      break;
  }
  return Transition::none();
}

long CarouselScreen::system_cursor(const Ctx& cx) const {
  const long n = long(cx.items.size());
  return n > 0 ? ((long(target_) % n) + n) % n : -1;
}

void CarouselScreen::reset_system_cursor(Ctx& cx, long item) {
  (void)cx;
  target_ = int(item);
  cam_ = float(item);
  anim_ = false;
  info_idx_ = -1;  // the text under the carousel is built again for the new system, without a fade
  info_a_ = 1.0f;
  announced_ = -1;
  recent_ = RecentRow();
  recent_for_ = -1;
}

bool CarouselScreen::tick(float dt, Ctx& cx) {
  if (!anim_ && !cx.items.empty()) {  // `system-selected` once the carousel stopped on a system (ES: CURSOR_STOPPED)
    long n = long(cx.items.size());
    long idx = ((long(target_) % n) + n) % n;
    if (idx != announced_) {
      announced_ = idx;
      const size_t si = cx.system_of(size_t(idx));
      if (si < cx.systems.size() && oscontract::selected_listening("system-selected")) oscontract::fire_system_selected(cx.systems[si].name);
    }
  }
  if (anim_) {
    const float to = float(target_);
    cam_ += (to - cam_) * (1.0f - std::exp(-kGlideRate * dt));
    if (std::fabs(to - cam_) < kGlideEnd) {
      anim_ = false;
      cam_ = to;
    }
  }
  bool changed = false;
  if (!cx.items.empty()) {
    const long n = long(cx.items.size());
    const long cur = ((long(target_) % n) + n) % n;
    if (!cx.ui_motion || info_idx_ < 0 || info_idx_ >= n) {
      info_idx_ = cur;
      info_a_ = 1.0f;
    } else if (cur != info_idx_) {  // leaving the old text: quick
      info_a_ = std::max(0.0f, info_a_ - dt / kInfoOut);
      if (info_a_ <= 0.0f) info_idx_ = cur;
      changed = true;
    } else if (info_a_ < 1.0f) {  // the new text comes in as the camera settles
      if (std::fabs(float(target_) - cam_) < kInfoInAt) info_a_ = std::min(1.0f, info_a_ + dt / kInfoIn);
      changed = true;
    }
  }
  if (!anim_ && !cx.items.empty()) {  // the "continue playing" row is read once the carousel stopped, not while it moves
    if (reread_) recent_for_ = -1, reread_ = false;  // a game was started from the row (its dialog or its run is over now)
    long n = long(cx.items.size());
    long idx = ((long(target_) % n) + n) % n;
    if (idx != recent_for_) {
      recent_for_ = idx;
      const size_t si = cx.system_of(size_t(idx));
      recent_.items.clear();
      if (si < cx.systems.size()) {
        constexpr size_t kTiles = 3;
        recent_.items = recent_states(cx.systems[si], std::time(nullptr), kTiles * 3);  // spare ones replace the tiles without ROM
        recent_.items.erase(std::remove_if(recent_.items.begin(), recent_.items.end(), [&](const RecentItem& it) { return !rom_present(cx, si, it.name); }),
                            recent_.items.end());
        if (recent_.items.size() > kTiles) recent_.items.resize(kTiles);
      }
      changed = true;
      if (!resume_name_.empty()) {  // back from a game started on the row: the keys return to the tile of that game
        for (size_t i = 0; i < recent_.items.size(); ++i)
          if (recent_.items[i].name == resume_name_) set_focus(int(i), cx);
        resume_name_.clear();
      }
    }
  }
  if (focus_ >= 0 && !row_live(cx)) set_focus(-1, cx);  // the row went away (the carousel moved, a menu changed the language...)
  const float ring_to = focus_ >= 0 ? 1.0f : 0.0f;
  if (ring_a_ != ring_to) {  // focus ring fade (without motion `set_focus` already put it there)
    const float step = cx.ui_motion ? dt / kRingFade : 1.0f;
    ring_a_ = ring_to > ring_a_ ? std::min(ring_to, ring_a_ + step) : std::max(ring_to, ring_a_ - step);
    if (ring_a_ <= 0.0f) ring_idx_ = -1;
    changed = true;
  }
  return anim_ || changed;
}

void CarouselScreen::draw(Ctx& cx, std::vector<Op>& out) {
  const bool show = row_live(cx);
  recent_.decode_inline = !cx.async_covers;
  recent_.focus = show ? ring_idx_ : -1;
  recent_.focus_a = ring_a_;
  recent_.focus_help = show && focus_ >= 0;
  system_view(cx.th, cx.assets, cx.common, cx.w, cx.h, cx.items, target_, cam_, cx.status, show ? &recent_ : nullptr, InfoFade{int(info_idx_), info_a_}, out);
  if (show && recent_.box_w > 0 && cx.async_covers) {
    // the decoder keeps only the newest request: ask for one screenshot per frame, the next one when this one lands
    for (const RecentItem& it : recent_.items) {
      ImgId id;
      if (covers::failed(it.shot) || covers::ready(cx.assets, it.shot, recent_.box_w, recent_.box_h, id)) continue;
      covers::request(cx.assets, cx.sv->jobs, it.shot, recent_.box_w, recent_.box_h);
      break;
    }
  }
  if (show && focus_ >= 0) {  // the tile with the keys failed to decode and is not drawn: they go to the last one that is
    const std::vector<size_t> vis = row_tiles(cx);
    if (std::find(vis.begin(), vis.end(), size_t(focus_)) == vis.end()) set_focus(vis.empty() ? -1 : int(vis.back()), cx);
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
      sound::play(cx, sound::Cue::Back);
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
  if (cur_ != prev) sound::play(cx, sound::Cue::Scroll);  // only when the cursor really moved
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
