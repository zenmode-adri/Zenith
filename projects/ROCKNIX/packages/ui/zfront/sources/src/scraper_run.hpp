// The zfront scraper engine: queue building, one game per job on the shared worker, flush of the gamelist, progress state
// for the UI. The UI (menu_scraper.cpp) only reads this state; tests drive it headless.
//
// Threading: everything here runs on the MAIN thread except `process`/`connect`/`prepare_system`, which run as `cx.sv->jobs`
// work on immutable data (`Env`, the item list) and return their result through a shared_ptr that the job's `done` reads on the
// main thread. The one worker is FIFO, so covers/favourites interleave with the scrape instead of waiting for it to end.
// Network: ONLY the `curl` binary (or $ZF_CURL, tests), started through `cx.sv->cmd.run`, with the URL (which carries the
// credentials) in a 0600 config file inside a private 0700 directory: it never appears on a command line, in the dry-run
// command log, in a toast or in an error list.
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app.hpp"
#include "gamelist.hpp"
#include "scraper_ss.hpp"

namespace zf::scraper {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

/// This build can talk to ScreenScraper (has a developer login).
bool available();

struct Options {
  ss::Wish wish;
  bool overwrite_names = true, overwrite_desc = true, overwrite_media = true;  // ScrapeNames / ScrapeDescription / ScrapeOverWrite
  int resize_px = 640;                                                          // ScraperResizeWidth (else Height), 0 = full size
  std::string user, pass, locale;                                               // ScreenScraperUser/Pass, system.language
  enum class Filter : uint8_t { All, MissingAny, MissingAll } filter = Filter::MissingAny;  // GAMES TO SCRAPE FOR
  int skip_days = 15;                                                           // IGNORE RECENTLY SCRAPED GAMES (0 = no)
  std::vector<size_t> systems;                                                  // indexes into Ctx::systems
  // engine tuning (tests lower them)
  int min_interval_ms = 1000;  // between two API requests (raised from the account's requests per minute)
  int busy_retry_ms = 15000;   // wait after "too many requests" (a longer Retry-After wins)
  int max_retries = 5;
  int flush_every = 50;        // games between two writes of the gamelist
  int flush_secs = 120;
  bool follow_account_rate = true;  // raise the interval to the account's requests per minute (tests turn it off)
  int api_timeout_s = 40, media_timeout_s = 90, video_timeout_s = 240;

  /// ES's names and defaults (Settings.cpp): absent keys behave as in ES, so switching between ES and zfront changes nothing.
  static Options load(const Ctx& cx);
};

/// Settings of es_settings.cfg that the menus edit, in one place (read by `Options::load`).
namespace key {
constexpr const char* kImage = "ScrapperImageSrc";
constexpr const char* kThumb = "ScrapperThumbSrc";
constexpr const char* kLogo = "ScrapperLogoSrc";
constexpr const char* kRegion = "ScraperRegion";
constexpr const char* kNames = "ScrapeNames";
constexpr const char* kDesc = "ScrapeDescription";
constexpr const char* kOverwrite = "ScrapeOverWrite";
constexpr const char* kRatings = "ScrapeRatings";
constexpr const char* kVideos = "ScrapeVideos";
constexpr const char* kFanart = "ScrapeFanart";
constexpr const char* kBoxBack = "ScrapeBoxBack";
constexpr const char* kMap = "ScrapeMap";
constexpr const char* kManual = "ScrapeManual";
constexpr const char* kUser = "ScreenScraperUser";
constexpr const char* kPass = "ScreenScraperPass";
constexpr const char* kResizeW = "ScraperResizeWidth";
constexpr const char* kResizeH = "ScraperResizeHeight";
}  // namespace key

enum class Phase : uint8_t { Preparing, Connecting, Running, Stopping, Finished, Cancelled, Aborted };
/// Why a game failed (the UI turns it into text with `describe`).
enum class Err : uint8_t { None, Network, Busy, Server, Write };
/// Why the whole run stopped.
enum class Abort : uint8_t { None, BadLogin, ApiClosed, Maintenance, Blacklisted, Quota, TooManyFails, NoNetwork, DiskFull };

const char* describe(Err e);    // translated
const char* describe(Abort a);  // translated

/// Answer of "test my account" (ssuserInfos.php).
struct AccountInfo {
  bool reached = false;      // ScreenScraper answered
  Abort fail = Abort::None;  // BadLogin / ApiClosed / Maintenance / Blacklisted / NoNetwork
  std::string id;
  int requests_today = 0, max_per_day = 0, max_per_min = 0;
};
/// Asks ScreenScraper about the account on the job worker; `done` runs on the main thread. Nothing is written anywhere.
void check_account(Ctx& cx, const Options& o, std::function<void(const AccountInfo&)> done);

/// Systems of `cx.systems` that ScreenScraper knows (by their `<platform>`), as indexes.
std::vector<size_t> supported_systems(const Ctx& cx);

struct Stats {
  size_t total = 0;        // games in the queue
  size_t done = 0;         // processed (found + not found + failed)
  size_t found = 0, not_found = 0, errors = 0;
  size_t warnings = 0;     // found, but some media could not be downloaded
  size_t media = 0;        // files downloaded
  uint64_t bytes = 0;
};

struct ErrLine {
  std::string game;
  Err code = Err::None;
  bool warning = false;  // a media download failed but the game was scraped
};

struct Item;
struct Env;
struct StepResult;

class Run : public std::enable_shared_from_this<Run> {
 public:
  /// `cx` must outlive the run (it does: it is the app's context). Takes a private copy of what the worker needs.
  static std::shared_ptr<Run> create(Ctx& cx, Options o);
  ~Run();
  Run(const Run&) = delete;
  Run& operator=(const Run&) = delete;

  /// Starts building the queue (one job per system), then checks the account and scrapes.
  void start();
  /// Stops after the game in progress (a download in flight finishes first), writes what is done and ends in `Cancelled`.
  void cancel();
  /// Main thread, from the screen's `tick`: posts the next game when its time has come.
  void tick();
  /// Seconds until `tick` has something to do (pacing, server backoff), nullopt when nothing is scheduled.
  std::optional<float> wake_in() const;
  /// A screen is showing this run: when it ends alone nobody else shows a toast.
  void set_watched(bool w) { watched_ = w; }

  Phase phase() const { return phase_; }
  bool finished() const { return phase_ == Phase::Finished || phase_ == Phase::Cancelled || phase_ == Phase::Aborted; }
  bool working() const { return !finished(); }
  const Stats& stats() const { return st_; }
  Abort abort_reason() const { return abort_; }
  const std::vector<ErrLine>& errors() const { return errors_; }
  const std::string& system_name() const { return sys_name_; }
  const std::string& game_name() const { return game_name_; }
  size_t systems_done() const { return sys_done_; }
  size_t systems_total() const { return o_.systems.size(); }
  /// Whole seconds still to wait (server backoff / pacing longer than 2 s), 0 otherwise.
  int wait_seconds() const;
  /// Quota of the account when ScreenScraper told it (0 = unknown).
  int quota_left() const { return quota_left_; }
  uint64_t version() const { return version_; }  // bumps on every visible change

 private:
  Run() = default;
  void bump() { ++version_; }
  void post_prepare(size_t k);
  void post_connect();
  void post_step();
  void on_step(StepResult& r);
  void after_item();
  void finalize(Phase end);
  void flush(bool last);
  void apply_fresh(size_t sys, std::vector<Game>&& fresh);

  Ctx* cx_ = nullptr;
  std::shared_ptr<Services> sv_;
  Options o_;
  std::shared_ptr<Env> env_;
  std::shared_ptr<std::vector<Item>> items_;
  size_t next_ = 0;
  int attempt_ = 0;
  Phase phase_ = Phase::Preparing;
  Phase end_ = Phase::Finished;
  Abort abort_ = Abort::None;
  Stats st_;
  std::vector<ErrLine> errors_;
  std::string sys_name_, game_name_;
  size_t sys_done_ = 0;
  bool inflight_ = false, cancel_ = false, watched_ = false, flushing_ = false;
  TimePoint due_{};
  int interval_ms_ = 1000, base_interval_ms_ = 1000;  // pacing now / without the backoff of "too many requests"
  int net_fail_streak_ = 0;
  int quota_left_ = 0;
  std::vector<GameFieldEdit> pending_;        // edits not yet written
  std::vector<uint32_t> pending_sys_;         // system of each pending edit
  std::vector<uint32_t> touched_;             // systems written to (their lists are refreshed at the end)
  size_t since_flush_ = 0;
  TimePoint last_flush_{};
  uint64_t version_ = 0;
};

}  // namespace zf::scraper
