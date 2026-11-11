// MODULE `ssmedia`: the slideshow and random-video screensavers of ES (SystemScreenSaver.cpp / GuiGeneralScreensaverOptions.cpp).
//
// `oscontract` decides WHEN (ScreenSaverTime, wake on a key, no start while a job runs...); this module decides WHAT is shown once the
// behaviour is `slideshow` or `random video`:
//   * the source: the games' `image` / `video` metadata of every game system (collections and image-viewer systems left out, hidden games
//     too: ES `countGameListNodes` / `getFilesRecursive(GAME, true)`), or a custom directory (`SlideshowScreenSaver*` keys);
//   * ES's pick rules: random without repeating until the list is used up (`pickRandomGameMedia`), a random file of the directory
//     matching the extension filter (`pickRandomCustomImage`);
//   * the timing: a new picture every `ScreenSaverSwapImageTimeout` ms (10 s), a new video every `ScreenSaverSwapVideoTimeout` ms (30 s) or
//     when it ends, a 500 ms fade (ES FADE_TIME) for each;
//   * the overlays: game name + system (or the marquee image), date and time, ES's positions and colours.
// Without media (no games with pictures, an empty directory, libvlc missing for the video) ES shows the plain black screensaver: so does
// this (`screensaver::Mode::Black`, panel off).
//
// Threads: the pick (it reads gamelists and stats files) and the image decode run on the shared job thread (`cx.sv->jobs`, `covers`); the
// video is `video.hpp`'s. Everything else is the main thread. While it is on the loop sleeps: wake-ups are the fade (16 ms, 500 ms in total),
// the swap time, the clock of the date/time overlay (once a second) and the frames of a video (`video::set_wake`).
#pragma once
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app.hpp"
#include "gamelist.hpp"
#include "menu.hpp"
#include "util.hpp"

namespace zf {
namespace ssmedia {

// ---- settings (ES keys and defaults) ------------------------------------------------------------------------------------------
struct Config {
  bool video = false;
  long swap_ms = 10000;        // ScreenSaverSwapImageTimeout 10000 / ScreenSaverSwapVideoTimeout 30000 (Settings.cpp:274, :305)
  bool stretch = false;        // SlideshowScreenSaverStretch / StretchVideoOnScreenSaver, false: fill the screen instead of fitting in it
  bool custom = false;         // SlideshowScreenSaverCustomImageSource / ...VideoSource, false
  std::string dir;             // SlideshowScreenSaverImageDir ("/storage/roms/screenshots") / ...VideoDir (empty = the screenshots folder)
  std::string filter;          // SlideshowScreenSaverImageFilter ".png,.jpg" / ...VideoFilter ".mp4,.avi"; empty = every file
  bool recurse = false;        // SlideshowScreenSaverRecurse / ...VideoRecurse, false
  bool game_name = true;       // SlideshowScreenSaverGameName, true (it gates the name for the video too, as ES's setGame)
  std::string game_info = "never";  // ScreenSaverGameInfo (video only): always | start & end | never
  bool marquee = true;         // ScreenSaverMarquee, true
  bool datetime = false;       // ScreenSaverDateTime, false
  std::string date_format = "%Y-%m-%d", time_format = "%H:%M:%S";  // ScreenSaverDateFormat / ScreenSaverTimeFormat
  bool mute = false;           // ScreenSaverVideoMute, false (in screensaver mode ES looks at this key only, not at VideoAudio)
};
constexpr const char* kScreenshotsDir = "/storage/roms/screenshots";  // Paths::getScreenShotPath()
Config load_config(const EsSettings& es, bool video);

// ---- picking (pure: tests) ----------------------------------------------------------------------------------------------------
using Rng = std::function<size_t(size_t n)>;  // 0..n-1
/// ES `imageFilter.find(getExtension(path)) != npos`: a case sensitive SUBSTRING test of the extension (with its dot) in the filter string;
/// an empty filter takes every file. (ES also takes files without an extension, an accident of find(""); not here.)
bool ext_matches(const std::string& path, const std::string& filter);
/// Regular files of `dir` (sub-folders too when `recurse`) that match; names starting with "." are skipped; sorted. Empty when `dir` is no directory.
std::vector<std::string> list_custom(const std::string& dir, const std::string& filter, bool recurse);
/// ES `pickRandomCustomImage`: one of `list_custom` at random; "" when none. An empty `dir` is the screenshots folder.
std::string pick_custom(const Config& c, const Rng& rng);

/// One game of the pick: what the overlay shows.
struct Pick {
  std::string path;  // the picture / video
  std::string game, system, system_full, marquee;
  bool has_game = false;  // false for a custom directory
};

/// The games behind the pick. The real one reads the gamelists; tests give their own.
class GameSource {
 public:
  struct Cand {
    std::string rom, name;
    uint64_t begin = 0, end = 0;  // byte range of the element in the gamelist
  };
  struct Info {
    std::string name, full;
  };
  virtual ~GameSource() = default;
  virtual size_t systems() const = 0;
  virtual Info info(size_t sys) const = 0;
  /// The not-hidden games of the system.
  virtual void entries(size_t sys, std::vector<Cand>& out) = 0;
  /// `image`, `video` and `marquee` of one of them (absolute paths, maybe empty).
  virtual bool details(size_t sys, const Cand& c, GameDetails& out) = 0;
};
/// Snapshot of what a system needs; taken on the main thread, used on the job thread.
struct SysRef {
  std::string name, full, rom_dir, gamelist;
};
/// The systems that count (game systems that are neither collections nor image viewers: ES `isGameSystem`, `IMAGEVIEWER`, `PLATFORM_IGNORE`).
std::vector<SysRef> eligible_systems(const std::vector<System>& systems);
std::unique_ptr<GameSource> make_gamelist_source(std::vector<SysRef> systems);

/// ES `pickRandomGameMedia`: a random game that has the media and whose file exists, each used once until the list is exhausted (then it
/// starts again). Two lists, one per kind, like ES. At most `kMaxTries` games are looked at per call.
class Picker {
 public:
  static constexpr size_t kMaxTries = 400;
  Picker(std::unique_ptr<GameSource> src, Rng rng) : src_(std::move(src)), rng_(std::move(rng)) {}
  bool next(bool video, Pick& out);
  /// Games left in the list of this kind (0 before the first pick).
  size_t remaining(bool video) const { return pool_[video ? 1 : 0].size(); }

 private:
  std::unique_ptr<GameSource> src_;
  Rng rng_;
  std::vector<std::vector<GameSource::Cand>> cands_;
  bool loaded_ = false, built_[2] = {false, false};
  std::vector<std::pair<uint32_t, uint32_t>> pool_[2];  // (system, index in cands_)
};

// ---- timing and look (pure: tests) ------------------------------------------------------------------------------------------
constexpr long kFadeMs = 500;  // ES FADE_TIME
/// 0..1 of the fade that started `t_ms` ago.
float fade_alpha(long t_ms);
bool swap_due(TimePoint now, TimePoint shown_at, long swap_ms);
/// ES `VideoScreenSaver::update`: the game info of a video, 0..1. "always" 1, "never" 0, "start & end": 4 s at the start and 4 s at the end
/// of the `total_ms` the video is shown, 150 ms fades in between.
float info_alpha(const std::string& mode, long t_ms, long total_ms);

// ---- the screensaver ----------------------------------------------------------------------------------------------------------
/// `oscontract::screensaver_start`: the behaviour is `slideshow` / `random video` (the work starts at the next `poll`, which has the `Ctx`).
void request_start(bool video);
/// `oscontract::screensaver_stop`: stops the video, forgets the pick state; late job results are ignored.
void on_stop();
/// True while it is set up or running.
bool active();
/// Main thread, every loop iteration (`App::poll_status`): starts, advances, swaps. True when the picture changed (redraw). Does nothing and
/// costs one comparison when no media screensaver is on.
bool poll(Ctx& cx, TimePoint now);
/// The ops of one frame: black, the picture(s), the overlays.
void draw(Ctx& cx, std::vector<Op>& out);
/// The loop's next wake-up while it is on (fade, swap time, the clock); nullopt when only events can change anything.
std::optional<TimePoint> deadline(TimePoint now);

/// Rows of SCREENSAVER SETTINGS below SCREENSAVER TYPE (ES `addVideoScreensaverOptions` / `addSlideShowScreensaverOptions`, shown for the
/// current type; the menu rebuilds when the type changes).
void build_rows(MenuBuilder& m, Ctx& cx);

// ---- tests ------------------------------------------------------------------------------------------------------------------
void set_rng_for_test(Rng rng);
/// Picks made so far by `Picker`s of this module (job thread), and whether a pick is running.
bool busy_for_test();
void reset_for_test();

}  // namespace ssmedia
}  // namespace zf
