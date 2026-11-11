// PERFORMANCE SUGGESTIONS: while a game runs a helper thread samples the GPU and RAM frequencies, the GPU load (when the driver exposes it)
// and the CPU use of the emulator. After a long enough session the series is classified; a game that looks "governor limited" (the
// automatic GPU governor kept the GPU at its lowest steps while the GPU was the bottleneck: measured on the R36S with Mario Tennis,
// 19 -> 25 fps with GPU 600 + RAM 666) is only SUGGESTED to use PERFORMANCE MODE: a toast when the game ends and a row in its options.
// Nothing is ever enabled on its own: the row writes the same key as the PERFORMANCE MODE row (`<system>["<rom file>"].perfmode=1`).
//
// Heuristic (conservative, see `classify`):
//  * the first `warmup_s` seconds are ignored (loading), a session under `min_secs` is ignored, perfmode already on is ignored;
//  * with GPU load data a sample is "limited" when the GPU sat in the lower third of its steps while the load was >= `busy_hi`;
//  * without it a sample is limited only when the hottest emulator THREAD was saturated (>= `cpu_sat`) AND the GPU sat low AND the RAM sat low
//    (when its data exists): the same picture a CPU-bound game gives, so this branch asks for more samples (`share_nobusy`) and the toast
//    says "may";
//  * a game whose GPU load is low while a thread is saturated is "CPU bound" (a suggestion would not help) and an idle one is ignored.
// The sources are sysfs files read through `Paths`, so tests point them at a fake tree. Whether the GPU load file exists on the console is a
// SUPPOSITION (libmali: /sys/kernel/debug/mali0/dvfs_utilization may be unreadable; panfrost has none); the code copes with its absence.
#pragma once
#include <string>
#include <vector>

#include "menu.hpp"

namespace zf {
struct Ctx;
namespace autoperf {

struct Paths {
  std::string gpu_dir = "/sys/class/devfreq/ff400000.gpu";
  std::string dmc_dir = "/sys/class/devfreq/dmc";
  std::string proc_root = "/proc";
  /// Files that may hold the GPU load; the first readable one wins. kbase's debugfs file holds cumulative counters ("busy_time N idle_time M",
  /// turned into a percentage between two samples); anything else is read as a percentage (first integer 0..100: "45", "45@600000000Hz").
  /// Mainline devfreq probably has no `load` file for the Mali: with panfrost this is -1 and the weaker CPU rule decides.
  std::vector<std::string> busy_files = {"/sys/class/devfreq/ff400000.gpu/load", "/sys/kernel/debug/mali0/dvfs_utilization",
                                         "/sys/devices/platform/ff400000.gpu/devfreq/ff400000.gpu/load"};
  long clk_tck = 0;  // 0: sysconf(_SC_CLK_TCK)
};

struct Sample {
  double t_s = 0;
  double gpu_mhz = -1, dmc_mhz = -1;  // -1: unreadable
  double gpu_busy = -1;               // percent, -1: no data
  double cpu_hot = -1;                // percent of one core used by the busiest thread of the emulator's process tree, -1: unknown
  double cpu_total = -1;              // percent of one core used by the whole tree
};

struct Series {
  std::vector<Sample> samples;
  double gpu_min = 0, gpu_max = 0, dmc_min = 0, dmc_max = 0;  // MHz, lowest / highest step (0: unknown)
  double secs = 0;                                            // length of the session
};

/// Reads one sample. The CPU figures need a previous call (the first one gives -1).
class Sampler {
 public:
  explicit Sampler(Paths p);
  Sample take(int root_pid, double now_s);
  void fill_ranges(Series& s) const;

 private:
  Paths p_;
  long tck_ = 100;
  double prev_t_ = -1;
  long long prev_busy_ = 0, prev_idle_ = 0;
  bool have_counters_ = false;
  std::vector<std::pair<int, unsigned long long>> prev_;  // (tid, ticks), sorted by tid
};

enum class Verdict { TooShort, PerfmodeOn, NoData, Idle, CpuBound, GovernorLimited, Unclear };
const char* verdict_name(Verdict v);

struct Params {
  double min_secs = 120, warmup_s = 20;
  size_t min_samples = 30;
  double low_frac = 0.34;  // "low" = within this fraction of the step range above the lowest step
  double busy_hi = 70, busy_lo = 40, cpu_sat = 90, idle_cpu = 35;
  double share_busy = 0.6, share_nobusy = 0.75, share_other = 0.7;
};
Verdict classify(const Series& s, bool perfmode_on, const Params& p = Params());

/// Samples a running process tree every `interval_s` on its own thread.
class Monitor {
 public:
  Monitor(Paths p, double interval_s);
  ~Monitor();
  Monitor(const Monitor&) = delete;
  Monitor& operator=(const Monitor&) = delete;
  void start(int pid);
  /// Stops and joins; the series so far (empty when never started).
  Series stop();

 private:
  struct Impl;
  Impl* i_;
};

// ---- the hook of launch::run: `arm` before a launch, the run itself calls on_spawn / on_exit --------------------------------------
void arm(const Paths& p, double interval_s = 2.0);
void disarm();
bool armed();
void on_spawn(int pid);  // no-op when not armed
void on_exit();          // stops the thread and keeps the series for `take_series`
bool take_series(Series& out);

// ---- suggestions kept between sessions ------------------------------------------------------------------------------------
enum class State { None, Suggest, Dismissed, Accepted };
/// `/storage/.config/zfront/autoperf.tsv`; `ZF_AUTOPERF` replaces it (tests). Line: state TAB system TAB rom path.
std::string store_path();
State get_state(const std::string& file, const std::string& system, const std::string& rom);
/// None removes the game's line. False when the file cannot be written.
bool set_state(const std::string& file, const std::string& system, const std::string& rom, State st);

/// A finished session: True when a NEW suggestion was recorded (the caller shows the toast). `perfmode_on` as for `classify`.
bool record_result(const std::string& file, const std::string& system, const std::string& rom, Verdict v);

// ---- with the settings ----------------------------------------------------------------------------------------------------
inline constexpr const char* kKey = "zfront.perf_suggestions";  // "0" = off; absent = on (it only ever suggests)
bool enabled(const Ctx& cx);
/// The effective PERFORMANCE MODE of a game: its own value, else its system's, else the global one; "1" is on.
bool perfmode_on(const Ctx& cx, const std::string& system, const std::string& rom_file);
/// The game's emulator offers PERFORMANCE MODE (es_features.cfg).
bool supported(const Ctx& cx, const std::string& system, const std::string& rom_file);
/// Before a launch: arms the sampler when suggestions are on, the game's emulator offers perfmode and it is not on already.
void begin_session(const Ctx& cx, const std::string& system, const std::string& rom_path);
/// `end_session` after the series is in hand: classifies it and records the result; the toast text or "".
std::string conclude(Ctx& cx, const std::string& system, const std::string& rom_path, const Series& s);
/// After the game (any exit code): classifies the series taken by the run and records the result. Returns the toast text, "" for none.
std::string end_session(Ctx& cx, const std::string& system, const std::string& rom_path, unsigned long secs, int exit_code);
/// A suggestion is waiting for this game (shown in its options).
bool pending(const Ctx& cx, const std::string& system, const std::string& rom_path);
/// The rows of the game's options when a suggestion is waiting: enable PERFORMANCE MODE, or stop suggesting it for this game.
void game_entries(MenuBuilder& m, Ctx& cx, const std::string& system, const std::string& rom_path);

}  // namespace autoperf
}  // namespace zf
