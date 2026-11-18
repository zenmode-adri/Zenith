// DATE & TIME by hand (SYSTEM > LANGUAGE & TIME > DATE & TIME, screen `menu.datetime`): the pure rules (valid ranges, days per
// month, the two commands that set the clock) and the label of the row. No I/O here, so the tests can check every case.
#pragma once
#include <string>

namespace zf {
namespace dtset {

constexpr int kMinYear = 2024, kMaxYear = 2099;  // below 2024 the clock counts as "not set" (the RTC lost its battery)

bool leap_year(int y);
/// 28..31 (leap years included); 0 for a month outside 1..12.
int days_in_month(int y, int m);
/// Year within `kMinYear..kMaxYear`, real month, a day that month has, hour 0..23, minute 0..59.
bool valid(int y, int mo, int d, int h, int mi);
/// `date -s 'YYYY-MM-DD HH:MM:00'` (local time, the system's zone), argument through `shell_quote`. Empty when not `valid`.
std::string date_cmd(int y, int mo, int d, int h, int mi);
/// What `date -s` leaves in the hardware clock (RTC): `hwclock --systohc`.
const char* hwclock_cmd();
/// "2026-11-15 14:05" (or "2026-11-15 02:05 PM" with `h12`); "Not set" (translated) when the year is below `kMinYear`.
std::string label(int y, int mo, int d, int h, int mi, bool h12);

}  // namespace dtset
}  // namespace zf
