#include "video.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <thread>

namespace zf {
// The music module is another branch: a weak symbol, so this builds and links without it and ducks the music once it is there.
namespace music {
void duck(bool on) __attribute__((weak));
}
namespace video {

// ---- geometry -------------------------------------------------------------------------------------------------------------

void fit_dims(uint32_t sw, uint32_t sh, uint32_t bw, uint32_t bh, bool cover, uint32_t& ow, uint32_t& oh) {
  if (sw == 0 || sh == 0 || bw == 0 || bh == 0) {
    ow = std::max<uint32_t>(sw, 2);
    oh = std::max<uint32_t>(sh, 2);
    return;
  }
  const double sx = double(bw) / double(sw), sy = double(bh) / double(sh);
  double s = cover ? std::max(sx, sy) : std::min(sx, sy);
  if (s > 1.0) s = 1.0;  // never decode bigger than the file: the GPU scales up
  ow = std::max<uint32_t>(2, uint32_t(std::lround(double(sw) * s)));
  oh = std::max<uint32_t>(2, uint32_t(std::lround(double(sh) * s)));
}

void place(float fw, float fh, float bx, float by, float bw, float bh, bool cover, float& x, float& y, float& w, float& h) {
  if (fw <= 0 || fh <= 0) {
    x = bx, y = by, w = bw, h = bh;
    return;
  }
  const float sx = bw / fw, sy = bh / fh;
  const float s = cover ? std::max(sx, sy) : std::min(sx, sy);
  w = fw * s;
  h = fh * s;
  x = bx + (bw - w) / 2.0f;
  y = by + (bh - h) / 2.0f;
}

// ---- FrameStore -----------------------------------------------------------------------------------------------------------

void FrameStore::configure(uint32_t w, uint32_t h) {
  std::lock_guard<std::mutex> g(m_);
  for (auto& b : buf_) b = std::make_shared<std::vector<uint8_t>>(size_t(w) * h * 4);
  w_ = w;
  h_ = h;
  writing_ = ready_ = held_ = -1;
  fresh_ = false;
  ready_flag_ = false;
}

uint8_t* FrameStore::begin_write() {
  std::lock_guard<std::mutex> g(m_);
  if (!buf_[0]) return nullptr;
  int idx = 0;
  while (idx < 2 && (idx == ready_ || idx == held_)) ++idx;  // three buffers, two excluded: one is always left
  writing_ = idx;
  return buf_[idx]->data();
}

void FrameStore::publish() {
  {
    std::lock_guard<std::mutex> g(m_);
    if (writing_ < 0) return;
    ready_ = writing_;
    writing_ = -1;
    fresh_ = true;
    ready_flag_ = true;
  }
  if (!pending_.exchange(true) && on_frame_) on_frame_();  // one wake-up per frame the loop has not seen yet
}

bool FrameStore::acquire(Frame& f) {
  std::lock_guard<std::mutex> g(m_);
  if (ready_ < 0) return false;
  held_ = ready_;
  f.keep = buf_[held_];
  f.px = buf_[held_]->data();
  f.w = w_;
  f.h = h_;
  const bool fresh = fresh_;
  fresh_ = false;
  return fresh;
}

void FrameStore::reset(uint32_t session) {
  std::lock_guard<std::mutex> g(m_);
  writing_ = ready_ = held_ = -1;
  fresh_ = false;
  ready_flag_ = false;
  pending_ = false;
  session_ = session;
}

void FrameStore::dims(uint32_t& w, uint32_t& h) const {
  std::lock_guard<std::mutex> g(m_);
  w = w_;
  h = h_;
}

// ---- the player -----------------------------------------------------------------------------------------------------------

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

struct Mgr {
  std::mutex m;
  std::condition_variable cv;
  std::thread th;
  bool threaded = true, quit = false, inited = false;
  // the command (written by the main thread under `m`, taken by the control thread)
  bool have_cmd = false, cmd_play = false;
  Request cmd_req;
  uint32_t cmd_epoch = 0;
  std::atomic<uint32_t> epoch{0};
  // control thread only
  std::unique_ptr<Engine> eng;
  bool engine_playing = false, engine_loaded = false, end_reported = false;
  Request cur;
  FrameStore store;
  std::atomic<bool> unavail{false}, ended{false}, failed{false};
  std::function<void()> wake;
  // main thread only
  bool want = false;
  Request want_req;
  bool ducked = false;
  std::function<void(bool)> duck;

  ~Mgr() {  // at exit: never leave a joinable thread behind (std::terminate), nor a player running
    {
      std::lock_guard<std::mutex> g(m);
      quit = true;
    }
    cv.notify_all();
    if (th.joinable()) th.join();
    if (engine_playing && eng) eng->stop();
    if (engine_loaded && eng) eng->release();
  }
  void notify_wake() {
    std::function<void()> w;
    {
      std::lock_guard<std::mutex> g(m);
      w = wake;
    }
    if (w) w();
  }
  void set_duck(bool on) {
    if (on == ducked) return;
    ducked = on;
    if (duck) duck(on);
    else if (music::duck) music::duck(on);  // weak: null when the music module is not linked
  }
  bool ensure_engine() {
    if (!eng) eng = make_vlc_engine();
    if (!engine_loaded) {
      if (!eng->usable()) return false;
      engine_loaded = true;
    }
    return true;
  }
  void apply() {
    Request r;
    bool play;
    uint32_t ep;
    {
      std::lock_guard<std::mutex> g(m);
      if (!have_cmd) return;
      r = cmd_req;
      play = cmd_play;
      ep = cmd_epoch;
      have_cmd = false;
    }
    if (engine_playing) {
      eng->stop();  // when it returns no VLC thread writes into the store any more
      engine_playing = false;
    }
    store.reset(ep);
    end_reported = false;
    if (!play) return;
    if (!ensure_engine()) {
      unavail = true;
      failed = true;
      notify_wake();
      return;
    }
    if (!eng->start(r, store)) {
      failed = true;
      notify_wake();
      return;
    }
    engine_playing = true;
    cur = r;
  }
  void poll() {
    if (!engine_playing || end_reported) return;
    switch (eng->state()) {
      case Engine::State::Playing: break;
      case Engine::State::Ended:
        if (cur.loop) {
          if (!eng->restart()) {
            failed = true;
            notify_wake();
          }
        } else {
          end_reported = true;
          ended = true;
          notify_wake();
        }
        break;
      case Engine::State::Error:
        eng->stop();
        engine_playing = false;
        failed = true;
        notify_wake();
        break;
    }
  }
  void run() {
    std::unique_lock<std::mutex> lk(m);
    for (;;) {
      if (quit) return;
      if (have_cmd) {
        lk.unlock();
        apply();
        lk.lock();
        continue;
      }
      auto pred = [this] { return quit || have_cmd; };
      if (engine_playing && !end_reported) {
        if (cv.wait_for(lk, milliseconds(250), pred)) continue;  // the only timer, and only while a video plays
        lk.unlock();
        poll();
        lk.lock();
      } else if (engine_loaded && !engine_playing) {
        if (!cv.wait_for(lk, seconds(20), pred)) {  // idle for 20 s: give libvlc back (its plugins are tens of MB)
          lk.unlock();
          eng->release();
          engine_loaded = false;
          lk.lock();
        }
      } else {
        cv.wait(lk, pred);
      }
    }
  }
  void init() {
    if (inited) return;
    inited = true;
    store.set_on_frame([this] { notify_wake(); });
  }
};

Mgr& G() {
  static Mgr m;
  return m;
}

Box g_box;
uint64_t g_frame_uid = 0;
ImgId g_frame_id = 0;

}  // namespace

bool play(const Request& r) {
  Mgr& s = G();
  s.init();
  if (s.unavail.load()) return false;
  if (r.path.empty() || r.box_w == 0 || r.box_h == 0) return false;
  if (s.want && s.want_req.same(r)) return true;
  {
    std::lock_guard<std::mutex> g(s.m);
    s.cmd_req = r;
    s.cmd_play = true;
    s.cmd_epoch = s.epoch.fetch_add(1) + 1;
    s.have_cmd = true;
  }
  s.want = true;
  s.want_req = r;
  s.ended = false;
  s.failed = false;
  s.set_duck(!r.mute);
  if (s.threaded) {
    if (!s.th.joinable()) {
      s.quit = false;
      s.th = std::thread([&s] { s.run(); });
    }
    s.cv.notify_one();
  } else {
    s.apply();
  }
  return true;
}

void stop() {
  Mgr& s = G();
  if (!s.want) return;
  {
    std::lock_guard<std::mutex> g(s.m);
    s.cmd_play = false;
    s.cmd_epoch = s.epoch.fetch_add(1) + 1;
    s.have_cmd = true;
  }
  s.want = false;
  s.set_duck(false);
  if (s.threaded) s.cv.notify_one();
  else s.apply();
}

void shutdown() {
  Mgr& s = G();
  {
    std::lock_guard<std::mutex> g(s.m);
    s.quit = true;
  }
  s.cv.notify_all();
  if (s.th.joinable()) s.th.join();
  {
    std::lock_guard<std::mutex> g(s.m);
    s.quit = false;
    s.have_cmd = false;
  }
  if (s.engine_playing && s.eng) s.eng->stop();
  s.engine_playing = false;
  if (s.eng && s.engine_loaded) s.eng->release();
  s.engine_loaded = false;
  s.store.reset(s.epoch.fetch_add(1) + 1);
  s.want = false;
  s.set_duck(false);
  s.ended = false;
  s.failed = false;
}

bool wanted() { return G().want; }
std::string current_path() { return G().want ? G().want_req.path : std::string(); }

bool has_frame() {
  Mgr& s = G();
  return s.want && s.store.has_frame() && s.store.session() == s.epoch.load();
}

void frame_dims(uint32_t& w, uint32_t& h) { G().store.dims(w, h); }

bool take_frame() {
  Mgr& s = G();
  const bool p = s.store.take_pending();
  return p && s.want;
}

bool acquire(FrameStore::Frame& f) {
  if (!has_frame()) return false;
  return G().store.acquire(f);
}

bool take_ended() { return G().ended.exchange(false); }
bool take_failed() { return G().failed.exchange(false); }
bool unavailable() { return G().unavail.load(); }

void set_wake(std::function<void()> wake) {
  Mgr& s = G();
  std::lock_guard<std::mutex> g(s.m);
  s.wake = std::move(wake);
}

void set_duck_hook(std::function<void(bool)> hook) { G().duck = std::move(hook); }

ImgId frame_id(Assets& a) {
  if (g_frame_uid == a.uid()) return g_frame_id;
  ImgId id;
  if (!a.find("video:frame", id)) {
    Img im;
    im.w = 1;
    im.h = 1;
    im.rgba.assign(4, 0);
    id = a.add("video:frame", std::move(im));
    a.pin(id);
  }
  g_frame_uid = a.uid();
  g_frame_id = id;
  return id;
}

bool is_frame(const Assets& a, ImgId id) { return g_frame_uid == a.uid() && g_frame_id == id; }

void box_reset() { g_box = Box(); }
void box_offer(float x, float y, float w, float h) {
  if (w * h > g_box.w * g_box.h) g_box = Box{x, y, w, h};
}
Box box_get() { return g_box; }

void set_engine_for_test(std::unique_ptr<Engine> e, bool threaded) {
  shutdown();
  Mgr& s = G();
  s.eng = std::move(e);
  s.engine_loaded = false;
  s.threaded = threaded;
  s.unavail = false;
}

void poll_for_test() { G().poll(); }
FrameStore& store_for_test() { return G().store; }

void reset_for_test() {
  shutdown();
  Mgr& s = G();
  s.eng.reset();
  s.threaded = true;
  s.unavail = false;
  s.duck = nullptr;
  s.wake = nullptr;
  box_reset();
}

}  // namespace video
}  // namespace zf
