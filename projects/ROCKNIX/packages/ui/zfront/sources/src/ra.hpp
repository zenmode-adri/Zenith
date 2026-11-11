// RetroAchievements account and options (ES: GuiRetroAchievementsSettings + RetroAchievements::testAccount).
//
// The keys are the ones the OS reads: `setsettings.sh` turns them into the RetroArch config and the standalone emulators'
// `cheevos_*.sh` scripts read the token. Two keys differ from the ones ES writes (ES: `challenge_indicators`, `unofficial`, `encore`):
// NOTHING reads those, so zfront writes `challengeindicators` and `testunofficial` (the names setsettings.sh and
// cheevos_dolphin.sh use) and has no ENCORE row.
//
// SECRETS: the password and the token never reach a log, a toast or a process argument. The login request lives in a 0600 file
// of a private temporary directory passed to `curl -K`; texts that come from the server are cleaned (`clean_error`).
#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "app.hpp"

namespace zf::ra {

namespace key {
constexpr const char* kEnabled = "global.retroachievements";
constexpr const char* kUser = "global.retroachievements.username";
constexpr const char* kPass = "global.retroachievements.password";
constexpr const char* kToken = "global.retroachievements.token";
constexpr const char* kHardcore = "global.retroachievements.hardcore";
constexpr const char* kLeaderboards = "global.retroachievements.leaderboards";
constexpr const char* kVerbose = "global.retroachievements.verbose";
constexpr const char* kRich = "global.retroachievements.richpresence";
constexpr const char* kScreenshot = "global.retroachievements.screenshot";
constexpr const char* kChallenge = "global.retroachievements.challengeindicators";
constexpr const char* kUnofficial = "global.retroachievements.testunofficial";
constexpr const char* kSound = "global.retroachievements.sound";
constexpr const char* kLeds = "global.retroachievements.leds";
constexpr const char* kMenuItem = "RetroachievementsMenuitem";  // es_settings.cfg, default on
}  // namespace key

// ---- login (dorequest.php?r=login) -----------------------------------------------------------------------------------------
struct LoginResult {
  enum class Kind : unsigned char {
    Ok,           // `token` is set
    NoCredentials,
    Network,      // curl could not get an answer
    BadReply,     // not the JSON the site sends
    Rejected,     // the site said no: `error` is its (cleaned) text
    Dry,          // dry-run executor: nothing was asked
  } kind = Kind::NoCredentials;
  std::string token, error;
  bool ok() const { return kind == Kind::Ok; }
};

/// The text of the curl config (`curl -K file`) of a login: POST body with the percent-encoded credentials. Secret.
std::string login_config(std::string_view user, std::string_view pass);
/// Reads curl's exit code, the HTTP status and the body of the answer. `user`/`pass` are removed from any text kept.
LoginResult parse_login(int curl_exit, int http, std::string_view body, std::string_view user, std::string_view pass);
/// One line for the person, translated, with no secrets: the server's text for `Rejected`.
std::string describe(const LoginResult& r);
/// A server text made safe to show: control characters become spaces, the credentials are masked, 120 bytes at most.
std::string clean_error(std::string_view s, std::string_view user, std::string_view pass);
/// Blocking: writes the config, runs `curl -K`, parses. Call from a job (`cx.sv->jobs`), never from the UI thread.
/// Honours $ZF_CURL (tests) and $ZF_RA_TMP (scratch directory, default /tmp).
LoginResult login(Services& sv, const std::string& user, const std::string& pass);

/// What ES's "save" compared against: taken when the menu opens.
struct Snapshot {
  bool enabled = false;
  std::string user, pass;
};
Snapshot snapshot(const Ctx& cx);
/// ES's save function: with RA on, and just turned on / credentials changed / no token, logs in (on a job) and stores the token;
/// a failure clears it, turns RA off and shows why. With RA off the token is removed. `done` (optional) runs on the main thread.
void apply_login(Ctx& cx, const Snapshot& before, std::function<void()> done = nullptr);

// ---- unlock sounds ----------------------------------------------------------------------------------------------------------
/// File names (without `.ogg`), sorted, unique, of the two sound folders (re-rooted by `mg::env().root`).
std::vector<std::string> sound_names();
/// True when the device has LEDs on the sticks (ES: $DEVICE_ANALOG_STICKS_LED_CONTROL == "true").
bool has_stick_leds();

/// ES: the RETROACHIEVEMENTS row of the main menu (menu_main.cpp) shows when RetroAchievements is on, an account is set and the entry
/// switch (es_settings `RetroachievementsMenuitem`, on when never set) is on. The row opens the settings of menu_ra.cpp, not a stats viewer.
bool in_main_menu(const Ctx& cx);

}  // namespace zf::ra
