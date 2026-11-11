#include "covers.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "services.hpp"

namespace zf {
namespace covers {

namespace {

constexpr size_t kMaxFailed = 64;
constexpr double kFailedTtl = 60.0;  // seconds: a cover that failed (file still being written, scraper) is tried again after this

double (*g_now)() = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };

// main thread only: path -> when it failed
std::unordered_map<std::string, double>& failed_set() {
  static std::unordered_map<std::string, double> s;
  return s;
}
using Gen = std::shared_ptr<std::atomic<uint64_t>>;
// requests started and not finished: key -> generation the job must still match to run
std::unordered_map<std::string, Gen>& in_flight_map() {
  static std::unordered_map<std::string, Gen> m;
  return m;
}
uint64_t g_gen = 0;                 // last generation handed out (main thread)
std::atomic<uint64_t> g_latest{0};  // the same, readable by the worker

struct Result {
  Img img;
  bool skipped = false, ok = false;
};

}  // namespace

bool ready(const Assets& a, const std::string& path, uint32_t max_w, uint32_t max_h, ImgId& id) { return a.find_file_fit(path, max_w, max_h, id); }

bool failed(const std::string& path) {
  auto it = failed_set().find(path);
  if (it == failed_set().end()) return false;
  if (g_now() - it->second >= kFailedTtl) {  // expired: ask again
    failed_set().erase(it);
    return false;
  }
  return true;
}

void mark_failed(const std::string& path) {
  if (failed_set().size() >= kMaxFailed) failed_set().clear();
  failed_set()[path] = g_now();
}

void cancel_pending() {
  g_latest.store(++g_gen);  // every job that has not started sees a newer request and skips its decode
}

void clear_failed() {
  failed_set().clear();
  failed_set().rehash(0);
}

void set_clock_for_test(double (*now)()) {
  g_now = now ? now : [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
}

size_t in_flight() { return in_flight_map().size(); }

bool request(Assets& a, Jobs& jobs, const std::string& path, uint32_t max_w, uint32_t max_h) {
  ImgId id;
  if (a.find_file_fit(path, max_w, max_h, id)) return true;
  if (failed(path)) return false;
  std::string key = path + "\n" + std::to_string(max_w) + "x" + std::to_string(max_h);
  uint64_t gen = ++g_gen;
  g_latest.store(gen);
  auto it = in_flight_map().find(key);
  if (it != in_flight_map().end()) {  // already queued or decoding: it becomes the wanted one again
    it->second->store(gen);
    return false;
  }
  Gen mine = std::make_shared<std::atomic<uint64_t>>(gen);
  in_flight_map()[key] = mine;
  auto res = std::make_shared<Result>();
  Assets* assets = &a;  // `Ctx` owns both the assets and the jobs: `done` never runs after the assets are gone
  jobs.post(
      [=] {
        if (g_latest.load() != mine->load()) {  // a newer request superseded this one before it started
          res->skipped = true;
          return;
        }
        res->ok = Assets::decode_fit(path, max_w, max_h, res->img);
      },
      [=] {
        in_flight_map().erase(key);
        if (res->skipped) return;
        if (res->ok) assets->add_file_fit(path, max_w, max_h, std::move(res->img));
        else mark_failed(path);
      });
  return false;
}

}  // namespace covers
}  // namespace zf
