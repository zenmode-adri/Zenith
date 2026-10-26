// `menu.sound` ("SOUND SETTINGS") and the pieces it shares with the rest of the frontend.
//
// What ES offers on Zenith and what is here (GuiMenu::openSoundSettings, 5406-5580, and the Zenith patches):
//   VOLUME   SYSTEM VOLUME (slider)      -> the OS script `volume N` (pactl + audio.volume under the system.cfg lock)
//            SHOW OVERLAY WHEN VOLUME CHANGES -> es_settings `VolumePopup` (default true), now gates the zfront OSD pill
//            AUDIO OUTPUT / AUDIO PROFILE    -> `set-audio ...`; ES shows them only when /usr/bin/set-audio exists (it does
//                                              not in the current image), so they are hidden unless it does
//   SOUNDS   ENABLE AUDIBLE BATTERY WARNING / BATTERY WARNING THRESHOLD -> system.battery.warning / .warning_threshold
//            (consumed by powerstate.sh, not by the frontend)
// Hidden on purpose (zfront has no player): MUSIC VOLUME, FRONTEND MUSIC and the whole music group, navigation sounds,
// video preview audio. Their keys are never touched, so switching back to ES loses nothing.
//
// Hardware volume keys: the OS handles them (input_sense / adckeys.sh run `volume up|down`, step 5). ES shows its popup
// because libpulse tells it the sink changed. zfront has no libpulse: the "sound-watch" service watches system.cfg with
// inotify and shows the OSD when `audio.volume` changes behind its back (see menu_sound_watch.cpp).
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app.hpp"
#include "menu.hpp"

namespace zf {
namespace sound {

// ---- the environment the section probes (tests and the PC harness replace it) -------------------------------------------
struct Env {
  std::string bin_dir = "/usr/bin/";  // where ES looks for its helper scripts (ApiSystem::isScriptingSupported)
  /// Dev/harness (ZF_SOUND_FAKE=1): every tool counts as installed and the audio lists are canned. Never set on the console.
  bool fake = false;
  /// When set, answers `set-audio list` / `set-audio list-profiles` instead of running them (tests, fake mode).
  std::function<std::string(const std::string& cmd)> lister;
};
/// Process-wide; first use reads ZF_SOUND_FAKE and ZF_SOUND_BIN_DIR. Change it only from the main thread, before opening the menu.
Env& env();
/// True when the helper `name` exists in `env().bin_dir`.
bool have_tool(std::string_view name);

// ---- system volume --------------------------------------------------------------------------------------------------
/// Sets the volume (0..100): applies it with the `volume` script on the job thread (one run at a time, always ending on the
/// last value asked) and shows the OSD when `VolumePopup` is on. For the menu, and for whoever wires the right-stick
/// volume shortcut of ES (`RightStickVolume`, step 5).
void set_volume(Ctx& cx, int percent);
void change_volume(Ctx& cx, int delta);
/// Current volume as zfront knows it (re-reads system.cfg first when nothing was set yet).
int volume(Ctx& cx);
/// A change made by someone else (hardware keys): updates the state and, when `fresh` and VolumePopup is on, shows the OSD.
void external_volume(Ctx& cx, int percent, bool fresh);

/// While true the file watcher ignores changes of audio.volume (our own `volume N` runs write it).
void quiet_for(int ms);
bool quiet();

/// Drops the global volume state (tests).
void reset_state();

// ---- parsing (pure) ----------------------------------------------------------------------------------------------------
/// `audio.volume` of a system.cfg text, 0..100, or -1 when the line is missing or not a number.
int parse_volume(std::string_view cfg_text);
/// ES's rule for `set-audio list` (profiles = false) and `list-profiles` (true): lines `id<TAB>name words`. Profiles whose
/// name starts with a `bluez_card...` token drop that token.
std::vector<Option> parse_audio_list(std::string_view text, bool profiles);

/// Remembers the last volume seen in the file. `update` returns the new volume when it differs from the previous one, else
/// -1. A missing value (-1: `set_setting` deletes the line before appending it) is never a change.
class VolumeTracker {
 public:
  void seed(int v) { last_ = v; }
  int update(int now) {
    if (now < 0) return -1;
    int prev = last_;
    last_ = now;
    return prev >= 0 && prev != now ? now : -1;
  }

 private:
  int last_ = -1;
};

/// What the watcher does with the text of system.cfg after a change.
enum class Seen { Nothing, Sync, Osd };
/// `Nothing`: no real change (value unchanged or line missing) or it is our own `volume N` run (`quiet`); `Sync`: somebody
/// else changed it while a game was running, so only the state is updated (no OSD for something that happened long ago);
/// `Osd`: update the state and show the OSD. `volume` receives the new value for `Sync` and `Osd`.
Seen decide(VolumeTracker& tracker, std::string_view cfg_text, bool is_quiet, bool game_running, int& volume);

// ---- file watching --------------------------------------------------------------------------------------------------------
/// One thread blocked in poll(2) on an inotify descriptor and an eventfd (no wake-ups while nothing changes). When the file
/// `name` inside `dir` is written, replaced or created, `on_change` runs on the watcher thread after `settle_ms` without
/// further events (one `set_setting` makes several).
class FileWatch {
 public:
  FileWatch() = default;
  ~FileWatch() { stop(); }
  FileWatch(const FileWatch&) = delete;
  FileWatch& operator=(const FileWatch&) = delete;
  bool start(const std::string& dir, const std::string& name, std::function<void()> on_change, std::string& err, int settle_ms = 40);
  void stop();
  bool running() const { return th_.joinable(); }

 private:
  int ino_ = -1, ev_ = -1;
  std::thread th_;
};

}  // namespace sound
}  // namespace zf
