// MODULE `menu-system`: helpers shared by the screens `menu.system`, `menu.system.info`, `menu.system.reset` and
// `menu.updates` (menu_system_main.cpp, menu_system_info.cpp, menu_updates.cpp).
//
// Everything that looks at the OS goes through `Probe`: paths are prefixed with `probe().root` and the `DEVICE_*` variables can
// be faked, so the tests and the review harness (env `ZF_SYS_FIXTURE=<dir>`, see `load_fixture`) run an Ultra-like console on
// a PC. On the device the probe is empty and reads the real `/usr/bin`, `/sys` and the process environment.
//
// Effects follow the rules of the menu stage: commands via `cx.sv->cmd` (dry-run aware), `system.cfg` via `persist`, no threads
// of our own (`cx.sv->jobs` for the slow reads), no sound.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "app.hpp"
#include "menu.hpp"

namespace zf {
namespace msys {

struct Probe {
  /// Prefix of every OS path that is read ("" = the real file system).
  std::string root;
  /// Where the update status files live (`/tmp`).
  std::string tmp = "/tmp";
  /// true: `devenv` reads `env` instead of the process environment.
  bool use_env = false;
  std::map<std::string, std::string> env;
  struct Canned {
    int rc = 0;
    std::string out;
  };
  /// `query` in dry-run mode answers from here (exact command string), otherwise empty output and exit code 0.
  std::map<std::string, Canned> canned;
  /// What "Reboot now" does. Default: `oscontract::quit(Mode::Reboot)`.
  std::function<void(Ctx&)> reboot;
  bool fixture_loaded = false;
};
Probe& probe();
/// Reads `$ZF_SYS_FIXTURE/fixture.txt` once (review harness). Lines: `root <path>`, `tmp <path>`, `env NAME=value`,
/// `cmd<TAB>command<TAB>rc<TAB>output with \n escapes`. Called by `probe()` the first time.
void load_fixture(const std::string& dir);

/// `probe().root + abs`.
std::string os_path(const std::string& abs);
bool exists(const std::string& abs);
/// `/usr/bin/<exe>` exists (ES's `isScriptingSupported` test).
bool have_bin(const char* exe);
std::string devenv(const char* name);
/// `DEVICE_X == "true"`.
bool devflag(const char* name);
/// First line of a file, trimmed ("" when missing).
std::string read_line(const std::string& abs);
/// Whole file (capped at 64 KB), "" when missing.
std::string read_all(const std::string& abs);
/// Words separated by blanks / newlines.
std::vector<std::string> words(const std::string& text);
/// Entries of a directory (names only, sorted), without "." and "..".
std::vector<std::string> list_dir(const std::string& abs);

/// A command whose output we need. Real run, or the canned answer in dry-run. Blocking: call it from a job.
CmdResult query(Services& sv, const std::string& cmd, int timeout_ms = 10000);

/// system.cfg key. ES's rule on save: an empty value, "auto" or "default" REMOVES the key (except `keep_default` keys such as
/// `audio.device`, `updates.branch`). Failure to save: toast, the in-memory value is still updated.
void put(Ctx& cx, const std::string& key, const std::string& value, bool keep_default = false);
void put_bool(Ctx& cx, const std::string& key, bool v);
std::string get(Ctx& cx, const std::string& key, const std::string& def = {});
/// SYSTEM > TIMEZONE: ES's `ln -sf /usr/share/zoneinfo/<zone> $(readlink /etc/localtime)` and the `system.timezone` key.
void set_timezone(Ctx& cx, const std::string& zone);
bool get_bool(Ctx& cx, const std::string& key, bool def);

/// KEYBOARD LAYOUT that goes with a `system.language` code ("es_ES" -> "es", "pt_BR" -> "br"). "us" when there is none, and for
/// the layouts that only type another script (Russian, Ukrainian, Greek: no Latin letters without a toggle key) or change the
/// number row (Vietnamese).
std::string keyboard_for_language(const std::string& lang);
/// A new language moves the keyboard layout with it (`rocknix-keyboard set`, applied at once by the OS with swaymsg), unless the
/// user chose one in KEYBOARD LAYOUT (`zfront.kblayout_manual`). Nothing when the console has no `rocknix-keyboard`.
void keyboard_follow_language(Ctx& cx, const std::string& lang);

/// Writes a sysfs/dev file: `os_path(abs)` content `text`. In dry-run only a line `sysfs: <abs> <- <text>` in `cmd.log`.
bool sysfs_write(Ctx& cx, const std::string& abs, const std::string& text);

/// POWER LED COLOR: `on` / `off` written to the R36 power LED (the one place that knows the path).
bool set_power_led(Ctx& cx, const std::string& on_or_off);
/// Nothing in the OS puts the chosen colour back after a reboot: zfront reapplies `option_powerled` once at start (only when the
/// key is set and this console has that LED).
void restore_power_led(Ctx& cx);

/// "Reboot now" / "Later" question (ES: GuiMsgBox with `quitES(REBOOT)`).
Transition reboot_prompt(const std::string& message);
void do_reboot(Ctx& cx);

/// Fits a long ES warning to the confirm dialog, which cuts what does not fit after about 8 lines on every screen size:
/// the blank lines between paragraphs go and, when the text is long, the closing question moves to the front.
std::string fit_message(const std::string& text);

/// `cx.sv->jobs.post` (or `post_slow` with `slow`: external commands that can take tens of seconds), except under a review
/// fixture (`ZF_SYS_FIXTURE`): the preview harness never drains the jobs, so both halves run at once, on the caller's thread.
void post(Ctx& cx, std::function<void()> work, std::function<void()> done, bool slow = false);

/// A list that is read in a job: `opts` is filled by `done`. While it is not loaded the current value stands in.
struct LazyList {
  std::vector<Option> opts;
  bool loaded = false;
};
/// Shown name of a raw value of a system.cfg list: "default" (governors, suspend mode, LED colour) is translated; the rest as is.
std::string value_label(const std::string& value);
/// Same list with the raw `default` option relabelled by value_label() (the stored value does not change).
std::vector<Option> label_values(std::vector<Option> v);
/// The options to show: the loaded ones, or only `current` (never an empty popup).
std::vector<Option> options_or_current(const LazyList& l, const std::string& current);
/// GPU DRIVER: human name of a `gpudriver` value (libmali, panfrost); an unknown value is shown as is.
std::string gpu_driver_label(const std::string& value);
/// Like options_or_current, but each option keeps the raw `gpudriver` value and shows gpu_driver_label().
std::vector<Option> gpu_driver_options(const LazyList& l, const std::string& current);

}  // namespace msys
}  // namespace zf
