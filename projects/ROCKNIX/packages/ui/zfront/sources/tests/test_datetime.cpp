// Date and time by hand: the rules (days per month, leap years, 2024..2099), the row's label and the two commands.
#include "check.hpp"
#include "datetime_set.hpp"
#include "i18n.hpp"

using namespace zf;

int main() {
  using namespace dtset;
  // leap years: /4, but not /100 unless /400
  CHECK(leap_year(2024) && leap_year(2028) && leap_year(2096) && leap_year(2000));
  CHECK(!leap_year(2025) && !leap_year(2026) && !leap_year(2100) && !leap_year(1900));
  const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  for (int m = 1; m <= 12; ++m) {
    CHECK_EQ(days_in_month(2025, m), dm[m - 1]);
    CHECK_EQ(days_in_month(2024, m), m == 2 ? 29 : dm[m - 1]);
  }
  CHECK_EQ(days_in_month(2026, 0), 0);
  CHECK_EQ(days_in_month(2026, 13), 0);

  // limits
  CHECK(valid(2024, 1, 1, 0, 0));
  CHECK(valid(2099, 12, 31, 23, 59));
  CHECK(!valid(2023, 12, 31, 23, 59));
  CHECK(!valid(2100, 1, 1, 0, 0));
  CHECK(!valid(2026, 0, 1, 0, 0) && !valid(2026, 13, 1, 0, 0));
  CHECK(!valid(2026, 1, 0, 0, 0) && !valid(2026, 1, 32, 0, 0));
  CHECK(!valid(2026, 4, 31, 0, 0) && valid(2026, 4, 30, 0, 0));
  CHECK(valid(2028, 2, 29, 12, 0) && !valid(2027, 2, 29, 12, 0) && !valid(2028, 2, 30, 12, 0));
  CHECK(!valid(2026, 1, 1, 24, 0) && !valid(2026, 1, 1, -1, 0));
  CHECK(!valid(2026, 1, 1, 0, 60) && !valid(2026, 1, 1, 0, -1));

  // the command: local time, seconds at 00, quoted
  CHECK_EQ(date_cmd(2026, 11, 5, 9, 7), std::string("date -s '2026-11-05 09:07:00'"));
  CHECK_EQ(date_cmd(2099, 12, 31, 23, 59), std::string("date -s '2099-12-31 23:59:00'"));
  CHECK(date_cmd(2027, 2, 29, 0, 0).empty());
  CHECK_EQ(std::string(hwclock_cmd()), std::string("hwclock --systohc"));

  // the row: a year below 2024 is "Not set", not a made-up date
  CHECK_EQ(label(2026, 11, 15, 14, 5, false), std::string("2026-11-15 14:05"));
  CHECK_EQ(label(2026, 11, 15, 14, 5, true), std::string("2026-11-15 02:05 PM"));
  CHECK_EQ(label(2026, 11, 15, 0, 30, true), std::string("2026-11-15 12:30 AM"));
  CHECK_EQ(label(1970, 1, 1, 0, 0, false), std::string("Not set"));
  CHECK_EQ(label(2023, 12, 31, 23, 59, true), std::string("Not set"));
  return zt::done();
}
