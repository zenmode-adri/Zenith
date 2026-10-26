#include "covers.hpp"

#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "services.hpp"

namespace zf {
namespace covers {

namespace {

constexpr size_t kMaxFailed = 64;

// main thread only
std::unordered_set<std::string>& failed_set() {
  static std::unordered_set<std::string> s;
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

bool failed(const std::string& path) { return failed_set().count(path) != 0; }

void mark_failed(const std::string& path) {
  if (failed_set().size() >= kMaxFailed) failed_set().clear();
  failed_set().insert(path);
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
