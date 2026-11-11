// Battery: the driver's files, the log, and the estimate of what used the most.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "check.hpp"
#include "power.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace zf {
// battery_screen.cpp: how the screen shows ScreenSaverTime (milliseconds)
long saver_time_minutes(long ms);
std::string saver_time_label(long ms);
}  // namespace zf

namespace {

void write(const std::string& p, const std::string& s) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream(p) << s;
}

// kind, boot, mono, charge uAh, percent, brightness, name
std::string line(char k, long boot, long mono, long uah, int pct, int bright, const std::string& name = {}) {
  return std::string(1, k) + "\t" + std::to_string(boot) + "\t" + std::to_string(mono) + "\t" + std::to_string(uah) + "\t" + std::to_string(pct) + "\t" + std::to_string(bright) + "\t" + name + "\n";
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-test-power-XXXXXX";
  const std::string root = mkdtemp(tmpl);
  setenv("ZF_BATTERY_ROOT", root.c_str(), 1);
  setenv("ZF_BATTERY_LOG", (root + "/battery.log").c_str(), 1);

  CHECK(!power::read_now().have);  // no battery: the screen falls back to the settings

  const std::string b = root + "/sys/class/power_supply/battery/";
  write(b + "capacity", "78\n");
  write(b + "status", "Discharging\n");
  write(b + "voltage_avg", "3840000\n");
  write(b + "current_avg", "-810000\n");
  write(b + "charge_now", "2340000\n");
  write(b + "charge_full", "3000000\n");
  write(root + "/sys/class/backlight/x/brightness", "128\n");
  write(root + "/sys/class/backlight/x/max_brightness", "255\n");
  power::Now n = power::read_now();
  CHECK(n.have);
  CHECK_EQ(n.percent, 78);
  CHECK(!n.charging);
  CHECK(n.amps > 0.80f && n.amps < 0.82f);
  CHECK(n.watts > 3.1f && n.watts < 3.12f);
  CHECK_EQ(n.brightness_pct, 50);
  float left = power::hours_left(n);
  CHECK(left > 2.8f && left < 2.9f);
  write(b + "status", "Charging\n");
  write(b + "current_avg", "500000\n");
  n = power::read_now();
  CHECK(n.charging);
  CHECK(power::hours_left(n) == power::kNoHours);
  write(b + "status", "Discharging\n");
  write(b + "current_avg", "-810000\n");

  {  // nothing yet
    power::Summary s = power::summarize("");
    CHECK(!s.valid);
    s = power::summarize("basura\n\tx\n");
    CHECK(!s.valid);
  }
  {  // a game, menus and a night asleep: the game is what it measured, less the share of the screen
    std::string log;
    log += line('S', 1000, 1000, 2900000, 100, 50);  // full
    log += line('G', 2000, 2000, 2800000, 96, 50, "Super Metroid");
    log += line('E', 5600, 5600, 2200000, 76, 50);  // one hour playing: 600 mAh
    log += line('S', 7400, 7400, 2100000, 73, 50);  // half an hour in menus: 100 mAh
    log += line('S', 43400, 7500, 2050000, 71, 50);  // ten hours asleep: 50 mAh
    power::Summary s = power::summarize(log, 417.0f);
    CHECK(s.valid);
    CHECK_EQ(s.used_pct, 29);
    CHECK(!s.rows.empty());
    CHECK_EQ(int(s.rows[0].kind), int(power::Row::Game));
    CHECK_EQ(s.rows[0].name, std::string("Super Metroid"));
    CHECK_EQ(s.rows[0].secs, 3600L);
    float total = 0, screen = 0, asleep = 0;
    long asleep_s = 0;
    for (const power::Row& r : s.rows) {
      total += r.pct;
      if (r.kind == power::Row::Screen) screen = r.pct;
      if (r.kind == power::Row::Asleep) asleep = r.pct, asleep_s = r.secs;
    }
    CHECK(total > 99.9f && total < 100.1f);
    CHECK(screen > 5.0f && screen < 40.0f);
    CHECK(asleep > 3.0f && asleep < 9.0f);
    CHECK(asleep_s > 30000L);
    CHECK(s.curve.size() >= 5);
    CHECK(s.curve.front().first > s.curve.back().first);  // oldest first
    CHECK_EQ(s.curve.back().second, 71);
  }
  {  // only what came after the last full charge counts; time on the charger uses nothing; a reboot is not a measure
    std::string log;
    log += line('G', 100, 100, 2500000, 80, 50, "Old game");
    log += line('E', 200, 200, 2000000, 60, 50);
    log += line('S', 300, 300, 2900000, 100, 50);
    log += line('S', 400, 400, 2800000, 95, 50);
    log += line('S', 500, 500, 2850000, 97, 50);  // charging a little
    log += line('S', 10, 10, 2700000, 90, 50);    // rebooted
    log += line('S', 700, 700, 2600000, 85, 50);
    power::Summary s = power::summarize(log, 417.0f);
    for (const power::Row& r : s.rows) CHECK(r.name != "Old game");
  }
  {  // a line of the log has no way to break the file; it trims itself
    power::log_event('G', std::string("A\tname\nwith breaks"));
    std::string text;
    std::ifstream f(root + "/battery.log");
    std::getline(f, text);
    CHECK(text.find("A name with breaks") != std::string::npos);
    std::string more;
    std::ofstream(root + "/battery.log") << std::string(100 * 1024, 'x') + "\n" + std::string(100 * 1024, 'y') + "\n";
    power::log_event('S');
    CHECK(fs::file_size(root + "/battery.log") < 150 * 1024);
    // the trim goes through a temp file (fix4 z5 #16): the log stays whole and nothing is left beside it
    std::string back;
    std::ifstream in(root + "/battery.log");
    std::getline(in, back, '\0');
    CHECK(!back.empty() && back.back() == '\n');
    CHECK(back.size() < 150 * 1024);  // the older half went
    for (const auto& e : fs::directory_iterator(root)) CHECK(e.path().filename().string().find("zfront-tmp") == std::string::npos);
  }
  {  // the first line makes its folder
    const std::string deep = root + "/no/such/folder/battery.log";
    setenv("ZF_BATTERY_LOG", deep.c_str(), 1);
    power::log_event('S');
    CHECK(fs::exists(deep));
  }
  {  // ScreenSaverTime under a minute is not "Never" (ES's slider allows 30 s)
    CHECK_EQ(saver_time_label(0), std::string("Never"));
    CHECK_EQ(saver_time_label(-5), std::string("Never"));
    CHECK_EQ(saver_time_label(30000), std::string("30 s"));
    CHECK_EQ(saver_time_label(59999), std::string("60 s"));
    CHECK_EQ(saver_time_label(60000), std::string("1 min"));
    CHECK_EQ(saver_time_label(300000), std::string("5 min"));
    CHECK_EQ(saver_time_label(1800000), std::string("30 min"));
    CHECK_EQ(saver_time_minutes(0), 0L);
    CHECK_EQ(saver_time_minutes(30000), 1L);  // stepping from 30 s goes on from the 1 minute slot, not from "Never"
    CHECK_EQ(saver_time_minutes(300000), 5L);
  }
  std::error_code ec;
  fs::remove_all(root, ec);
  return zt::done();
}
