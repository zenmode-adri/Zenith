// `menu.datetime`: set the date and the time by hand (a console without Wi-Fi has no clock). Plain menu rows, so the themes, the
// layout test and the pad repeat already cover it: YEAR / MONTH / DAY / HOUR / MINUTE are choices (Left / Right step, A opens the
// list), SET DATE & TIME applies them: `date -s` and `hwclock --systohc` in a job. Network time is NOT turned off: with Wi-Fi
// systemd-timesyncd corrects the clock again.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "datetime_set.hpp"
#include "i18n.hpp"
#include "menu.hpp"
#include "menu_system.hpp"
#include "registry.hpp"

namespace zf {
namespace {

struct Fields {
  int y = 2026, mo = 1, d = 1, h = 0, mi = 0;
};

std::vector<Option> numbers(int lo, int hi, int width) {
  std::vector<Option> o;
  o.reserve(size_t(hi - lo + 1));
  for (int v = lo; v <= hi; ++v) {
    char b[8];
    std::snprintf(b, sizeof b, "%0*d", width, v);
    o.push_back({std::to_string(v), b});
  }
  return o;
}

int to_int(const std::string& s) { return std::atoi(s.c_str()); }

std::unique_ptr<Screen> make_datetime() {
  auto f = std::make_shared<Fields>();
  time_t t = time(nullptr);
  tm tmv{};
  tzset();
  if (localtime_r(&t, &tmv) && tmv.tm_year + 1900 >= dtset::kMinYear && tmv.tm_year + 1900 <= dtset::kMaxYear) {
    f->y = tmv.tm_year + 1900, f->mo = tmv.tm_mon + 1, f->d = tmv.tm_mday, f->h = tmv.tm_hour, f->mi = tmv.tm_min;
  }  // else: clock not set, start from the defaults above
  MenuSpec s;
  s.title = tr("DATE & TIME");
  s.title_key = N_("DATE & TIME");
  s.build = [f](MenuBuilder& m, Ctx&) {
    auto fix_day = [f] { f->d = std::min(f->d, dtset::days_in_month(f->y, f->mo)); };  // 31 Jan -> Feb: the day follows the month
    auto r = m.refresh;
    m.choice(tr("YEAR"), numbers(dtset::kMinYear, dtset::kMaxYear, 4), [f] { return std::to_string(f->y); }, [f, fix_day, r](const std::string& v) {
      f->y = to_int(v);
      fix_day();
      if (r) r();  // the day list may have changed (29 Feb)
    });
    m.choice(tr("MONTH"), numbers(1, 12, 2), [f] { return std::to_string(f->mo); }, [f, fix_day, r](const std::string& v) {
      f->mo = to_int(v);
      fix_day();
      if (r) r();
    });
    m.choice(tr("DAY"), numbers(1, dtset::days_in_month(f->y, f->mo), 2), [f] { return std::to_string(f->d); },
             [f](const std::string& v) { f->d = to_int(v); });
    m.choice(tr("HOUR"), numbers(0, 23, 2), [f] { return std::to_string(f->h); }, [f](const std::string& v) { f->h = to_int(v); });
    m.choice(tr("MINUTE"), numbers(0, 59, 2), [f] { return std::to_string(f->mi); }, [f](const std::string& v) { f->mi = to_int(v); });
    m.action(tr("SET DATE & TIME"), [f](Ctx& cx) {
      const std::string date = dtset::date_cmd(f->y, f->mo, f->d, f->h, f->mi);
      if (date.empty()) {
        cx.sv->toasts.push(tr("AN ERROR OCCURRED"));
        return Transition::none();
      }
      auto sv = cx.sv;
      auto rc = std::make_shared<int>(-1);
      msys::post(cx, [sv, date, rc] {
        *rc = msys::query(*sv, date).exit_code;
        if (*rc == 0) *rc = msys::query(*sv, dtset::hwclock_cmd()).exit_code;
      }, [&cx, rc] { cx.sv->toasts.push(*rc == 0 ? tr("Date and time set") : tr("AN ERROR OCCURRED"), *rc == 0 ? 3000 : 4000); });
      return Transition::pop();
    }).hint(tr("With Wi-Fi the time is set automatically."));
  };
  return make_menu(std::move(s));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.datetime", [](Ctx&, const std::string&) { return make_datetime(); });

}  // namespace zf
