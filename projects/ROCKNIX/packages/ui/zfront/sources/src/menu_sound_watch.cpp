// Device-loop service "sound-watch": shows the volume OSD when audio.volume changes in system.cfg behind zfront's back.
//
// Who changes the volume on the console: the hardware keys (input_sense / adckeys.sh -> `volume up|down` -> pactl +
// set_setting audio.volume), headphone_sense (`volume restore`). ES reacts because libpulse notifies it of every sink
// change. zfront links no libpulse; system.cfg is a regular file, so inotify works on it (it would not on sysfs) and costs
// nothing while it is not written. `set_setting` is "delete the line, then append it": a read in between sees no value, which
// is never taken for a change (`VolumeTracker`). Changes caused by our own `volume N` runs are ignored (`quiet()`).
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "menu_sound.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "util.hpp"

namespace zf {
namespace sound {

bool FileWatch::start(const std::string& dir, const std::string& name, std::function<void()> on_change, std::string& err, int settle_ms) {
  if (th_.joinable()) return true;
  ino_ = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (ino_ < 0) {
    err = std::string("inotify_init: ") + std::strerror(errno);
    return false;
  }
  if (::inotify_add_watch(ino_, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
    err = std::string("inotify_add_watch ") + dir + ": " + std::strerror(errno);
    ::close(ino_);
    ino_ = -1;
    return false;
  }
  ev_ = ::eventfd(0, EFD_CLOEXEC);
  if (ev_ < 0) {
    err = std::string("eventfd: ") + std::strerror(errno);
    ::close(ino_);
    ino_ = -1;
    return false;
  }
  const int ino = ino_, ev = ev_;
  th_ = std::thread([ino, ev, name, on_change = std::move(on_change), settle_ms] {
    // Reads every queued event; true when one of them concerns `name` (or the queue overflowed).
    auto drain = [&]() {
      bool hit = false;
      alignas(struct inotify_event) char buf[4096];
      for (;;) {
        ssize_t n = ::read(ino, buf, sizeof buf);
        if (n <= 0) break;
        for (char* p = buf; p < buf + n;) {
          const struct inotify_event* e = reinterpret_cast<const struct inotify_event*>(p);
          if ((e->mask & IN_Q_OVERFLOW) || (e->len && name == e->name)) hit = true;
          p += sizeof(struct inotify_event) + e->len;
        }
      }
      return hit;
    };
    for (;;) {
      struct pollfd fds[2] = {{ino, POLLIN, 0}, {ev, POLLIN, 0}};
      int r = ::poll(fds, 2, -1);
      if (r < 0 && errno == EINTR) continue;
      if (r < 0 || (fds[1].revents & POLLIN)) return;
      if (!(fds[0].revents & POLLIN)) continue;
      bool hit = drain();
      // wait until the file is quiet: one `set_setting` is a rename plus an append
      while (hit) {
        struct pollfd w[2] = {{ino, POLLIN, 0}, {ev, POLLIN, 0}};
        int q = ::poll(w, 2, settle_ms);
        if (q < 0 && errno == EINTR) continue;
        if (q < 0 || (w[1].revents & POLLIN)) return;
        if (q == 0) {
          on_change();
          break;
        }
        drain();
      }
    }
  });
  return true;
}

void FileWatch::stop() {
  if (!th_.joinable()) return;
  uint64_t one = 1;
  ssize_t w = ::write(ev_, &one, sizeof one);
  (void)w;
  th_.join();
  ::close(ino_);
  ::close(ev_);
  ino_ = ev_ = -1;
}

namespace {

FileWatch& watch() {
  static FileWatch w;
  return w;
}

std::string read_text(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace
}  // namespace sound

ZF_REGISTER_SERVICE("sound-watch", [](App& app) {
  const std::string path = app.cx.conf.path();
  size_t slash = path.rfind('/');
  if (path.empty() || slash == std::string::npos || slash + 1 >= path.size()) return;
  static sound::VolumeTracker tracker;  // touched by the watcher thread only (after the seed)
  tracker.seed(sound::parse_volume(sound::read_text(path)));
  Ctx* cx = &app.cx;  // the App outlives the watcher: it is stopped in oscontract::finish()
  Services* sv = app.cx.sv.get();
  std::string err;
  bool ok = sound::watch().start(path.substr(0, slash), path.substr(slash + 1), [path, cx, sv] {
    int v = -1;
    // The main thread is blocked inside a game, and `jobs` is FIFO behind whatever else is queued, so "how old is the change"
    // cannot tell a stale event from a late one: ask whether a game was running when it happened.
    sound::Seen s = sound::decide(tracker, sound::read_text(path), sound::quiet(), oscontract::game_running(), v);
    if (s == sound::Seen::Nothing) return;
    bool osd = s == sound::Seen::Osd;
    sv->jobs.post(nullptr, [cx, v, osd] { sound::external_volume(*cx, v, osd); });
  }, err);
  if (!ok) {
    std::fprintf(stderr, "zfront: sound-watch not started: %s\n", err.c_str());
    return;
  }
  oscontract::on_finish([] { sound::watch().stop(); });
});

}  // namespace zf
