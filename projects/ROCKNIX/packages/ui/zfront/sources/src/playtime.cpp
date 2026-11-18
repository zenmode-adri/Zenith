// PLAY TIME LIMIT, the pure part (playtime.hpp): the day's total, its file, the session clock and the watcher thread.
#include "playtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "util.hpp"

namespace zf::playtime {

namespace {

constexpr long kMaxUsedMs = 2L * 24 * 3600 * 1000;  // more than two days in one day: the file is not ours
constexpr long kSaveMs = 60 * 1000;                 // a look at least this often
constexpr long kRetryMs = 30 * 1000;                // RetroArch quits at the first SIGTERM and exits without saving at the second: be patient
constexpr const char* kHeader = "zfront-playtime 1";

bool all_digits(std::string_view s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

int days_in(int year, int month) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 2 && leap ? 29 : kDays[month - 1];
}

}  // namespace

// days since 1970-01-01 of a valid "YYYY-MM-DD" key (Date::valid / Tracker::parse guarantee the shape)
static long days_of_key(const std::string& k) {
  int y = std::atoi(k.substr(0, 4).c_str());
  const int m = std::atoi(k.substr(5, 2).c_str());
  const int d = std::atoi(k.substr(8, 2).c_str());
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const long yoe = y - era * 400;
  const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

int limit_minutes(std::string_view s) {
  if (!all_digits(s) || s.size() > 4) return 0;
  const int v = std::atoi(std::string(s).c_str());
  return v >= 1 && v <= 1440 ? v : 0;
}

bool Date::valid() const {
  return year >= kFirstValidYear && year <= kLastValidYear && month >= 1 && month <= 12 && day >= 1 && day <= days_in(year, month);
}

std::string Date::key() const {
  char b[24];
  std::snprintf(b, sizeof b, "%04d-%02d-%02d", year, month, day);
  return b;
}

Date local_today() {
  Date d;
  const time_t now = ::time(nullptr);
  struct tm tm;
  if (now == time_t(-1) || !::localtime_r(&now, &tm)) return d;
  d.year = tm.tm_year + 1900;
  d.month = tm.tm_mon + 1;
  d.day = tm.tm_mday;
  return d;
}

// ---- Tracker ------------------------------------------------------------------------------------------------------------------------

Tracker Tracker::parse(std::string_view text) {
  Tracker t;
  if (text.empty() || text.size() > 512 || text.find('\0') != std::string_view::npos) return t;
  bool header = false, have_day = false, have_used = false;
  std::string day;
  long used = 0;
  size_t i = 0;
  while (i <= text.size()) {
    size_t j = text.find('\n', i);
    if (j == std::string_view::npos) j = text.size();
    std::string_view line = text.substr(i, j - i);
    i = j + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;
    if (!header) {
      if (line != kHeader) return Tracker();
      header = true;
    } else if (line.substr(0, 4) == "day=" && !have_day) {
      std::string_view v = line.substr(4);
      if (!v.empty()) {
        // YYYY-MM-DD, a real date
        if (v.size() != 10 || v[4] != '-' || v[7] != '-' || !all_digits(v.substr(0, 4)) || !all_digits(v.substr(5, 2)) || !all_digits(v.substr(8, 2)))
          return Tracker();
        Date d;
        d.year = std::atoi(std::string(v.substr(0, 4)).c_str());
        d.month = std::atoi(std::string(v.substr(5, 2)).c_str());
        d.day = std::atoi(std::string(v.substr(8, 2)).c_str());
        if (!d.valid()) return Tracker();
        day = std::string(v);
      }
      have_day = true;
    } else if (line.substr(0, 5) == "used=" && !have_used) {
      std::string_view v = line.substr(5);
      if (!all_digits(v) || v.size() > 7) return Tracker();
      used = std::atol(std::string(v).c_str()) * 1000;
      if (used > kMaxUsedMs) return Tracker();
      have_used = true;
    } else {
      return Tracker();
    }
  }
  if (!header || !have_day || !have_used) return Tracker();
  t.day_ = day;
  t.used_ms_ = used;
  return t;
}

std::string Tracker::text() const {
  char b[96];
  std::snprintf(b, sizeof b, "%s\nday=%s\nused=%ld\n", kHeader, day_.c_str(), used_ms_ / 1000);
  return b;
}

void Tracker::set_date(const Date& today) {
  if (!today.valid()) return;  // a clock that was lost: the day is unknown, nothing changes
  const std::string key = today.key();
  if (day_.empty()) {
    day_ = key;  // the first valid date adopts the time counted before it
    dirty_ = true;
  } else if (key < day_ && days_of_key(day_) - days_of_key(key) > 1) {
    // the stored day is MORE than a day ahead of a valid today (the date was moved far forward once and then put right): kept, the day would
    // stay blocked until then. Adopt today and keep what was counted (no time is given); tomorrow, for real, it starts again.
    day_ = key;
    dirty_ = true;
  } else if (key > day_) {  // later: a new day. Equal or earlier (the clock went back a little): nothing changes
    day_ = key;
    used_ms_ = 0;
    warned_ = expired_ = false;
    dirty_ = true;
  }
}

Events Tracker::add(long ms, int limit_min) {
  if (ms > 0) {
    used_ms_ = std::min(used_ms_ + ms, kMaxUsedMs);
    dirty_ = true;
  }
  Events e;
  if (limit_min <= 0) {
    warned_ = expired_ = false;
    return e;
  }
  const long left = limit_min * 60000L - used_ms_;
  if (left <= 0) {
    if (!expired_) e.expired = true;
    expired_ = warned_ = true;
  } else if (left <= kWarnMs) {
    if (!warned_) e.warn = true;
    warned_ = true;
    expired_ = false;
  } else {
    warned_ = expired_ = false;
  }
  return e;
}

long Tracker::left_ms(int limit_min) const {
  if (limit_min <= 0) return -1;
  return std::max(0L, limit_min * 60000L - used_ms_);
}

Tracker load_file(const std::string& path) {
  std::string s;
  if (!read_file(path, s)) return Tracker();
  return Tracker::parse(s);
}

bool Saver::tick(Tracker& t, long now_ms, bool force) {
  if (!t.dirty()) return false;
  if (!force && saved_ && now_ms - last_ < interval_) return false;
  if (!write_atomic(path_, t.text())) return false;  // stays dirty: the next look tries again
  t.clear_dirty();
  last_ = now_ms;
  saved_ = true;
  return true;
}

// ---- Session ------------------------------------------------------------------------------------------------------------------------

Session::Step Session::advance(long now_ms, const Date& today) {
  Step st;
  st.wake_ms = kSaveMs;
  if (limit_ <= 0) return st;
  t_.set_date(today);
  long dt = now_ms - last_;
  if (dt < 0) dt = 0;  // never back: nothing is added and `last_` stays, so the same stretch is not counted twice
  else last_ = now_ms;
  st.ev = t_.add(dt, limit_);
  saver_.tick(t_, now_ms);
  const long left = t_.left_ms(limit_);
  if (left <= 0) st.wake_ms = kRetryMs;
  else if (left > kWarnMs) st.wake_ms = std::min(kSaveMs, left - kWarnMs);
  else st.wake_ms = std::min(kSaveMs, left);
  return st;
}

void Session::finish(long now_ms, const Date& today) {
  if (limit_ <= 0) return;
  t_.set_date(today);
  const long dt = now_ms - last_;
  if (dt > 0) last_ = now_ms;
  t_.add(dt, limit_);
  saver_.tick(t_, now_ms, true);
}

// ---- Watcher ------------------------------------------------------------------------------------------------------------------------

namespace {
long steady_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

Watcher::Watcher(Tracker& t, Saver& s, int limit_min, Hooks h, std::function<Date()> today, long grace_ms)
    : t_(t), saver_(s), limit_(limit_min), hooks_(std::move(h)), today_(std::move(today)), grace_ms_(grace_ms) {}

Watcher::~Watcher() { stop(); }

void Watcher::start() {
  if (th_.joinable()) return;
  th_ = std::thread([this] { loop(); });
}

void Watcher::stop() {
  {
    std::lock_guard<std::mutex> g(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (th_.joinable()) th_.join();
}

void Watcher::loop() {
  Session s(t_, saver_, limit_, steady_ms());
  int attempt = 0;
  std::unique_lock<std::mutex> lk(m_);
  if (grace_ms_ > 0) cv_.wait_for(lk, std::chrono::milliseconds(grace_ms_), [this] { return stop_; });  // the game is still starting
  while (!stop_) {
    lk.unlock();  // the hooks may take a while: never with the lock held (`stop` must get in)
    const Session::Step st = s.advance(steady_ms(), today_());
    if (st.ev.warn && hooks_.warn) hooks_.warn(t_.left_ms(limit_));
    if (t_.exhausted(limit_) && hooks_.expire) hooks_.expire(attempt++);
    lk.lock();
    if (stop_) break;
    cv_.wait_for(lk, std::chrono::milliseconds(std::max(1L, st.wake_ms)), [this] { return stop_; });
  }
  lk.unlock();
  s.finish(steady_ms(), today_());
}

}  // namespace zf::playtime
