// MODULE `music`: ES's background music (es-core/src/AudioManager.cpp) for zfront.
//
// What ES does and what is here:
//   * a random track of the music folders, fading in over 1 s, never one of the recently played (the last 40 % of the tracks);
//     when a track ends another one starts (AudioManager::musicEnd_callback); folder search order (playRandomMusic): the theme's
//     music folder, /storage/roms/music, /storage/.config/emulationstation/music (the first folder with audio files wins)
//   * `audio.persystem`: only the sub-folders named like the system on screen (getMusicIn) and a new track on every system change
//   * the music stops while a game runs (FileData::launchGame deinit()) and starts again afterwards; with `StopMusicOnScreenSaver`
//     it also stops under the screensaver and starts again when it ends (SystemScreenSaver)
//   * `audio.display_titles`: a toast with the song title (ID3 for mp3, the title field of tracker modules, else the file name)
//   * keys (es_settings.cfg, ES defaults): audio.bgmusic=true, audio.persystem=false, audio.thememusics=true,
//     audio.display_titles=true, audio.display_titles_time=10, MusicVolume=128, VideoLowersMusic=true,
//     StopMusicOnScreenSaver=false. (audio.useFavoriteMusic and its playlist screens are not ported.)
//
// Output: SDL_mixer (libSDL2_mixer-2.0.so.0 of the image), loaded with dlopen when first needed, so the PC build needs no headers
// and a system without the library simply has no music (the menu rows are hidden). Mix_OpenAudio opens its OWN SDL audio device:
// the navigation cues of sdlr.cpp (AudioOut, a second device opened on demand) keep working beside it, each device mixes in SDL.
// Everything runs on the main thread. The only other thread is SDL_mixer's audio thread, which just posts "track ended" to the
// job queue (the loop wakes up on that), so nothing polls: idle CPU is the decoder's while a track plays and 0 otherwise.
#pragma once
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace zf {

struct Ctx;
class MenuBuilder;
class EsSettings;

namespace music {

// ---- the output ---------------------------------------------------------------------------------------------------------
/// The few SDL_mixer calls the module needs. The real one is `dlopen`ed; tests install a fake.
class Backend {
 public:
  virtual ~Backend() = default;
  /// Opens the audio device (Mix_OpenAudio, 48 kHz then 44.1 kHz).
  virtual bool open() = 0;
  /// Frees the music and releases the device (Mix_CloseAudio), as ES does before a game: emulators need the sound card.
  virtual void close() = 0;
  /// Frees the previous track and starts `path` fading in over `fade_ms`, once (no loop). False when it cannot be played.
  virtual bool play(const std::string& path, int fade_ms) = 0;
  /// Halts and frees the track without calling the "ended" callback.
  virtual void stop() = 0;
  /// 0..128 (MIX_MAX_VOLUME).
  virtual void set_volume(int v) = 0;
  /// Called from the AUDIO thread when a track reaches its end by itself (never after `stop`, `close` or a new `play`).
  virtual void set_on_finished(std::function<void()> cb) = 0;
};

/// Installs a backend (tests). nullptr = "no music available", which also stops the real library from being probed.
void set_backend(std::unique_ptr<Backend> b);
/// True when there is something to play with: `start()` found libSDL2_mixer, or a fake is installed. The library is never loaded
/// before `start()`, so the menus of the PC harness and of the tests do not offer music unless a test installs a backend.
bool available();

// ---- settings (pure) ----------------------------------------------------------------------------------------------------
struct Config {
  bool enabled = true;         // audio.bgmusic
  bool per_system = false;     // audio.persystem
  bool theme_music = true;     // audio.thememusics
  bool titles = true;          // audio.display_titles
  int title_secs = 10;         // audio.display_titles_time (ES: <= 2 or > 120 means 10)
  long volume = 128;           // MusicVolume (the menu slider is 0..100, ES's default is 128 = full)
  bool stop_on_saver = false;  // StopMusicOnScreenSaver
  bool lowers = true;          // VideoLowersMusic
  bool video_audio = true;     // VideoAudio (ES: a silent video never lowers the music)
};
Config read_config(const EsSettings& es);

/// AudioManager::getMaxMusicVolume: 0 stays 0, else 10^((v-100)/40) * 128, at most 128.
int max_volume(long linear);
/// Volume while a video plays (AudioManager::update): a twentieth of the maximum, at least 1 unless the maximum is 0.
int ducked_volume(int max);
/// ES: a title time of 2 or less, or over 120, falls back to 10 s.
int title_seconds(long v);

// ---- picking a track (pure, the tests use temporary folders) ---------------------------------------------------------------
/// Audio extensions ES knows (FileSystemUtil `_audioExtensions`), case-insensitive.
bool is_audio_file(const std::string& path);
/// AudioManager::getMusicIn: the audio files of `dir` and of its sub-folders (all of them, or with `per_system` only those named
/// `system`), folders walked in name order.
void files_in(const std::string& dir, bool per_system, const std::string& system, std::vector<std::string>& out);
/// ES's search: the first of `dirs` that yields files wins (the others are not read).
std::vector<std::string> find_files(const std::vector<std::string>& dirs, bool per_system, const std::string& system);
/// The folders in ES's order for this configuration: theme folder (only without per_system and with theme_music), then
/// /storage/roms/music and /storage/.config/emulationstation/music.
std::vector<std::string> search_dirs(const Config& c, const std::string& theme_dir);

/// AudioManager::addLastPlayed / songWasPlayedRecently: the last 40 % (rounded down) of the number of tracks. With fewer than
/// three tracks the history stays empty.
class Recent {
 public:
  void add(const std::string& song, size_t total);
  bool has(const std::string& song) const;
  size_t size() const { return q_.size(); }

 private:
  std::deque<std::string> q_;
};
/// Index of the track to play: random, asking again up to size-1 times while it was played recently (ES's loop). `rnd(n)` is a
/// number in [0, n).
size_t pick_index(const std::vector<std::string>& files, const Recent& recent, const std::function<size_t(size_t)>& rnd);

/// Title shown for a track: mp3 ID3v2 `title - artist`, else ID3v1, tracker modules (.mod .s3m .xm ...) their title field, else the
/// file name without extension.
std::string song_title(const std::string& path);

// ---- the player ---------------------------------------------------------------------------------------------------------
/// The frontend is on screen: starts the music when audio.bgmusic is on. (ES: AudioManager::init + changePlaylist in main().)
void start(Ctx& cx);
/// The loop ended: stops and releases the device.
void shutdown();
/// A game is about to start / has ended (the window goes away in between): the device is released and a new track starts after.
void pause_for_game();
void resume_after_game();
/// The screensaver started / stopped (StopMusicOnScreenSaver decides whether the music follows).
void on_screensaver(bool on);
/// Carousel card the cursor is on (-1 = none): with audio.persystem a change of system starts another track.
void on_system(Ctx& cx, long item);
/// Another track now (ES: L3/R3 and the "playRandomMusic(false)" calls of the menus).
void next();
/// A music setting changed in the menu: `enabled_changed` starts / stops the music; the volume and the folder mode are applied.
void apply_settings(bool enabled_changed);
/// A video plays (preview): the music goes down to a twentieth of its volume while `on` (only with VideoLowersMusic), and back up when it
/// ends. Jumps, it does not fade as ES does. For the video previews; harmless when no music plays.
void duck(bool on);
/// Folder of the theme's music (`<sound name="directory">`), "" when the theme has none. Zenith has none.
void set_theme_dir(const std::string& dir);

/// Path of the track playing, "" when none (tests).
std::string current_track();
/// Forgets everything (tests).
void reset_for_tests();
/// Prefix put before the absolute music folders (tests use a temporary tree).
void set_root_for_tests(const std::string& root);
/// Replaces the random number source `rnd(n)` in [0, n) (tests).
void set_random_for_tests(std::function<size_t(size_t)> f);

// ---- menu rows ----------------------------------------------------------------------------------------------------------
/// The MUSIC group of ES's sound menu (GuiMenu::openSoundSettings): FRONTEND MUSIC, MUSIC VOLUME, DISPLAY SONG TITLES, SONG TITLE
/// DISPLAY DURATION, ONLY PLAY SYSTEM-SPECIFIC MUSIC FOLDER, PLAY SYSTEM-SPECIFIC MUSIC, LOWER MUSIC WHEN PLAYING VIDEO.
void build_menu(MenuBuilder& m, Ctx& cx);
/// STOP MUSIC ON SCREENSAVER (GuiGeneralScreensaverOptions): only with frontend music on, as in ES.
void build_saver_row(MenuBuilder& m, Ctx& cx);

}  // namespace music
}  // namespace zf
