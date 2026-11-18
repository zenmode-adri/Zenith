#include "datetime_set.hpp"

#include <cstdio>

#include "i18n.hpp"
#include "services.hpp"
#include "ui_prefs.hpp"

namespace zf {
namespace dtset {

bool leap_year(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int days_in_month(int y, int m) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  return m == 2 && leap_year(y) ? 29 : kDays[m - 1];
}

bool valid(int y, int mo, int d, int h, int mi) {
  return y >= kMinYear && y <= kMaxYear && mo >= 1 && mo <= 12 && d >= 1 && d <= days_in_month(y, mo) && h >= 0 && h <= 23 && mi >= 0 && mi <= 59;
}

std::string date_cmd(int y, int mo, int d, int h, int mi) {
  if (!valid(y, mo, d, h, mi)) return {};
  char b[32];
  std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:00", y, mo, d, h, mi);
  return "date -s " + shell_quote(b);
}

const char* hwclock_cmd() { return "hwclock --systohc"; }

std::string label(int y, int mo, int d, int h, int mi, bool h12) {
  if (y < kMinYear) return tr("Not set");
  char b[16];
  std::snprintf(b, sizeof b, "%02d:%02d", h % 100, mi % 100);
  std::string t = b;
  if (h12) t = ui_prefs::clock12(t);
  char dd[16];
  std::snprintf(dd, sizeof dd, "%04d-%02d-%02d ", y % 10000, mo % 100, d % 100);
  return dd + t;
}

}  // namespace dtset
}  // namespace zf
