// `menu.system` (ES: "SYSTEM SETTINGS"): language, keyboard, time zone, brightness and panel, LEDs, storage, performance
// governors, overclock, GPU driver, suspend mode, root password and the entry to the reset menu. Keys and scripts are the ones
// ES uses (GuiMenu::openSystemSettings, ROCKNIX/RK3326 build), so switching between ES and zfront loses no setting.
//
// What is NOT shown (hidden, never dimmed):
//  * what needs a tool the image does not have (set-audio, wlr-randr, dtb_overlay, batocera-*), checked on `/usr/bin/<exe>`;
//  * what needs a feature the console does not have (`DEVICE_*` variables of the platform / device quirks);
//  * what zfront cannot honour: POWER SAVING MODE, USER INTERFACE MODE, SCREEN READER, ROCKNIX SCREENSHOT, CLOCK 12-HOUR
//    (read only by ES), MULTIMEDIA KEYS (no OS script reads the key).
#include <algorithm>
#include <memory>

#include "i18n.hpp"
#include "menu.hpp"
#include "menu_groups.hpp"
#include "menu_system.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf {
namespace {

using namespace msys;

struct Lang {
  const char* code;
  const char* name;
};
// ES's list (GuiMenu.cpp), names in their own language. (ES labels hu_HU "HRVATSKI", Croatian: fixed here.)
const Lang kLangs[] = {
    {"ar_YE", "العربية"},   {"ca_ES", "CATALÀ"},         {"cs_CZ", "ČEŠTINA"},         {"cy_GB", "CYMRAEG"},         {"de_DE", "DEUTSCH"},
    {"el_GR", "ΕΛΛΗΝΙΚΆ"},  {"en_US", "ENGLISH (US)"},   {"en_GB", "ENGLISH (UK)"},   {"es_ES", "ESPAÑOL"},         {"es_MX", "ESPAÑOL MEXICANO"},
    {"eu_ES", "EUSKARA"},   {"fi_FI", "SUOMI"},          {"fr_FR", "FRANÇAIS"},        {"gl_ES", "GALEGO"},          {"he_IL", "עברית"},
    {"hu_HU", "MAGYAR"},    {"id_ID", "BAHASA INDONESIA"}, {"it_IT", "ITALIANO"},      {"ja_JP", "日本語"},           {"ko_KR", "한국어"},
    {"nb_NO", "NORSK BOKMÅL"}, {"nl_NL", "NEDERLANDS"},  {"nn_NO", "NORSK NYNORSK"},   {"oc_FR", "OCCITAN"},         {"pl_PL", "POLSKI"},
    {"pt_BR", "PORTUGUÊS BRASILEIRO"}, {"pt_PT", "PORTUGUÊS"}, {"ro_RO", "ROMÂNĂ"},    {"ru_RU", "РУССКИЙ"},         {"sk_SK", "SLOVENČINA"},
    {"sv_SE", "SVENSKA"},   {"tr_TR", "TÜRKÇE"},         {"uk_UA", "УКРАЇНСЬКА"},      {"vi_VN", "TIẾNG VIỆT"},      {"zh_CN", "简体中文"},
    {"zh_TW", "正體中文"},
};

// What the console can do; read once when the screen opens.
struct Caps {
  bool keyboard = false, timezone = false, mangohud = false, touch = false, paneladj = false;
  bool power_led = false, led = false, sticks = false, led_brightness = false, power_led_r36 = false;
  bool devswitch = false, storage = false, merged = false, fan = false, turbo = false, gpu_oc = false, gpudriver = false;
  bool suspend = false, root_pass = false, gpu_govs = false;
};

struct Backlight {
  std::string path;  // brightness file (abs, without the probe root)
  int max = 0;
  int pct = 100;
  std::string key;
};

struct State {
  Caps caps;
  std::vector<Backlight> bl;
  int panel[4] = {50, 50, 50, 50};
  std::vector<std::string> langs_avail;  // codes with a catalog
  std::vector<std::string> cpu_govs, sleep_modes, switch_opts;
  std::string switch_cur;
  LazyList kb_layouts, kb_variants, zones, gpu_govs, led_colors, stick_modes, gpu_drivers;
  std::string kb_layout, kb_variant, tz, gpu_driver;
  std::string reboot_msg;  // non-empty: something needs a reboot
  bool toast_reboot = false;
};

const char* kPanelProp[4] = {"brightness", "contrast", "saturation", "hue"};

std::string until_nul(const std::string& s) {
  size_t n = s.find('\0');
  return std::string(trim(n == std::string::npos ? s : s.substr(0, n)));
}

// ES accepts the short codes `en`, `es`, `fr`, `ko` for its own entries
std::string norm_lang(const std::string& v) {
  if (v.empty() || v == "en") return "en_US";
  if (v == "es") return "es_ES";
  if (v == "fr") return "fr_FR";
  if (v == "ko") return "ko_KR";
  return v;
}

std::string cpu_gov_file() { return "/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors"; }

void read_caps(State& st) {
  Caps& c = st.caps;
  c.keyboard = have_bin("rocknix-keyboard");
  c.timezone = have_bin("timeinfo");
  c.mangohud = devflag("DEVICE_MANGOHUD_SUPPORT") && have_bin("mangohud_set");
  c.touch = devflag("DEVICE_HAS_TOUCHSCREEN");
  c.paneladj = have_bin("paneladj");
  c.power_led = devflag("DEVICE_PWR_LED_CONTROL");
  c.led = devflag("DEVICE_LED_CONTROL") && have_bin("ledcontrol");
  c.sticks = devflag("DEVICE_ANALOG_STICKS_LED_SINGLE") && have_bin("analog_sticks_ledcontrol_single");
  c.led_brightness = devflag("DEVICE_LED_BRIGHTNESS") && have_bin("ledcontrol");
  c.power_led_r36 = exists("/sys/class/leds/blue:power/brightness");
  c.devswitch = devflag("DEVICE_DTB_SWITCH") && have_bin("device-switch");
  c.storage = devenv("DEVICE_MMC_EJECT") != "false";
  c.merged = exists("/storage/.ms_supported");
  c.fan = devflag("DEVICE_HAS_FAN");
  c.turbo = devflag("DEVICE_TURBO_MODE") && have_bin("turbomode");
  c.gpu_oc = devflag("DEVICE_GPU_OVERCLOCK");
  c.gpudriver = have_bin("gpudriver");
  c.root_pass = have_bin("setrootpass");

  // brightness: every backlight with the two sysfs files (ES shows more than one only on dual-screen consoles)
  bool dual = devflag("DEVICE_HAS_DUAL_SCREEN");
  for (const std::string& n : list_dir("/sys/class/backlight")) {
    std::string d = "/sys/class/backlight/" + n;
    long max = 0, cur = 0;
    if (!exists(d + "/brightness") || !parse_int(read_line(d + "/max_brightness"), max) || max <= 0) continue;
    parse_int(read_line(d + "/brightness"), cur);
    Backlight b;
    b.path = d + "/brightness";
    b.max = int(max);
    b.pct = std::min(100, std::max(5, int(float(cur) / float(max) * 100.0f + 0.5f)));
    // keys of the OS script `brightness`: system.brightness, system.brightness2...
    b.key = st.bl.empty() ? "system.brightness" : "system.brightness" + std::to_string(st.bl.size() + 1);
    st.bl.push_back(b);
    if (!dual) break;
  }
  c.gpu_govs = exists("/etc/profile.d/099-freqfunctions");

  st.cpu_govs.push_back("default");
  for (const std::string& g : words(read_all(cpu_gov_file()))) st.cpu_govs.push_back(g);
  if (st.cpu_govs.size() == 1) st.cpu_govs.clear();  // no cpufreq: nothing to choose
  // ES: `echo default; tr ' ' '\n' < /sys/power/state | grep -v disk`
  std::vector<std::string> modes = words(read_all("/sys/power/state"));
  if (!modes.empty()) st.sleep_modes.push_back("default");
  for (const std::string& m : modes)
    if (m != "disk") st.sleep_modes.push_back(m);
  c.suspend = have_bin("suspendmode") && !st.sleep_modes.empty();

  if (c.devswitch) {
    const std::string dir = "/sys/firmware/devicetree/base/rocknix,device_switch";
    st.switch_cur = until_nul(read_all(dir + "/this"));
    for (const std::string& n : list_dir(dir))
      if (n != "name" && n != "this") st.switch_opts.push_back(n);
    if (st.switch_cur.empty()) st.switch_cur = "unknown";
    if (st.switch_opts.empty()) c.devswitch = false;
  }
}

// "code Name" lines (rocknix-keyboard) -> options
std::vector<Option> parse_code_name(const std::string& out) {
  std::vector<Option> v;
  size_t i = 0;
  while (i < out.size()) {
    size_t e = out.find('\n', i);
    if (e == std::string::npos) e = out.size();
    std::string line(trim(std::string_view(out).substr(i, e - i)));
    i = e + 1;
    size_t sp = line.find(' ');
    if (line.empty() || sp == std::string::npos) continue;
    v.push_back({line.substr(0, sp), std::string(trim(std::string_view(line).substr(sp + 1)))});
  }
  return v;
}

std::vector<Option> parse_zones(const std::string& out) {
  std::vector<Option> v;
  std::string cur;
  auto flush = [&] {
    std::string z(trim(cur));
    if (!z.empty()) v.push_back({z, z});
    cur.clear();
  };
  for (char c : out) {
    if (c == ',' || c == '\n')
      flush();
    else
      cur += c;
  }
  flush();
  return v;
}

std::vector<Option> parse_lines(const std::string& out) {
  std::vector<Option> v;
  for (const std::string& w : words(out)) v.push_back({w, w});
  return v;
}

std::vector<Option> to_options(const std::vector<std::string>& v) {
  std::vector<Option> o;
  for (const std::string& s : v) o.push_back({s, s});
  return o;
}

// Posts a read; `parse` runs on the worker, the list is stored from `done` (main thread) and the menu refreshed.
void load_list(Ctx& cx, const Refresh& refresh, const std::string& cmd, std::function<std::vector<Option>(const std::string&)> parse,
               std::function<void(std::vector<Option>)> store) {
  auto sv = cx.sv;
  auto res = std::make_shared<std::vector<Option>>();
  post(cx, [sv, cmd, parse, res] { *res = parse(query(*sv, cmd, 15000).out); },
                   [res, store, refresh] {
                     store(std::move(*res));
                     if (refresh) refresh();
                   });
}

// What a screen made of `secs` can show: the capabilities of the parts that are not in it are switched off.
Caps masked(const Caps& in, unsigned secs) {
  using namespace sys_sec;
  Caps c = in;
  if (!(secs & kLeds)) c.power_led = c.led = c.sticks = c.led_brightness = c.power_led_r36 = false;
  if (!(secs & kPanel)) c.paneladj = false;
  if (!(secs & kPower)) c.fan = c.gpu_govs = false;
  if (!(secs & kSuspend)) c.suspend = false;
  if (!(secs & kTurbo)) c.turbo = c.gpu_oc = c.mangohud = false;
  if (!(secs & kSysAdv)) c.touch = c.devswitch = c.gpudriver = c.root_pass = false;
  if (!(secs & kStorage)) c.storage = false;
  if (!(secs & kLang)) c.keyboard = c.timezone = false;
  return c;
}

void build_system(MenuBuilder& m, Ctx& cx, const std::shared_ptr<State>& sp, unsigned secs) {
  using namespace sys_sec;
  State& st = *sp;
  const Caps c = masked(st.caps, secs);
  const bool hdr = !(secs & kNoHdr);  // section headers
  const bool cpugov = (secs & kPower) && !st.cpu_govs.empty();

  if (!st.reboot_msg.empty()) {
    std::string msg = st.reboot_msg;
    m.action(tr("Reboot now"), [msg] { return reboot_prompt(msg); }).hint(tr("REBOOT REQUIRED TO APPLY THE NEW CONFIGURATION")).with_id("reboot-now");
  }

  // ---- SYSTEM
  if (hdr && (secs & (kInfo | kLang))) m.header(tr("SYSTEM"));
  if (secs & kInfo) m.submenu(tr("INFORMATION"), "menu.system.info");

  if (secs & kLang) {  // LANGUAGE (REGION): ES list, reduced to the languages that have a catalog (English always)
    std::string cur = norm_lang(get(cx, "system.language"));
    std::vector<Option> o;
    for (const Lang& l : kLangs) {
      std::string code = l.code;
      bool have = code == "en_US" || code == cur;
      for (const std::string& a : i18n::candidates(code)) have = have || std::find(st.langs_avail.begin(), st.langs_avail.end(), a) != st.langs_avail.end();
      if (have) o.push_back({code, l.name});
    }
    m.choice(tr("LANGUAGE (REGION)"), o, [&cx] { return norm_lang(get(cx, "system.language")); }, [&cx](const std::string& v) {
      put(cx, "system.language", v, true);
      i18n::set_language(v);
    }).no_cycle();
  }

  if (c.keyboard) {
    std::string lay = st.kb_layout, var = st.kb_variant;
    auto apply = [&cx, sp, r = m.refresh](const std::string& layout, const std::string& variant) {
      auto sv = cx.sv;
      sp->kb_layout = layout;
      sp->kb_variant = variant;
      std::string cmd = "/usr/bin/rocknix-keyboard set " + shell_quote(layout) + " " + shell_quote(variant);
      auto rc = std::make_shared<int>(-1);
      post(cx, [sv, cmd, rc] { *rc = query(*sv, cmd, 15000).exit_code; },
                       [&cx, sp, rc, layout, variant, r] {
                         if (*rc == 0) {
                           put(cx, "system.kblayout", layout, true);
                           put(cx, "system.kbvariant", variant, true);
                           sp->reboot_msg = tr("REBOOT REQUIRED TO APPLY THE NEW CONFIGURATION");
                           sp->toast_reboot = true;
                         } else {
                           sp->kb_layout = get(cx, "system.kblayout", "us");
                           sp->kb_variant = get(cx, "system.kbvariant", "none");
                           cx.sv->toasts.push(tr("AN ERROR OCCURRED"));
                         }
                         if (r) r();
                       });
    };
    m.choice(tr("KEYBOARD LAYOUT"), options_or_current(st.kb_layouts, st.kb_layout), [sp] { return sp->kb_layout; },
             [sp, apply, &cx](const std::string& v) {
               if (v == sp->kb_layout) return;
               sp->kb_variants.loaded = false;
               apply(v, "none");  // ES: the variant goes back to "none" with a new layout
               (void)cx;
             }).no_cycle().hint(tr("Select the physical keyboard layout. A reboot may be required for changes to take full effect."));
    std::vector<Option> vo = {{"none", tr("NONE")}};
    if (st.kb_variants.loaded)
      for (const Option& o : st.kb_variants.opts) vo.push_back(o);
    else if (var != "none")
      vo.push_back({var, var});
    m.choice(tr("KEYBOARD VARIANT"), vo, [sp] { return sp->kb_variant; },
             [sp, apply](const std::string& v) {
               if (v != sp->kb_variant) apply(sp->kb_layout, v);
             }).no_cycle().hint(tr("Select the keyboard variant for the chosen layout (for example, dead keys or alternative key placement)."));
  }

  if (c.timezone) {
    std::string cur = st.tz;
    m.choice(tr("TIMEZONE"), options_or_current(st.zones, cur), [sp] { return sp->tz; }, [&cx, sp](const std::string& v) {
      if (v.empty() || v == sp->tz) return;
      sp->tz = v;
      // ES: ln -sf /usr/share/zoneinfo/<tz> $(readlink /etc/localtime); then the key
      cx.sv->cmd.run("ln -sf " + shell_quote("/usr/share/zoneinfo/" + v) + " $(readlink /etc/localtime)");
      put(cx, "system.timezone", v, true);
    }).no_cycle();
  }

  if (c.mangohud)
    m.toggle(tr("ENABLE MANGOHUD OVERLAY"), [&cx] { return get(cx, "rocknix.mangohud.enabled") == "1"; }, [&cx](bool b) {
      cx.sv->cmd.spawn(b ? "/usr/bin/mangohud_set enable" : "/usr/bin/mangohud_set disable");
      put_bool(cx, "rocknix.mangohud.enabled", b);
    });
  if (c.touch)
    m.toggle(tr("ENABLE TOUCHSCREEN KEYBOARD"), [&cx] { return get(cx, "rocknix.touchscreen-keyboard.enabled") == "1"; }, [&cx](bool b) {
      cx.sv->cmd.spawn(b ? "systemctl start touchkeyboard" : "systemctl stop touchkeyboard");
      put_bool(cx, "rocknix.touchscreen-keyboard.enabled", b);
    });

  // ---- HARDWARE
  if (hdr && (secs & kBright) && !st.bl.empty()) m.header(secs == kAll ? tr("HARDWARE") : tr("SCREEN"));
  for (size_t i = 0; (secs & kBright) && i < st.bl.size(); ++i) {
    std::string label = tr("SCREEN BRIGHTNESS");
    if (i > 0) label += " " + std::to_string(i + 1);
    m.slider(label, 5, 100, 1, [sp, i] { return sp->bl[i].pct; }, [&cx, sp, i](int v) {
      Backlight& b = sp->bl[i];
      b.pct = v;
      // like the OS script `brightness set`: floor(pct / 100 * max)
      sysfs_write(cx, b.path, std::to_string(v * b.max / 100));
    }).unit("%").commit([&cx, sp, i](int v) { put(cx, sp->bl[i].key, std::to_string(v), true); });
  }
  if (c.paneladj) {
    if (hdr) m.header(tr("PANEL ADJUSTMENT"));
    const char* labels[4] = {N_("GAMMA"), N_("CONTRAST"), N_("SATURATION"), N_("HUE")};
    for (int k = 0; k < 4; ++k) {
      m.slider(tr(labels[k]), 5, 100, 5, [sp, k] { return sp->panel[k]; }, [&cx, sp, k](int v) {
        sp->panel[k] = v;
        // ES: runs in the background; paneladj applies the latest value
        cx.sv->cmd.spawn(std::string("/usr/bin/paneladj ") + kPanelProp[k] + " " + std::to_string(v) + " nosave >/dev/null 2>&1");
      }).unit("%").commit([&cx, k](int v) { put(cx, std::string("display.") + kPanelProp[k], std::to_string(v), true); });
    }
  }

  bool leds = c.power_led || c.led || c.sticks || c.led_brightness || c.power_led_r36;
  if (hdr && leds) m.header(tr("LEDS"));
  if (c.power_led)
    m.toggle(tr("DISABLE POWER LED"), [&cx] { return get(cx, "powerled.disabled") == "1"; }, [&cx](bool b) { put_bool(cx, "powerled.disabled", b); });
  if (c.led) {
    std::string cur = get(cx, "led.color", "default");
    m.choice(tr("LED COLOR"), options_or_current(st.led_colors, cur), [&cx] { return get(cx, "led.color", "default"); }, [&cx](const std::string& v) {
      put(cx, "led.color", v);
      cx.sv->cmd.spawn("/usr/bin/sh -lc " + shell_quote("/usr/bin/ledcontrol " + shell_quote(v)) + " >/dev/null 2>&1");
    }).no_cycle();
  }
  if (c.sticks) {
    std::string cur = get(cx, "analogsticks.led.color", "off");
    m.choice(tr("JOYSTICK LED"), options_or_current(st.stick_modes, cur), [&cx] { return get(cx, "analogsticks.led.color", "off"); }, [&cx](const std::string& v) {
      put(cx, "analogsticks.led.color", v, true);
      std::string safe;
      for (char ch : v)
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') safe += ch;
      if (safe.empty()) return;
      // ES (applyJoystickLedAsync): the last requested mode wins, one worker applies it
      cx.sv->cmd.spawn("echo " + safe + " > /run/joyled.want; ( flock -n 9 || exit 0; cur=; while w=$(cat /run/joyled.want); [ \"$w\" != \"$cur\" ]; do "
                       "/usr/bin/analog_sticks_ledcontrol_single \"$w\"; cur=\"$w\"; done ) 9>/run/joyled.lock >/dev/null 2>&1");
    }).no_cycle();
  }
  if (c.led_brightness)
    m.choice(tr("LED BRIGHTNESS"), {{"max", tr("MAX")}, {"mid", tr("MID")}, {"min", tr("MIN")}}, [&cx] { return get(cx, "led.brightness", "max"); },
             [&cx](const std::string& v) {
               put(cx, "led.brightness", v, true);
               cx.sv->cmd.spawn("/usr/bin/ledcontrol brightness " + shell_quote(v));
             });
  if (c.power_led_r36)
    m.choice(tr("POWER LED COLOR"), {{"on", tr("on")}, {"off", tr("off")}}, [&cx] { return get(cx, "option_powerled", "on") == "off" ? std::string("off") : std::string("on"); },
             [&cx](const std::string& v) {
               // ES (setPowerLedR36): the LED itself, then the key
               sysfs_write(cx, "/sys/class/leds/blue:power/brightness", v == "off" ? "0" : "1");
               put(cx, "option_powerled", v, true);
             });

  if (c.devswitch) {
    if (hdr) m.header(tr("DEVICE"));
    m.choice(tr("THIS DEVICE IS"), to_options(st.switch_opts), [sp] { return sp->switch_cur; }, [&cx, sp, r = m.refresh](const std::string& v) {
      if (v == sp->switch_cur) return;
      cx.sv->cmd.run("/usr/bin/device-switch " + shell_quote(v));
      sp->switch_cur = v;
      sp->reboot_msg = tr("Device will be switched on next reboot");
      if (r) r();
    }).no_cycle();
  }

  // ---- STORAGE
  if (c.storage) {
    if (hdr) m.header(tr("STORAGE"));
    bool auto_mount = get_bool(cx, "system.automount", false);
    m.toggle(tr("AUTODETECT GAMES CARD"), [&cx] { return get_bool(cx, "system.automount", false); }, [&cx](bool b) {
      put_bool(cx, "system.automount", b);
      cx.sv->cmd.spawn("/usr/bin/systemctl restart rocknix-automount");
    });
    if (c.merged && auto_mount) {
      m.toggle(tr("ENABLE MERGED STORAGE"), [&cx] { return get_bool(cx, "system.merged.storage", false); }, [&cx](bool b) {
        put_bool(cx, "system.merged.storage", b);
        cx.sv->cmd.spawn("/usr/bin/systemctl restart rocknix-automount");
      });
      // ES lets you pick and then asks "WARNING ... CONTINUE?"; a choice cannot ask, so the warning comes first, then the list.
      std::string cur = get(cx, "system.merged.device", "default");
      std::vector<Option> opts = {{"default", tr("DEFAULT")}, {"external", tr("EXTERNAL")}, {"internal", tr("INTERNAL")}};
      std::string shown = cur;
      for (const Option& o : opts)
        if (o.value == cur) shown = o.label;
      m.action(tr("MERGED STORAGE PRIMARY CARD"), [&cx, opts, cur] {
        ui::ConfirmOpts co;
        co.danger = true;
        return ui::confirm(
            tr("WARNING: CHANGING THE PRIMARY CARD CAN CAUSE ACCESS TO GAMES TO BE LOST, REQUIRING MANUAL INTERVENTION TO CORRECT. CONTINUE?"),
            [&cx, opts, cur](Ctx&) {
              return ui::choose(tr("MERGED STORAGE PRIMARY CARD"), opts, cur, [](Ctx& c2, const std::string& v) {
                put(c2, "system.merged.device", v);
                c2.sv->cmd.spawn("/usr/bin/systemctl restart rocknix-automount " + shell_quote(v));
              });
            },
            co);
      }).value(shown).with_id("merged-device");
    }
    bool ms = c.merged;
    m.action(tr("EJECT MICROSD CARD"), [&cx, ms] {
      cx.sv->cmd.run(ms ? "/usr/bin/umount -f /storage/roms; /usr/bin/umount -f /storage/games-external" : "/usr/bin/umount -f /storage/roms");
      return ui::message(tr("You may now remove the card."));
    });
  }

  // ---- PERFORMANCE
  if (hdr && (c.fan || cpugov || c.gpu_govs || c.turbo || c.gpu_oc || c.gpudriver)) m.header(tr("PERFORMANCE"));
  if (c.fan)
    m.choice(tr("COOLING PROFILE"),
             {{"auto", tr("AUTO")}, {"quiet", tr("QUIET")}, {"moderate", tr("MODERATE")}, {"aggressive", tr("AGGRESSIVE")}, {"custom", tr("CUSTOM")}},
             [&cx] { return get(cx, "cooling.profile", "auto"); }, [&cx](const std::string& v) {
               put(cx, "cooling.profile", v);
               cx.sv->cmd.spawn("systemctl restart fancontrol");
             });
  if (cpugov)
    m.choice(tr("DEFAULT SCALING GOVERNOR"), to_options(st.cpu_govs), [&cx] { return get(cx, "system.cpugovernor", "default"); }, [&cx](const std::string& v) {
      put(cx, "system.cpugovernor", v);
      cx.sv->cmd.spawn("/usr/bin/sh -lc " + shell_quote(". /etc/profile.d/099-freqfunctions; " + v) + " >/dev/null 2>&1");
    }).no_cycle();
  if (c.gpu_govs) {
    std::string cur = get(cx, "system.gpuperf", "default");
    m.choice(tr("DEFAULT GPU SCALING GOVERNOR"), options_or_current(st.gpu_govs, cur), [&cx] { return get(cx, "system.gpuperf", "default"); }, [&cx](const std::string& v) {
      put(cx, "system.gpuperf", v);
      cx.sv->cmd.spawn("/usr/bin/sh -lc " + shell_quote(". /etc/profile.d/099-freqfunctions; gpu_performance_level " + v) + " >/dev/null 2>&1");
    }).no_cycle();
  }
  if (c.turbo)
    m.toggle(tr("ENABLE CPU OVERCLOCK"), [&cx] { return get(cx, "enable.turbo-mode") == "1"; }, [&cx](bool b) {
      cx.sv->cmd.spawn(b ? "/usr/bin/turbomode enable" : "/usr/bin/turbomode disable");
      put_bool(cx, "enable.turbo-mode", b);
    });
  if (c.gpu_oc)
    m.toggle(tr("ENABLE GPU OVERCLOCK"), [&cx] { return get(cx, "enable.gpu-overclock") == "1"; }, [&cx](bool b) {
      cx.sv->cmd.spawn(std::string("/usr/lib/autostart/quirks/platforms/${HW_DEVICE}/bin/gpu_overclock ") + (b ? "enable" : "disable"));
      put_bool(cx, "enable.gpu-overclock", b);
    });
  if (c.gpudriver)
    m.choice(tr("GPU DRIVER"), options_or_current(st.gpu_drivers, st.gpu_driver), [sp] { return sp->gpu_driver; }, [&cx, sp, r = m.refresh](const std::string& v) {
      if (v.empty() || v == sp->gpu_driver) return;
      cx.sv->cmd.run("/usr/bin/gpudriver " + shell_quote(v), 30000);
      sp->gpu_driver = v;
      sp->reboot_msg = tr("GPU driver will be switched on next reboot");
      if (r) r();
    }).no_cycle();

  if (c.suspend) {
    if (hdr) m.header(tr("SUSPEND"));
    m.choice(tr("DEVICE SUSPEND MODE"), to_options(st.sleep_modes), [&cx] { return get(cx, "system.suspendmode", "default"); }, [&cx](const std::string& v) {
      put(cx, "system.suspendmode", v);
      cx.sv->cmd.spawn("/usr/bin/suspendmode " + shell_quote(v));
    }).no_cycle();
  }

  // ---- ADVANCED
  if (hdr && (secs & kSysAdv) && c.root_pass) m.header(tr("ADVANCED"));
  if (c.root_pass) {
    // ES: SECURITY > ROOT PASSWORD (plain text); one level less here
    m.text(tr("ROOT PASSWORD"), [&cx] { return get(cx, "root.password", "linux"); }, [&cx](const std::string& v) {
      if (v.empty()) {
        cx.sv->toasts.push(tr("AN ERROR OCCURRED"));
        return;
      }
      cx.sv->cmd.spawn("setrootpass " + shell_quote(v));
      put(cx, "root.password", v, true);
    });
  }
  if (secs & kReset) m.submenu(tr("SYSTEM MANAGEMENT AND RESET"), "menu.system.reset");
}

MenuSpec make_system_spec(Ctx& cx, unsigned secs) {
  auto st = std::make_shared<State>();
  read_caps(*st);
  st->kb_layout = get(cx, "system.kblayout", "us");
  st->kb_variant = get(cx, "system.kbvariant", "none");
  st->tz = get(cx, "system.timezone");
  st->langs_avail = i18n::available_languages();
  for (int i = 0; i < 4; ++i) {
    long v = 50;
    std::string raw = get(cx, std::string("display.") + kPanelProp[i]);
    if (!raw.empty()) parse_int(raw, v);
    v = std::min(100L, std::max(5L, v));
    st->panel[i] = int((v + 2) / 5) * 5;  // ES rounds to the 5 % steps of the slider
  }

  MenuSpec s;
  s.title = tr("SYSTEM SETTINGS");
  s.title_key = N_("SYSTEM SETTINGS");
  s.build = [st, secs](MenuBuilder& m, Ctx& cx) { build_system(m, cx, st, secs); };
  s.on_open = [st, secs](Ctx& cx, const Refresh& r) {
    const Caps c = masked(st->caps, secs);
    if (c.timezone) {
      auto sv = cx.sv;
      bool need_cur = st->tz.empty();
      auto cur = std::make_shared<std::string>();
      auto res = std::make_shared<std::vector<Option>>();
      post(cx, 
          [sv, need_cur, cur, res] {
            *res = parse_zones(query(*sv, "/usr/bin/timeinfo timezones").out);
            if (need_cur) *cur = std::string(trim(query(*sv, "/usr/bin/timeinfo current_timezone").out));
          },
          [st, cur, res, r] {
            st->zones.opts = std::move(*res);
            st->zones.loaded = true;
            if (st->tz.empty()) st->tz = *cur;
            if (r) r();
          });
    }
    if (c.keyboard) {
      load_list(cx, r, "/usr/bin/rocknix-keyboard list-layouts", parse_code_name, [st](std::vector<Option> o) { st->kb_layouts.opts = std::move(o), st->kb_layouts.loaded = true; });
      load_list(cx, r, "/usr/bin/rocknix-keyboard list-variants " + shell_quote(st->kb_layout), parse_code_name,
                [st](std::vector<Option> o) { st->kb_variants.opts = std::move(o), st->kb_variants.loaded = true; });
    }
    if (c.led)
      load_list(cx, r, "/usr/bin/sh -lc \"/usr/bin/ledcontrol list\"", parse_lines, [st](std::vector<Option> o) { st->led_colors.opts = std::move(o), st->led_colors.loaded = true; });
    if (c.sticks)
      load_list(cx, r, "/usr/bin/sh -lc \"/usr/bin/analog_sticks_ledcontrol_single list | tr '\\n' ' '\"", parse_lines,
                [st](std::vector<Option> o) { st->stick_modes.opts = std::move(o), st->stick_modes.loaded = true; });
    if (c.gpu_govs)
      load_list(cx, r, "/usr/bin/sh -lc \". /etc/profile.d/099-freqfunctions; get_available_gpu_governors\"", parse_lines,
                [st](std::vector<Option> o) { st->gpu_govs.opts = std::move(o), st->gpu_govs.loaded = true; });
    if (c.gpudriver) {
      auto sv = cx.sv;
      auto cur = std::make_shared<std::string>();
      auto res = std::make_shared<std::vector<Option>>();
      post(cx, 
          [sv, cur, res] {
            *cur = std::string(trim(query(*sv, "/usr/bin/gpudriver").out));
            *res = parse_lines(query(*sv, "/usr/bin/gpudriver --options").out);
          },
          [st, cur, res, r] {
            st->gpu_drivers.opts = std::move(*res);
            st->gpu_drivers.loaded = true;
            st->gpu_driver = *cur;
            if (r) r();
          });
    }
  };
  s.on_close = [st](Ctx& cx) {
    if (st->toast_reboot) cx.sv->toasts.push(tr("REBOOT REQUIRED TO APPLY THE NEW CONFIGURATION"), 4000);
  };
  return s;
}

}  // namespace

MenuSpec system_spec(Ctx& cx, unsigned sections) { return make_system_spec(cx, sections); }

bool system_has_section(unsigned section) {
  State st;
  read_caps(st);
  const Caps& c = st.caps;
  if (section & sys_sec::kPanel) return c.paneladj;
  if (section & sys_sec::kTurbo) return c.turbo || c.gpu_oc || c.mangohud;
  if (section & sys_sec::kSysAdv) return c.touch || c.devswitch || c.gpudriver || c.root_pass;
  return true;
}

ZF_REGISTER_SCREEN("menu.system", [](Ctx& cx, const std::string&) { return make_menu(make_system_spec(cx, sys_sec::kAll)); });

}  // namespace zf
