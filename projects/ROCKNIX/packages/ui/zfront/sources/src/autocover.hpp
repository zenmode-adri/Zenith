// Automatic covers: when the frontend has been left alone for a while, the scraper runs by itself in the background for the games
// that miss media, and stops the moment anything is pressed. It is the ordinary `scraper::Run` (scraper_run.hpp) started with
// `Options::background`, so there is no second scraper: same sources, same pacing, same "recently scraped" stamp, same cancel.
//
// Off by default (`ZfAutoCovers`, LIBRARY > SCRAPER). Runs only when ALL of this holds (`gate`): the setting is on, nothing was
// pressed for `kIdle`, no game runs and no other long job holds `oscontract::Busy`, the network is up, the battery is at
// `kMinBattery` % or charging, the source is usable (ScreenScraper has its developer key in this build, IGDB has the user's client
// id and secret) and some system is left to scrape. A game is not looked up again before a day has passed (the larger of the
// user's "ignore recently scraped games" and 1 day).
//
// Quiet: no screen, no dialog. Only when a run ends with N > 0 games that got pictures a toast says "N COVERS ADDED". Pictures that
// are already there are never replaced (missing-media filter, no overwrite).
//
// Main thread only. The device loop (sdlr.cpp) calls `input` on every key / button, `game_started` before a game, `poll` once per
// iteration and wakes at `deadline`.
#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include "app.hpp"
#include "scraper_run.hpp"
#include "util.hpp"

namespace zf::autocover {

constexpr const char* kKeyEnabled = "ZfAutoCovers";  // zfront's own setting (ES ignores it); default off
constexpr std::chrono::seconds kIdle{120};           // no input for this long before it starts
constexpr std::chrono::seconds kRecheck{30};         // how often a blocked gate is looked at again
constexpr int kMinBattery = 30;                      // percent, unless charging

/// What stops a start, in the order it is checked.
enum class Block : uint8_t { None, Off, Idle, Game, Busy, Network, Battery, Source, NoSystems };

struct Conditions {
  bool enabled = false;
  std::chrono::milliseconds idle{0};  // time since the last input
  bool game_running = false;          // a game is running
  bool busy = false;                  // another long job holds oscontract::Busy
  bool network = true;
  std::optional<Battery> battery;     // none: no battery, never blocks
  bool source_usable = true;
  bool systems = true;                // at least one system to scrape
};

/// Pure decision: `Block::None` when a run may start.
Block gate(const Conditions& c);

/// The chosen source can be used right now: ScreenScraper needs the developer key of this build, IGDB the user's client id and
/// secret, ArcadeDB nothing.
bool source_usable(const Ctx& cx);

/// The options of an automatic run: the menu's systems, missing media only, nothing overwritten, pictures only, at most one look
/// per game per day, no `Busy`.
scraper::Options make_options(Ctx& cx);

class Controller {
 public:
  explicit Controller(Ctx& cx, TimePoint now = Clock::now());
  ~Controller();
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  /// Probes and tuning, replaceable by tests (the defaults ask `oscontract`).
  std::function<bool()> game_running;
  std::function<bool()> busy;
  std::function<void(scraper::Options&)> tune;  // after `make_options`, before the run is created

  /// Any key, button or pointer event: stops a run at once and restarts the idle count.
  void input(TimePoint now);
  /// A game is about to start: stops a run (its answers wait in the job queue until the game ends).
  void game_started();
  /// Once per loop iteration. True when a toast was pushed (redraw).
  bool poll(TimePoint now);
  /// When `poll` has something to do again (nullopt: nothing, the loop may sleep).
  std::optional<TimePoint> deadline() const;

  bool running() const { return run_ != nullptr; }
  const scraper::Run* run() const { return run_.get(); }
  scraper::Phase last_phase() const { return last_phase_; }
  Block last_block() const { return block_; }
  size_t runs_started() const { return started_; }

 private:
  Conditions conditions(TimePoint now) const;
  bool finish();

  Ctx& cx_;
  std::shared_ptr<scraper::Run> run_;
  TimePoint last_input_, next_check_, run_started_{};
  bool armed_ = true;  // false after a run ended by itself: the next one waits for an input and a new idle period
  bool played_ = false;  // game_started happened: the next poll counts it as an input
  Block block_ = Block::None;
  scraper::Phase last_phase_ = scraper::Phase::Finished;
  size_t started_ = 0;
};

}  // namespace zf::autocover
