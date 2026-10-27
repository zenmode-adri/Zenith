#include "scan.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>

namespace zf {

Exts::Exts(const std::vector<std::string>& list) {
  for (const auto& e : list) {
    size_t i = 0;
    while (i < e.size() && e[i] == '.') ++i;
    if (i == e.size()) continue;
    std::string l = e.substr(i);
    for (char& c : l)
      if (c >= 'A' && c <= 'Z') c = char(c + 32);
    list_.push_back(std::move(l));
  }
}

bool Exts::matches(std::string_view name) const {
  size_t d = name.rfind('.');
  if (d == std::string_view::npos || d == 0) return false;  // ".nes" alone is a hidden file, not an extension
  std::string_view ext = name.substr(d + 1);
  for (const auto& e : list_) {
    if (e.size() != ext.size()) continue;
    size_t i = 0;
    for (; i < e.size(); ++i) {
      char c = ext[i];
      if (c >= 'A' && c <= 'Z') c = char(c + 32);
      if (c != e[i]) break;
    }
    if (i == e.size()) return true;
  }
  return false;
}

namespace {

// Walks `d` (takes ownership) and its sub-folders down to `depth` more levels. The visitor `v` gets
// `enter(name)` / `leave()` around each sub-folder and `file(name)` -> bool (false stops the whole walk) for every file
// whose extension matches. Returns false when stopped. No stat except for symlinks / unknown types; sub-folders are
// opened relative to their parent's fd, so no path is built.
template <class V>
bool walk(DIR* d, int depth, const Exts& exts, V& v) {
  int fd = ::dirfd(d);
  bool go = true;
  while (go) {
    dirent* e = ::readdir(d);
    if (!e) break;
    const char* n = e->d_name;
    if (n[0] == '.') continue;  // hidden, "." and ".."
    bool is_d, is_f;
    switch (e->d_type) {
      case DT_REG: is_d = false, is_f = true; break;
      case DT_DIR: is_d = true, is_f = false; break;
      case DT_LNK:
      case DT_UNKNOWN: {
        struct stat st;
        if (::fstatat(fd, n, &st, 0) != 0) continue;  // follows symlinks; broken ones are skipped
        is_d = S_ISDIR(st.st_mode);
        is_f = S_ISREG(st.st_mode);
        break;
      }
      default: continue;
    }
    if (is_d) {
      if (depth <= 0) continue;
      int sfd = ::openat(fd, n, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (sfd < 0) continue;
      DIR* sd = ::fdopendir(sfd);
      if (!sd) {
        ::close(sfd);
        continue;
      }
      v.enter(n);
      go = walk(sd, depth - 1, exts, v);
      v.leave();
    } else if (is_f && exts.matches(n)) {
      go = v.file(n);
    }
  }
  ::closedir(d);
  return go;
}

template <class V>
bool walk_root(const std::string& dir, const Exts& exts, V& v) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return true;
  DIR* d = ::fdopendir(fd);
  if (!d) {
    ::close(fd);
    return true;
  }
  return walk(d, kMaxDepth, exts, v);
}

struct Counter {
  size_t n = 0;
  void enter(const char*) {}
  void leave() {}
  bool file(const char*) { return ++n, true; }
};

struct Finder {
  bool found = false;
  void enter(const char*) {}
  void leave() {}
  bool file(const char*) { return found = true, false; }
};

// Keeps the current folder path as a string that grows/shrinks with enter/leave.
struct PathStack {
  std::string cur;
  std::vector<size_t> marks;
  void push(const char* n) {
    marks.push_back(cur.size());
    if (cur.empty() || cur.back() != '/') cur += '/';
    cur += n;
  }
  void pop() {
    cur.resize(marks.back());
    marks.pop_back();
  }
};

struct Lister : PathStack {
  std::vector<std::string>* out;
  void enter(const char* n) { push(n); }
  void leave() { pop(); }
  bool file(const char* n) {
    std::string p = cur;
    if (p.empty() || p.back() != '/') p += '/';
    p += n;
    out->push_back(std::move(p));
    return true;
  }
};

struct Collector : PathStack {
  std::vector<std::string>* dirs;
  std::string* names;
  std::vector<uint32_t>* flat;  // (name start, dir index) pairs
  std::vector<int64_t> idx{-1};  // dir index of each open folder, -1 = no match there yet
  void enter(const char* n) {
    push(n);
    idx.push_back(-1);
  }
  void leave() {
    pop();
    idx.pop_back();
  }
  bool file(const char* n) {
    if (idx.back() < 0) {  // first match of this folder: one path string per folder, not per game
      idx.back() = int64_t(dirs->size());
      dirs->push_back(cur);
    }
    flat->push_back(uint32_t(names->size()));
    flat->push_back(uint32_t(idx.back()));
    names->append(n);
    return true;
  }
};

}  // namespace

bool scan_has_game(const std::string& dir, const Exts& exts) {
  Finder f;
  walk_root(dir, exts, f);
  return f.found;
}

size_t scan_count_games(const std::string& dir, const Exts& exts) {
  Counter c;
  walk_root(dir, exts, c);
  return c.n;
}

std::vector<std::string> scan_list_games(const std::string& dir, const Exts& exts) {
  std::vector<std::string> out;
  Lister l;
  l.cur = dir;
  l.out = &out;
  walk_root(dir, exts, l);
  return out;
}

GameList GameList::scan(const std::string& dir, const Exts& exts) {
  GameList g;
  std::vector<uint32_t> flat;
  Collector c;
  c.cur = dir;
  c.dirs = &g.dirs_;
  c.names = &g.names_;
  c.flat = &flat;
  walk_root(dir, exts, c);
  g.items_.resize(flat.size() / 2);
  for (size_t i = 0; i < g.items_.size(); ++i) g.items_[i] = {flat[2 * i], flat[2 * i + 1]};
  g.names_.shrink_to_fit();
  g.dirs_.shrink_to_fit();
  return g;
}

std::string_view GameList::file_name(size_t i) const {
  size_t s = items_[i].start, e = i + 1 < items_.size() ? items_[i + 1].start : names_.size();
  return std::string_view(names_).substr(s, e - s);
}

std::string GameList::path(size_t i) const {
  const std::string& d = dirs_[items_[i].dir];
  std::string_view n = file_name(i);
  std::string p;
  p.reserve(d.size() + 1 + n.size());
  p = d;
  if (p.empty() || p.back() != '/') p += '/';
  p.append(n);
  return p;
}

// ---- counting

std::vector<size_t> scan_count_many(const std::vector<CountJob>& jobs, int threads) {
  std::vector<size_t> res(jobs.size(), 0);
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (size_t i; (i = next.fetch_add(1)) < jobs.size();) res[i] = scan_count_games(jobs[i].dir, jobs[i].exts);
  };
  size_t extra = threads > 1 ? std::min<size_t>(size_t(threads) - 1, jobs.size() > 1 ? jobs.size() - 1 : 0) : 0;
  std::vector<std::thread> th;
  for (size_t t = 0; t < extra; ++t) th.emplace_back(work);
  work();  // the caller works too
  for (auto& t : th) t.join();
  return res;
}

CountWorker::CountWorker(std::vector<CountJob> jobs, int threads, size_t first, std::function<void()> wake)
    : jobs_(std::move(jobs)), first_(jobs_.empty() ? 0 : first % jobs_.size()), wake_(std::move(wake)) {
  if (jobs_.empty()) return;
  int n = std::max(1, std::min(threads, 2));  // never more than 2: the drawing thread keeps its core
  for (int i = 0; i < n; ++i) threads_.emplace_back([this] { run(); });
}

CountWorker::~CountWorker() {
  stop_ = true;
  for (auto& t : threads_) t.join();
}

void CountWorker::set_wake(std::function<void()> wake) {
  std::lock_guard<std::mutex> lk(mu_);
  wake_ = std::move(wake);
}

bool CountWorker::drain(std::vector<std::pair<size_t, size_t>>& out) {
  if (!has_results_.exchange(false)) return false;
  std::lock_guard<std::mutex> lk(mu_);
  if (ready_.empty()) return false;
  out.insert(out.end(), ready_.begin(), ready_.end());
  ready_.clear();
  return true;
}

void CountWorker::run() {
  ::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);  // per-thread nice: drawing wins
  for (;;) {
    if (stop_) return;
    size_t k = next_.fetch_add(1);
    if (k >= jobs_.size()) return;
    size_t i = (first_ + k) % jobs_.size();
    size_t n = scan_count_games(jobs_[i].dir, jobs_[i].exts);
    std::function<void()> wake;
    {
      std::lock_guard<std::mutex> lk(mu_);
      bool was_empty = ready_.empty();
      ready_.emplace_back(i, n);
      has_results_ = true;
      if (was_empty) wake = wake_;
    }
    ++finished_;
    if (wake) wake();
  }
}

}  // namespace zf
