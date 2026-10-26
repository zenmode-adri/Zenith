// MODULE `scan` (owner: scan agent). Fast ROM-folder scanning that never blocks the first frame.
//
// Steps: `scan_has_game` (stops at the first match: decides which systems exist), `scan_count_games` (for the
// carousel's "N GAMES", run in the background by `CountWorker`) and `GameList` (on demand, when a system is opened).
// Rules (same as ES and as the Rust prototype): hidden names (leading dot) skipped, sub-folders followed up to
// `kMaxDepth`, extension compared case-insensitively (the part after the last dot; a leading dot is not an
// extension), symlinks to folders followed, no `stat` per file (`d_type`; only symlinks/unknown types are resolved).
//
// Folders are walked with `openat`/`fdopendir`, so no path string is built per entry.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace zf {

constexpr int kMaxDepth = 2;
/// `SystemItem::games` / `System::count` value while the background count has not finished (nothing is drawn).
constexpr uint32_t kCountPending = 0xFFFFFFFFu;

/// Lower-case extensions (with or without the leading dot in the input; stored without it).
class Exts {
 public:
  Exts() = default;
  explicit Exts(const std::vector<std::string>& list);
  /// No allocation. `.nes` alone is a hidden file, not an extension.
  bool matches(std::string_view file_name) const;
  bool empty() const { return list_.empty(); }

 private:
  std::vector<std::string> list_;  // lower case, without the dot
};

/// True as soon as one game exists (the walk stops at the first match).
bool scan_has_game(const std::string& dir, const Exts& exts);
size_t scan_count_games(const std::string& dir, const Exts& exts);
/// One string per game (convenient, heavier than `GameList`). Unsorted.
std::vector<std::string> scan_list_games(const std::string& dir, const Exts& exts);

/// Compact list of the games of a folder tree: one buffer with every file name and one string per FOLDER.
/// 8 bytes per game plus the name bytes. Unsorted (readdir order).
class GameList {
 public:
  static GameList scan(const std::string& dir, const Exts& exts);
  size_t size() const { return items_.size(); }
  bool empty() const { return items_.empty(); }
  std::string_view file_name(size_t i) const;
  const std::string& dir(size_t i) const { return dirs_[items_[i].dir]; }
  /// `dir/file_name`, built on demand.
  std::string path(size_t i) const;
  size_t folders() const { return dirs_.size(); }
  size_t heap_bytes() const { return names_.capacity() + items_.capacity() * sizeof(Item) + dirs_.capacity() * sizeof(std::string); }

 private:
  struct Item {
    uint32_t start;  // offset in names_; the name ends where the next one starts
    uint32_t dir;
  };
  std::vector<std::string> dirs_;
  std::string names_;
  std::vector<Item> items_;
};

struct CountJob {
  std::string dir;
  Exts exts;
};
/// Counts of several systems with up to `threads` workers (<= 1: current thread). Result in `jobs` order.
std::vector<size_t> scan_count_many(const std::vector<CountJob>& jobs, int threads);

/// Background counter for the carousel. Starts up to 2 low-priority threads that count the jobs (`first` before the
/// others, so the system under the cursor is ready first). Results are queued; `drain` hands them to the main thread
/// and `set_wake` registers a thread-safe callback that interrupts the event loop's sleep.
class CountWorker {
 public:
  CountWorker(std::vector<CountJob> jobs, int threads, size_t first = 0);
  ~CountWorker();  // stops after the folder in progress and joins
  CountWorker(const CountWorker&) = delete;
  CountWorker& operator=(const CountWorker&) = delete;

  void set_wake(std::function<void()> wake);
  /// Moves finished (job index, count) pairs into `out`; true if any.
  bool drain(std::vector<std::pair<size_t, size_t>>& out);
  bool done() const { return finished_.load() >= jobs_.size(); }

 private:
  void run();

  std::vector<CountJob> jobs_;
  size_t first_;
  std::atomic<size_t> next_{0};
  std::atomic<size_t> finished_{0};
  std::atomic<bool> stop_{false};
  std::atomic<bool> has_results_{false};
  std::mutex mu_;
  std::vector<std::pair<size_t, size_t>> ready_;
  std::function<void()> wake_;
  std::vector<std::thread> threads_;
};

}  // namespace zf
