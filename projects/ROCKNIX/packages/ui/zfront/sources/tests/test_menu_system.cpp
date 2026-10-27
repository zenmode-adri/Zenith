// menu.system, menu.system.info, menu.system.reset: what they show on an Ultra-like console (fake root), the commands they
// record in dry-run mode, the values written to system.cfg, availability gating and the confirmation of destructive actions.
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "menu_groups.hpp"
#include "menu_sound.hpp"
#include "menu_system.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

using namespace zf;
using namespace zf::msys;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string g_dir;  // temp dir of this run

void mkdirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i)
    if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
}

void put_file(const std::string& rel, const std::string& content = {}) {
  std::string p = g_dir + rel;
  mkdirs(p.substr(0, p.rfind('/')));
  std::ofstream(p, std::ios::binary) << content;
}

std::string slurp(const std::string& path) {
  std::string t;
  read_file(path, t);
  return t;
}

// Ultra-like console under g_dir/root. Returns the Ctx ready (dry-run, system.cfg in g_dir).
void make_console(Ctx& cx, bool full = true) {
  static int n = 0;
  g_dir = "/tmp/zf_test_system_" + std::to_string(getpid()) + "_" + std::to_string(++n);
  mkdirs(g_dir + "/root");
  Probe& p = probe();
  p = Probe();
  p.root = g_dir + "/root";
  p.tmp = g_dir + "/tmp";
  p.use_env = true;
  mkdirs(p.tmp);
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = true;
  cx.sv->cmd.log.clear();
  SystemConf::lock_path() = g_dir + "/lock";
  put_file("/system.cfg", "system.language=en_US\n");
  cx.conf = SystemConf::load(g_dir + "/system.cfg");
  if (!full) return;
  for (const char* b : {"rocknix-info", "timeinfo", "suspendmode", "paneladj", "turbomode", "gpudriver", "ledcontrol", "analog_sticks_ledcontrol_single",
                        "rocknix-keyboard", "setrootpass", "factoryreset", "backuptool", "run", "rocknix-update", "mangohud_set"})
    put_file(std::string("/root/usr/bin/") + b);
  put_file("/root/sys/class/backlight/backlight/brightness", "128\n");
  put_file("/root/sys/class/backlight/backlight/max_brightness", "255\n");
  put_file("/root/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors", "conservative ondemand userspace powersave performance schedutil \n");
  put_file("/root/sys/power/state", "freeze mem disk\n");
  put_file("/root/etc/profile.d/099-freqfunctions");
  p.env["DEVICE_TURBO_MODE"] = "true";
  p.env["DEVICE_LED_CONTROL"] = "true";
  p.env["DEVICE_ANALOG_STICKS_LED_SINGLE"] = "true";
  p.env["DEVICE_MANGOHUD_SUPPORT"] = "true";
  p.env["OS_VERSION"] = "20261024";
  p.env["OS_BUILD"] = "rocknix";
  p.canned["/usr/bin/timeinfo timezones"] = {0, "Africa/Abidjan,\nEurope/Madrid,\nEurope/Paris,\n"};
  p.canned["/usr/bin/timeinfo current_timezone"] = {0, "Europe/Paris\n"};
  p.canned["/usr/bin/rocknix-keyboard list-layouts"] = {0, "es Spanish\nfr French\nus English (US)\n"};
  p.canned["/usr/bin/rocknix-keyboard list-variants 'us'"] = {0, "intl English (US, intl., with dead keys)\n"};
  p.canned["/usr/bin/sh -lc \"/usr/bin/ledcontrol list\""] = {0, "off\nred\ngreen\n"};
  p.canned["/usr/bin/sh -lc \"/usr/bin/analog_sticks_ledcontrol_single list | tr '\\n' ' '\""] = {0, "off rainbow chroma \n"};
  p.canned["/usr/bin/sh -lc \". /etc/profile.d/099-freqfunctions; get_available_gpu_governors\""] = {0, "default\nperformance\nondemand\n"};
  p.canned["/usr/bin/gpudriver"] = {0, "libmali\n"};
  p.canned["/usr/bin/gpudriver --options"] = {0, "libmali panfrost\n"};
}

void settle(Ctx& cx) {
  for (int i = 0; i < 1000; ++i) {
    cx.sv->jobs.drain();
    if (cx.sv->jobs.idle()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  cx.sv->jobs.drain();
}

Menu* open(std::unique_ptr<Screen>& hold, Ctx& cx, const char* name) {
  hold = Registry::instance().make(name, cx, "");
  CHECK(hold != nullptr);
  Menu* m = static_cast<Menu*>(hold.get());
  m->tick(0.0f, cx);  // builds and starts on_open
  settle(cx);
  m->tick(0.0f, cx);  // applies what the jobs delivered
  return m;
}

int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}

bool has(const Menu& m, const std::string& label) { return find(m, label) >= 0; }

// moves the cursor onto `label`
bool go(Menu& m, Ctx& cx, const std::string& label) {
  int want = find(m, label);
  if (want < 0) return false;
  for (size_t k = 0; k <= m.size() + 1 && m.cursor() != want; ++k) m.handle(Action::Down, cx);
  return m.cursor() == want;
}

bool logged(Ctx& cx, const std::string& cmd) {
  for (const std::string& l : cx.sv->cmd.log)
    if (l == cmd) return true;
  return false;
}

bool logged_part(Ctx& cx, const std::string& part) {
  for (const std::string& l : cx.sv->cmd.log)
    if (l.find(part) != std::string::npos) return true;
  return false;
}

// A choice entry: A opens the popup, `steps` x Down, A picks.
void pick(Menu& m, Ctx& cx, const std::string& label, int steps) {
  CHECK(go(m, cx, label));
  Transition t = m.handle(Action::A, cx);
  CHECK(t.kind == Transition::Push && t.screen);
  if (!t.screen) return;
  for (int i = 0; i < steps; ++i) t.screen->handle(Action::Down, cx);
  t.screen->handle(Action::A, cx);
  m.tick(0.0f, cx);
}

}  // namespace

int main() {
  // ---- long warnings: nothing is lost, the question survives the smallest screen
  {
    const std::string w = "WARNING ONE\n\nWARNING TWO\n\nRESET?";
    CHECK_EQ(fit_message(w), std::string("WARNING ONE\nWARNING TWO\nRESET?"));  // short: same order, no blank lines
    const std::string l = std::string(150, 'x') + "\n\nRESET?";
    CHECK_EQ(fit_message(l), std::string("RESET?\n") + std::string(150, 'x'));  // long: the question first
    CHECK_EQ(fit_message("ONLY"), std::string("ONLY"));
  }

  // ---- registration and gating: a console with nothing special shows only the always-present rows
  {
    Ctx cx;
    make_console(cx, false);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    CHECK(Registry::instance().has("menu.system"));
    CHECK(Registry::instance().has("menu.system.info"));
    CHECK(Registry::instance().has("menu.system.reset"));
    CHECK(has(*m, "INFORMATION"));
    CHECK(has(*m, "SYSTEM MANAGEMENT AND RESET"));
    CHECK(!has(*m, "ENABLE CPU OVERCLOCK"));
    CHECK(!has(*m, "GPU DRIVER"));
    CHECK(!has(*m, "SCREEN BRIGHTNESS"));
    CHECK(!has(*m, "TIMEZONE"));
    CHECK(!has(*m, "KEYBOARD LAYOUT"));
    CHECK(!has(*m, "DEVICE SUSPEND MODE"));
    CHECK(!has(*m, "LED COLOR"));
    CHECK(!has(*m, "ROOT PASSWORD"));
    CHECK(!has(*m, "PANEL ADJUSTMENT"));
    // hidden for good (ES-only / no effect in zfront)
    for (const char* l : {"POWER SAVING MODE", "USER INTERFACE MODE", "SHOW CLOCK IN 12-HOUR FORMAT", "SCREEN READER (TEXT TO SPEECH)", "MULTIMEDIA KEYS",
                          "ENABLE ROCKNIX SCREENSHOT", "AUDIO OUTPUT", "DISPLAY MODE"})
      CHECK(!has(*m, l));
  }

  // ---- the Ultra-like console shows its rows
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    for (const char* l : {"INFORMATION", "LANGUAGE (REGION)", "KEYBOARD LAYOUT", "KEYBOARD VARIANT", "TIMEZONE", "ENABLE MANGOHUD OVERLAY", "SCREEN BRIGHTNESS",
                          "GAMMA", "CONTRAST", "SATURATION", "HUE", "LED COLOR", "JOYSTICK LED", "AUTODETECT GAMES CARD", "EJECT MICROSD CARD",
                          "DEFAULT SCALING GOVERNOR", "DEFAULT GPU SCALING GOVERNOR", "ENABLE CPU OVERCLOCK", "DEVICE SUSPEND MODE", "SYSTEM MANAGEMENT AND RESET"})
      CHECK(has(*m, l));
    CHECK(!has(*m, "ENABLE TOUCHSCREEN KEYBOARD"));  // DEVICE_HAS_TOUCHSCREEN not set
    CHECK(!has(*m, "COOLING PROFILE"));              // no fan
    CHECK(!has(*m, "THIS DEVICE IS"));
    CHECK(!has(*m, "ENABLE GPU OVERCLOCK"));
    CHECK(!has(*m, "LED BRIGHTNESS"));
    CHECK(!has(*m, "GPU DRIVER") == false);  // gpudriver is in the image
    // shown values
    CHECK_EQ(m->entry(size_t(find(*m, "SCREEN BRIGHTNESS"))).value_text, std::string("50%"));  // 128 / 255
    CHECK_EQ(m->entry(size_t(find(*m, "TIMEZONE"))).value_text, std::string("Europe/Paris"));  // current_timezone, nothing configured
    CHECK_EQ(m->entry(size_t(find(*m, "DEFAULT SCALING GOVERNOR"))).value_text, std::string("default"));
    CHECK_EQ(m->entry(size_t(find(*m, "GPU DRIVER"))).value_text, std::string("libmali"));
    CHECK_EQ(m->entry(size_t(find(*m, "LANGUAGE (REGION)"))).value_text, std::string("ENGLISH (US)"));
    // group titles exist only with something below them
    int last_header = -1;
    for (size_t i = 0; i < m->size(); ++i) {
      if (m->entry(i).kind == EntryKind::Header) {
        CHECK(int(i) != last_header + 1 || last_header < 0);  // two headers in a row = an empty group
        last_header = int(i);
      } else {
        last_header = -2;
      }
    }
    CHECK(m->entry(m->size() - 1).kind != EntryKind::Header);
  }

  // ---- toggles: commands and keys exactly as ES (turbo), stored as 1/0
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    CHECK(go(*m, cx, "ENABLE CPU OVERCLOCK"));
    m->handle(Action::A, cx);
    CHECK(logged(cx, "/usr/bin/turbomode enable"));
    CHECK_EQ(cx.conf.get_or("enable.turbo-mode", ""), std::string("1"));
    CHECK_EQ(slurp(g_dir + "/system.cfg").find("enable.turbo-mode=1") != std::string::npos, true);
    m->handle(Action::A, cx);
    CHECK(logged(cx, "/usr/bin/turbomode disable"));
    CHECK_EQ(cx.conf.get_or("enable.turbo-mode", ""), std::string("0"));
    CHECK(go(*m, cx, "ENABLE MANGOHUD OVERLAY"));
    m->handle(Action::A, cx);
    CHECK(logged(cx, "/usr/bin/mangohud_set enable"));
    CHECK_EQ(cx.conf.get_or("rocknix.mangohud.enabled", ""), std::string("1"));
    CHECK(go(*m, cx, "AUTODETECT GAMES CARD"));
    m->handle(Action::A, cx);
    CHECK_EQ(cx.conf.get_or("system.automount", ""), std::string("1"));
    CHECK(logged(cx, "/usr/bin/systemctl restart rocknix-automount"));
  }

  // ---- brightness: sysfs write like the OS script (floor(pct*max/100)), key system.brightness after the debounce
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    CHECK(go(*m, cx, "SCREEN BRIGHTNESS"));
    m->handle(Action::Right, cx);  // 51 %
    CHECK(logged(cx, "sysfs: /sys/class/backlight/backlight/brightness <- 130"));  // 51 * 255 / 100
    CHECK_EQ(cx.conf.get_or("system.brightness", ""), std::string(""));            // not yet: the debounce
    CHECK(m->wake_in().has_value());
    m->tick(0.6f, cx);
    CHECK_EQ(cx.conf.get_or("system.brightness", ""), std::string("51"));
    // never below 5 %
    for (int i = 0; i < 60; ++i) m->handle(Action::Left, cx);
    CHECK_EQ(m->entry(size_t(find(*m, "SCREEN BRIGHTNESS"))).value_text, std::string("5%"));
    CHECK(logged(cx, "sysfs: /sys/class/backlight/backlight/brightness <- 12"));
    // the real file was never touched in dry-run
    CHECK_EQ(slurp(g_dir + "/root/sys/class/backlight/backlight/brightness"), std::string("128\n"));
  }

  // ---- panel adjustment: paneladj in the background, then the key display.*
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    CHECK(go(*m, cx, "CONTRAST"));
    m->handle(Action::Right, cx);
    CHECK(logged(cx, "/usr/bin/paneladj contrast 55 nosave >/dev/null 2>&1"));
    m->handle(Action::B, cx);  // leaving flushes the pending commit
    CHECK_EQ(cx.conf.get_or("display.contrast", ""), std::string("55"));
    // the GAMMA slider is ES's display.brightness key (paneladj's property "brightness"), NOT the backlight key
    CHECK(go(*m, cx, "GAMMA"));
    m->handle(Action::Left, cx);
    CHECK(logged(cx, "/usr/bin/paneladj brightness 45 nosave >/dev/null 2>&1"));
  }

  // ---- governors: ES's commands; "default" removes the key (ES's delete-on-default rule)
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    pick(*m, cx, "DEFAULT SCALING GOVERNOR", 3);  // default, conservative, ondemand, userspace
    CHECK_EQ(cx.conf.get_or("system.cpugovernor", ""), std::string("userspace"));
    CHECK(logged(cx, "/usr/bin/sh -lc '. /etc/profile.d/099-freqfunctions; userspace' >/dev/null 2>&1"));
    CHECK(slurp(g_dir + "/system.cfg").find("system.cpugovernor=userspace") != std::string::npos);
    pick(*m, cx, "DEFAULT SCALING GOVERNOR", 0);  // the popup starts on the current value: A again re-picks it
    cx.sv->cmd.log.clear();
    {
      Transition t = (go(*m, cx, "DEFAULT SCALING GOVERNOR"), m->handle(Action::A, cx));
      CHECK(t.screen != nullptr);
      for (int i = 0; i < 3; ++i) t.screen->handle(Action::Up, cx);  // userspace -> default (3 rows up)
      t.screen->handle(Action::A, cx);
    }
    CHECK(slurp(g_dir + "/system.cfg").find("system.cpugovernor") == std::string::npos);
    CHECK(logged(cx, "/usr/bin/sh -lc '. /etc/profile.d/099-freqfunctions; default' >/dev/null 2>&1"));
    // GPU governors come from the script (job) and are applied with gpu_performance_level
    pick(*m, cx, "DEFAULT GPU SCALING GOVERNOR", 1);  // default -> performance
    CHECK_EQ(cx.conf.get_or("system.gpuperf", ""), std::string("performance"));
    CHECK(logged(cx, "/usr/bin/sh -lc '. /etc/profile.d/099-freqfunctions; gpu_performance_level performance' >/dev/null 2>&1"));
  }

  // ---- time zone: list from `timeinfo timezones`, ES's ln -sf, key system.timezone
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    pick(*m, cx, "TIMEZONE", 1);  // current Europe/Paris -> wraps? the popup starts on Paris (index 2): Down goes to the first
    // whatever the wrap rule, the choice must be one of the listed zones and the command must match it
    std::string tz = cx.conf.get_or("system.timezone", "");
    CHECK(tz == "Africa/Abidjan" || tz == "Europe/Madrid" || tz == "Europe/Paris");
    CHECK(tz != "Europe/Paris");
    CHECK(logged(cx, "ln -sf '/usr/share/zoneinfo/" + tz + "' $(readlink /etc/localtime)"));
  }

  // ---- keyboard: the layout resets the variant to "none"; a failing script changes nothing
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    CHECK_EQ(m->entry(size_t(find(*m, "KEYBOARD LAYOUT"))).value_text, std::string("English (US)"));
    pick(*m, cx, "KEYBOARD LAYOUT", 1);  // popup on "us" (idx 2 of es, fr, us) -> Down wraps or stays; accept either neighbour
    settle(cx);
    m->tick(0.0f, cx);
    std::string lay = cx.conf.get_or("system.kblayout", "");
    CHECK(lay == "es" || lay == "fr");
    CHECK(logged(cx, "/usr/bin/rocknix-keyboard set '" + lay + "' 'none'"));
    CHECK_EQ(cx.conf.get_or("system.kbvariant", ""), std::string("none"));
    CHECK(has(*m, "Reboot now"));  // reboot hint row appears
  }
  {
    Ctx cx;
    make_console(cx);
    probe().canned["/usr/bin/rocknix-keyboard set 'es' 'none'"] = {1, "boom"};
    probe().canned["/usr/bin/rocknix-keyboard set 'fr' 'none'"] = {1, "boom"};
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    pick(*m, cx, "KEYBOARD LAYOUT", 1);
    settle(cx);
    m->tick(0.0f, cx);
    CHECK_EQ(cx.conf.get_or("system.kblayout", ""), std::string(""));
    CHECK_EQ(m->entry(size_t(find(*m, "KEYBOARD LAYOUT"))).value_text, std::string("English (US)"));
    CHECK(!has(*m, "Reboot now"));
    CHECK(!cx.sv->toasts.items().empty());
  }

  // ---- language: key + live switch of the catalog
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    // only English is offered on a PC without catalogs
    Transition t = (go(*m, cx, "LANGUAGE (REGION)"), m->handle(Action::A, cx));
    CHECK(t.screen != nullptr);
  }

  // ---- root password: quoted, empty refused
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    CHECK(has(*m, "ROOT PASSWORD"));
    CHECK_EQ(m->entry(size_t(find(*m, "ROOT PASSWORD"))).value_text, std::string("linux"));  // ES's default
    Entry& e = const_cast<Entry&>(m->entry(size_t(find(*m, "ROOT PASSWORD"))));
    e.set_s("it's a 'test'; rm -rf /");
    CHECK(logged(cx, "setrootpass 'it'\\''s a '\\''test'\\''; rm -rf /'"));
    CHECK_EQ(cx.conf.get_or("root.password", ""), std::string("it's a 'test'; rm -rf /"));
    cx.sv->cmd.log.clear();
    e.set_s("");
    CHECK(cx.sv->cmd.log.empty());
  }

  // ---- suspend mode, device-switch, gpu driver: command + reboot hint
  {
    Ctx cx;
    make_console(cx);
    put_file("/root/usr/bin/device-switch");
    put_file("/root/sys/firmware/devicetree/base/rocknix,device_switch/name", "x");
    put_file("/root/sys/firmware/devicetree/base/rocknix,device_switch/this", std::string("R36S\0", 5));
    put_file("/root/sys/firmware/devicetree/base/rocknix,device_switch/R36S", "x");
    put_file("/root/sys/firmware/devicetree/base/rocknix,device_switch/R36Ultra", "x");
    probe().env["DEVICE_DTB_SWITCH"] = "true";
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    CHECK_EQ(m->entry(size_t(find(*m, "THIS DEVICE IS"))).value_text, std::string("R36S"));
    pick(*m, cx, "DEVICE SUSPEND MODE", 2);  // default, freeze, mem (disk is not offered)
    CHECK_EQ(cx.conf.get_or("system.suspendmode", ""), std::string("mem"));
    CHECK(logged(cx, "/usr/bin/suspendmode 'mem'"));
    pick(*m, cx, "THIS DEVICE IS", 1);
    CHECK(logged(cx, "/usr/bin/device-switch 'R36Ultra'"));
    CHECK(has(*m, "Reboot now"));
    int hits = 0;
    probe().reboot = [&hits](Ctx&) { ++hits; };
    CHECK(go(*m, cx, "Reboot now"));
    Transition t = m->handle(Action::A, cx);
    CHECK(t.kind == Transition::Push && t.screen);  // asks first
    CHECK_EQ(hits, 0);
    t.screen->handle(Action::Left, cx);  // YES
    t.screen->handle(Action::A, cx);
    CHECK_EQ(hits, 1);
  }

  // ---- eject: unmounts, then says so
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    CHECK(go(*m, cx, "EJECT MICROSD CARD"));
    Transition t = m->handle(Action::A, cx);
    CHECK(logged(cx, "/usr/bin/umount -f /storage/roms"));
    CHECK(t.kind == Transition::Push && t.screen);
  }

  // ---- LED: joystick mode goes through ES's coalescing snippet, with the mode sanitised
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system");
    pick(*m, cx, "JOYSTICK LED", 1);  // off -> rainbow
    CHECK_EQ(cx.conf.get_or("analogsticks.led.color", ""), std::string("rainbow"));
    CHECK(logged_part(cx, "echo rainbow > /run/joyled.want;"));
    pick(*m, cx, "LED COLOR", 1);  // default is not in the list: the popup starts on the first
    CHECK(!cx.conf.get_or("led.color", "").empty());
    CHECK(logged_part(cx, "/usr/bin/ledcontrol "));
  }

  // ---- information: rocknix-info --full, groups and rows, uptime added
  {
    Ctx cx;
    make_console(cx);
    probe().canned["rocknix-info --full"] = {0,
                                             "SYSTEM INFORMATION:\nDEVICE: Game Console R36Ultra\nOPERATING SYSTEM: Zenith\nVERSION: 20261024 (aurknix)\n"
                                             "NETWORK INFORMATION:\nHOST NAME: zenith\nIP ADDRESS: 192.168.1.42\nBATTERY INFORMATION:\nBATTERY REMAINING: 87%\n"
                                             "CPU INFORMATION:\nCPU TEMPERATURE: 41°\nRAM INFORMATION:\nRAM AVAILABLE: 611 MB\nRAM CAPACITY: 983 MB\n"};
    put_file("/root/proc/uptime", "93784.12 12345.67\n");
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system.info");
    CHECK(logged(cx, "rocknix-info --full"));
    CHECK_EQ(m->title(), std::string("INFORMATION"));
    int ip = find(*m, "IP ADDRESS");
    CHECK(ip >= 0);
    CHECK_EQ(m->entry(size_t(ip)).value_text, std::string("192.168.1.42"));
    CHECK(m->entry(size_t(ip)).kind == EntryKind::Info);
    CHECK(has(*m, "NETWORK INFORMATION") && m->entry(size_t(find(*m, "NETWORK INFORMATION"))).kind == EntryKind::Header);
    CHECK(has(*m, "RAM CAPACITY"));
    int up = find(*m, "UPTIME");
    CHECK(up >= 0);
    CHECK_EQ(m->entry(size_t(up)).value_text, std::string("1d 2:03"));
    CHECK(up < find(*m, "NETWORK INFORMATION"));  // closes the first group
    // nothing is selectable on this screen
    CHECK_EQ(m->cursor(), -1);
  }
  {
    // before the answer arrives: one "Loading..." row, no crash on an empty answer
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h = Registry::instance().make("menu.system.info", cx, "");
    Menu* m = static_cast<Menu*>(h.get());
    m->tick(0.0f, cx);
    CHECK_EQ(m->size(), size_t(1));
    settle(cx);
    m->tick(0.0f, cx);
    CHECK(has(*m, "No information available") || has(*m, "UPTIME"));
  }

  // ---- reset: nothing runs before YES; YES runs ES's exact command through `run`
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.system.reset");
    struct Case {
      const char* label;
      const char* cmd;
    };
    const Case cases[] = {
        {"BACKUP CONFIGURATIONS", "/usr/bin/run \"/usr/bin/backuptool backup\""},
        {"RESTORE FROM BACKUP", "/usr/bin/run \"/usr/bin/backuptool restore\""},
        {"RESET RETROARCH CONFIG TO DEFAULT", "/usr/bin/run \"/usr/bin/factoryreset retroarch\""},
        {"RESET OVERLAYS (CORES, CHEATS, JOYPADS, ETC)", "/usr/bin/run \"/usr/bin/factoryreset overlays\""},
        {"FULLY RESET RETROARCH", "/usr/bin/run \"/usr/bin/factoryreset retroarch-full && /usr/bin/factoryreset overlays\""},
        {"RESET MEDNAFEN CONFIG TO DEFAULT", "/usr/bin/run \"/usr/bin/factoryreset mednafen\""},
        {"RESET STANDALONE EMULATOR CONFIGS TO DEFAULT", "/usr/bin/run \"/usr/bin/factoryreset standalone\""},
        {"FULLY RESET PORTMASTER", "/usr/bin/run \"/usr/bin/factoryreset portmaster\""},
        {"AUDIO RESET", "/usr/bin/run \"/usr/bin/factoryreset audio\""},
        {"FACTORY RESET", "/usr/bin/run \"/usr/bin/factoryreset ALL\""},
    };
    CHECK(!has(*m, "RESET BATTERY FUEL GAUGE"));  // no RK817 flag
    CHECK(!has(*m, "CLEAN GAMELISTS & REMOVE UNUSED MEDIA"));
    for (const Case& c : cases) {
      cx.sv->cmd.log.clear();
      CHECK(go(*m, cx, c.label));
      Transition t = m->handle(Action::A, cx);
      CHECK(t.kind == Transition::Push && t.screen);  // the question
      CHECK(cx.sv->cmd.log.empty());
      if (!t.screen) continue;
      // NO is focused first: A on it cancels
      Transition no = t.screen->handle(Action::A, cx);
      CHECK(no.kind == Transition::Pop);
      CHECK(cx.sv->cmd.log.empty());
      // YES
      Transition t2 = m->handle(Action::A, cx);
      t2.screen->handle(Action::Left, cx);
      t2.screen->handle(Action::A, cx);
      CHECK_EQ(cx.sv->cmd.log.size(), size_t(1));
      CHECK(logged(cx, c.cmd));
    }
    // the fuel gauge only with the RK817 flag
    probe().env["DEVICE_RK817_FUEL_GAUGE"] = "true";
    std::unique_ptr<Screen> h2;
    Menu* m2 = open(h2, cx, "menu.system.reset");
    CHECK(has(*m2, "RESET BATTERY FUEL GAUGE"));
    // ... or the file the platform quirk writes into /storage/.config/profile.d
    probe().env.erase("DEVICE_RK817_FUEL_GAUGE");
    put_file("/root/storage/.config/profile.d/015-rk817_config", "DEVICE_RK817_FUEL_GAUGE=\"true\"\n");
    std::unique_ptr<Screen> h3;
    CHECK(has(*open(h3, cx, "menu.system.reset"), "RESET BATTERY FUEL GAUGE"));
  }

  // ---- group screens: the same rows, grouped by what the person wants to do
  {
    Ctx cx;
    make_console(cx);
    zf::sound::env().fake = true;  // volume + audio tools "installed"
    for (const char* n : {"menu.g.screen", "menu.g.panel", "menu.g.power", "menu.g.power.adv", "menu.g.library", "menu.g.system", "menu.g.lang", "menu.g.system.adv"})
      CHECK(Registry::instance().has(n));
    CHECK(!Registry::instance().has("menu.g.interface"));  // clock and battery went to SCREEN & SOUND, the buttons to CONTROLLER
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.g.screen");
    for (const char* l : {"SCREEN BRIGHTNESS", "PANEL ADJUSTMENT", "SHOW OVERLAY WHEN VOLUME CHANGES", "LED COLOR", "JOYSTICK LED", "SHOW CLOCK"}) CHECK(has(*m, l));
    // the volume is the hardware buttons' job; the panel sliders are one level down
    for (const char* l : {"SYSTEM VOLUME", "GAMMA", "HUE", "DEFAULT SCALING GOVERNOR", "ENABLE CPU OVERCLOCK", "TIMEZONE", "ENABLE AUDIBLE BATTERY WARNING", "START SCREENSAVER AFTER",
                          "SWITCH CONFIRM & CANCEL BUTTONS"})
      CHECK(!has(*m, l));
    CHECK(find(*m, "SCREEN BRIGHTNESS") < find(*m, "SHOW OVERLAY WHEN VOLUME CHANGES"));  // (the audio output lists need the real scripts)
    CHECK(find(*m, "SHOW OVERLAY WHEN VOLUME CHANGES") < find(*m, "LED COLOR"));
    CHECK(find(*m, "LED COLOR") < find(*m, "SHOW CLOCK"));
    std::unique_ptr<Screen> hp;
    m = open(hp, cx, "menu.g.panel");
    for (const char* l : {"GAMMA", "CONTRAST", "SATURATION", "HUE"}) CHECK(has(*m, l));
    CHECK(!has(*m, "SCREEN BRIGHTNESS"));

    std::unique_ptr<Screen> h2;
    m = open(h2, cx, "menu.g.power");
    for (const char* l : {"ENABLE AUDIBLE BATTERY WARNING", "BATTERY WARNING THRESHOLD", "DEFAULT SCALING GOVERNOR", "DEFAULT GPU SCALING GOVERNOR", "DEVICE SUSPEND MODE",
                          "START SCREENSAVER AFTER", "SCREENSAVER TYPE", "ADVANCED"})
      CHECK(has(*m, l));
    for (const char* l : {"ENABLE CPU OVERCLOCK", "SYSTEM VOLUME", "SCREEN BRIGHTNESS"}) CHECK(!has(*m, l));
    // what the console does with its power: governors, then idle (suspend, screensaver), then the warning
    CHECK(find(*m, "DEFAULT SCALING GOVERNOR") < find(*m, "DEVICE SUSPEND MODE"));
    CHECK(find(*m, "DEVICE SUSPEND MODE") < find(*m, "START SCREENSAVER AFTER"));
    CHECK(find(*m, "START SCREENSAVER AFTER") < find(*m, "ENABLE AUDIBLE BATTERY WARNING"));
    std::unique_ptr<Screen> h3;
    m = open(h3, cx, "menu.g.power.adv");
    CHECK(has(*m, "ENABLE CPU OVERCLOCK"));
    CHECK(has(*m, "ENABLE MANGOHUD OVERLAY"));
    CHECK(!has(*m, "DEFAULT SCALING GOVERNOR"));

    std::unique_ptr<Screen> h4;
    m = open(h4, cx, "menu.g.library");
    for (const char* l : {"GAME COLLECTION SETTINGS", "SCRAPER", "AUTODETECT GAMES CARD", "EJECT MICROSD CARD"}) CHECK(has(*m, l));
    CHECK(!has(*m, "TIMEZONE"));

    std::unique_ptr<Screen> h6;
    m = open(h6, cx, "menu.g.system");
    for (const char* l : {"INFORMATION", "LANGUAGE & TIME", "UPDATES & DOWNLOADS", "SYSTEM MANAGEMENT AND RESET", "ADVANCED"}) CHECK(has(*m, l));
    for (const char* l : {"LANGUAGE (REGION)", "KEYBOARD LAYOUT", "TIMEZONE", "ENABLE CPU OVERCLOCK", "SCREEN BRIGHTNESS", "ROOT PASSWORD"}) CHECK(!has(*m, l));
    CHECK(find(*m, "INFORMATION") < find(*m, "LANGUAGE & TIME"));
    CHECK(find(*m, "LANGUAGE & TIME") < find(*m, "UPDATES & DOWNLOADS"));
    std::unique_ptr<Screen> hl;
    m = open(hl, cx, "menu.g.lang");
    for (const char* l : {"LANGUAGE (REGION)", "KEYBOARD LAYOUT", "TIMEZONE"}) CHECK(has(*m, l));
    std::unique_ptr<Screen> h7;
    m = open(h7, cx, "menu.g.system.adv");
    CHECK(has(*m, "ROOT PASSWORD"));
    CHECK(has(*m, "GPU DRIVER"));

    // the main menu: only the groups (the volume and the brightness have hardware buttons)
    std::unique_ptr<Screen> h8;
    m = open(h8, cx, "menu.main");
    for (const char* l : {"GAMES", "SCREEN & SOUND", "BATTERY & PERFORMANCE", "LIBRARY", "SYSTEM", "QUIT"}) CHECK(has(*m, l));
    for (const char* l : {"SYSTEM VOLUME", "SCREEN BRIGHTNESS"}) CHECK(!has(*m, l));
    CHECK_EQ(find(*m, "GAMES"), 0);  // no quick settings above the groups

    // the section screens keep every row (ES order), so nothing that opens them changes
    std::unique_ptr<Screen> h9;
    m = open(h9, cx, "menu.system");
    for (const char* l : {"SCREEN BRIGHTNESS", "ENABLE CPU OVERCLOCK", "DEVICE SUSPEND MODE", "LANGUAGE (REGION)", "ROOT PASSWORD", "SYSTEM MANAGEMENT AND RESET"}) CHECK(has(*m, l));

    // a console without overclock: no ADVANCED row in BATTERY & PERFORMANCE
    probe().env.erase("DEVICE_TURBO_MODE");
    probe().env.erase("DEVICE_MANGOHUD_SUPPORT");
    std::unique_ptr<Screen> h10;
    m = open(h10, cx, "menu.g.power");
    CHECK(!has(*m, "ADVANCED"));
    zf::sound::env().fake = false;
  }

  // ---- fixture loader (review harness)
  {
    Ctx cx;
    make_console(cx, false);
    put_file("/fx/fixture.txt", "# demo\nroot /r\ntmp /t\nenv DEVICE_X=true\ncmd\tfoo bar\t3\tline1\\nline2\n");
    probe() = Probe();
    load_fixture(g_dir + "/fx");
    CHECK_EQ(probe().root, std::string("/r"));
    CHECK_EQ(probe().tmp, std::string("/t"));
    CHECK(devflag("DEVICE_X"));
    CHECK(!devflag("DEVICE_Y"));
    CHECK_EQ(probe().canned["foo bar"].rc, 3);
    CHECK_EQ(probe().canned["foo bar"].out, std::string("line1\nline2"));
  }

  return zt::done();
}
