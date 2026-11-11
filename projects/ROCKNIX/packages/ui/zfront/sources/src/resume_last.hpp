// RESUME LAST GAME AT STARTUP: when the console boots, the game played last starts again from its autosave, after a short countdown
// that B (or Start) cancels. Off by default (`zfront.resume_last=1` in system.cfg, the GAMES menu switch).
//
// Rules (all of them keep the person from being trapped in a game they did not choose):
//  - once per boot: the same /run marker as the boot game (`boot::startup_game`), claimed at the first start of the frontend that gets
//    here, whatever the outcome, so a restart of zfront (a crash, a quit) never resumes anything later in the same boot;
//  - the game set to start with the console (`global.bootgame.path`) wins: nothing resumes then;
//  - the game is the newest of the "Continue playing" data (the newest savestate screenshot of all the systems) and its ROM must be there;
//  - a last session of THAT game shorter than `kMinSession` seconds means it probably crashed on start: resuming it would loop the
//    console into a crashing game on every boot, so nothing resumes until a longer session replaces it. `note_session` records the
//    length and the ROM of every session in a one-line file that survives reboots (a short run of a tool or of another game does not
//    block the resume of the game that was played for an hour before: only the newest game matters, and it is the one that ran).
//  - Kid / Kiosk: the game must be in the game list as the UI mode shows it (Kid mode loads only the kid games), so a game the mode
//    hides is never resumed; the countdown itself is only a screen with a cancel, so it is not in the `uimode` whitelist.
// The launch is the one of the "Continue playing" row: `savestates::launch_state` (the autosave is loaded).
#pragma once
#include <ctime>
#include <memory>
#include <optional>
#include <string>

#include "app.hpp"

namespace zf::resume {

inline constexpr const char* kKey = "zfront.resume_last";  // "1" = on; absent = off
inline constexpr unsigned long kMinSession = 10;           // seconds: a shorter last session blocks the resume
inline constexpr float kCountdown = 3.0f;                  // seconds shown before the game starts

struct Paths {
  std::string marker;   // once-per-boot marker (the boot game's)
  std::string session;  // length in seconds of the last game session
};
/// The marker of `boot::startup_marker` and `$ZF_LAST_SESSION` or /storage/.config/zfront/last-session.
Paths default_paths();

bool enabled(const Ctx& cx);

/// The game to resume.
struct Target {
  size_t sys = 0, game = 0;
  int slot = -1;     // slot of the newest screenshot (-1 = autosave)
  std::string name;  // game name for the title
  std::string shot;  // savestate screenshot ("" = none)
};
/// The newest "Continue playing" entry of the systems shown by the carousel whose ROM is on disk. Nothing when there is none or when the
/// newest one has lost its ROM (an older game is not resumed in its place).
std::optional<Target> newest(Ctx& cx, time_t now);

/// Records how long a session of the game `rom` (its path) lasted (called after every game); creates the folder. One line: "<secs> <rom>".
void note_session(const std::string& path, unsigned long secs, const std::string& rom);
/// False when the recorded session was of `rom` (or of an unknown game: a line without a path) and lasted less than `kMinSession`
/// (`secs` gets its length). A missing or unreadable file, or the session of another game, allows.
bool session_allows(const std::string& path, const std::string& rom, unsigned long& secs);

/// The countdown screen: "RESUMING <GAME>", the game's picture, "STARTING IN n". A starts at once, B / Start cancel, and when the
/// time is up the launch is requested without a key (`take_transition`, which `App::tick` turns into a launch).
class ResumeScreen : public Screen {
 public:
  explicit ResumeScreen(Target t);
  /// Starts the countdown (the first `draw` does it; tests call it).
  void arm() { armed_ = true; }
  Transition handle(Action a, Ctx& cx) override;
  bool tick(float dt, Ctx& cx) override;
  std::optional<float> wake_in() const override;
  bool take_redraw() override;
  Transition take_transition(Ctx& cx) override;
  void draw(Ctx& cx, std::vector<Op>& out) override;
  bool closes_on_launch() const override { return true; }
  int seconds_left() const;
  const Target& target() const { return t_; }

 private:
  struct View;
  Transition start(Ctx& cx);
  Target t_;
  float elapsed_ = 0;
  bool armed_ = false, done_ = false, redraw_ = false;
  int shown_ = -1;
  std::shared_ptr<View> v_;
};

/// Everything above in one call, for the start of the frontend: nullptr (and `why` filled when it is worth a log line) or the countdown
/// screen to push. Claims the once-per-boot marker whenever the feature is on and no boot game is set.
std::unique_ptr<Screen> begin(Ctx& cx, const Paths& p, time_t now, std::string& why);

}  // namespace zf::resume
