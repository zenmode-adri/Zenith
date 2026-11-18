#include "dock.hpp"

#include <algorithm>

namespace zf::dock {

namespace {

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

// Nine different positions inside +-kShiftX / +-kShiftY cells. The walk steps 4 along the table (4 and 9 share no divisor, so it visits
// all nine before repeating) and two minutes in a row never land on the same one.
constexpr Offset kTable[9] = {{0, 0}, {2, -1}, {-2, 1}, {1, 1}, {-1, -1}, {2, 1}, {-2, -1}, {0, -1}, {0, 1}};

}  // namespace

int parse_minutes(std::string_view s) {
  if (ieq(s, "off")) return 0;
  if (s == "1") return 1;
  if (s == "5") return 5;
  if (s == "10") return 10;
  return kDefaultIdleMinutes;
}

Offset burn_in_offset(int64_t wall_minute) {
  int64_t i = (wall_minute % 9) * 4 % 9;
  if (i < 0) i += 9;
  return kTable[i];
}

Ms ms_to_next_minute(Ms wall_ms) {
  Ms into = wall_ms % 60000;
  if (into < 0) into += 60000;
  return 60000 - into + kMinuteSlackMs;
}

void Dock::configure(const Config& c) {
  cfg_ = c;
  if (!cfg_.charge_anim) anim_on_ = false;
}

bool Dock::update(const Clock& now, const Env& env) {
  const Ms t = now.mono_ms;
  reliable_ = env.reliable;

  // 1. the filtered plugged state
  const bool fresh = !known_ || env.sample_ms < 0 || env.sample_ms != last_sample_;
  const Ms s = env.sample_ms >= 0 ? env.sample_ms : t;  // when the value was read
  if (!known_) {
    known_ = true;
    last_sample_ = env.sample_ms;
    raw_ = stable_ = env.plugged;  // already plugged at start: that is the state, not an event
    raw_since_ = first_ = t;
  } else if (fresh) {
    last_sample_ = env.sample_ms;
    if (env.plugged != raw_) {
      raw_ = env.plugged;
      raw_since_ = s;
    }
  }
  if (fresh && raw_ != stable_ && s - raw_since_ >= confirm_ms(raw_)) {
    stable_ = raw_;
    if (stable_) {
      const bool settled = t - first_ >= kStartupGraceMs;
      const bool cooled = !has_event_ || t - last_event_ >= kEventCooldownMs;
      if (settled && cooled && !env.quiet) {
        has_event_ = true;
        last_event_ = t;
        if (cfg_.charge_anim) {
          event_pending_ = true;
          anim_on_ = true;
          anim_start_ = t;
          redraw_ = true;
        }
      }
    }
  }

  // 2. the notice: frames while it moves, one more redraw to erase it
  if (anim_on_) {
    if (t - anim_start_ >= kAnimMs) {
      anim_on_ = false;
      redraw_ = true;
    } else if (cfg_.motion) {
      redraw_ = true;
    }
  }

  const bool r = redraw_;
  redraw_ = false;
  return r;
}

bool Dock::take_plug_event() {
  const bool e = event_pending_;
  event_pending_ = false;
  return e;
}

float Dock::anim_t(const Clock& now) const {
  if (!anim_on_) return 0.0f;
  const float t = float(now.mono_ms - anim_start_) / float(kAnimMs);
  return std::clamp(t, 0.0f, 1.0f);
}

std::optional<Ms> Dock::next_deadline(const Clock& now) const {
  std::optional<Ms> d;
  auto soon = [&d](Ms v) {
    if (!d || v < *d) d = v;
  };
  if (raw_ != stable_) {
    const Ms due = raw_since_ + confirm_ms(raw_);
    soon(due > now.mono_ms ? due : now.mono_ms + kResampleMs);  // due but no fresh reading yet: look again soon (the caller re-reads)
  }
  if (anim_on_) {
    const Ms end = anim_start_ + kAnimMs;
    soon(cfg_.motion ? std::min(now.mono_ms + kFrameMs, end) : end);
  }
  return d;
}

}  // namespace zf::dock
