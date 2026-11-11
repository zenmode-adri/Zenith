#include "ssmedia.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>
#include <time.h>

#include "covers.hpp"
#include "oscontract.hpp"
#include "screensaver.hpp"
#include "services.hpp"
#include "video.hpp"

namespace zf {
namespace ssmedia {

// ---- settings -----------------------------------------------------------------------------------------------------------------

namespace {
std::string str_of(const EsSettings& es, const char* key, const char* def) {
  const std::string* v = es.get(key);
  return v ? *v : std::string(def);
}
}  // namespace

Config load_config(const EsSettings& es, bool video) {
  Config c;
  c.video = video;
  // Settings.cpp:274 / :305; never below a second: a stray 0 in the file would swap on every loop
  c.swap_ms = std::max(1000L, es.get_int(video ? "ScreenSaverSwapVideoTimeout" : "ScreenSaverSwapImageTimeout", video ? 30000 : 10000));
  c.stretch = es.get_bool(video ? "StretchVideoOnScreenSaver" : "SlideshowScreenSaverStretch", false);          // :268 / :275
  c.custom = es.get_bool(video ? "SlideshowScreenSaverCustomVideoSource" : "SlideshowScreenSaverCustomImageSource", false);  // :289 / :276
  c.dir = str_of(es, video ? "SlideshowScreenSaverVideoDir" : "SlideshowScreenSaverImageDir", video ? "" : kScreenshotsDir);  // :292 (no default for video)
  if (c.dir.empty()) c.dir = kScreenshotsDir;  // pickRandomCustomImage: Paths::getScreenShotPath()
  c.filter = str_of(es, video ? "SlideshowScreenSaverVideoFilter" : "SlideshowScreenSaverImageFilter", video ? ".mp4,.avi" : ".png,.jpg");  // :290 / :277
  c.recurse = es.get_bool(video ? "SlideshowScreenSaverVideoRecurse" : "SlideshowScreenSaverRecurse", false);   // :291 / :278
  c.game_name = es.get_bool("SlideshowScreenSaverGameName", true);                                                // :279
  c.game_info = str_of(es, "ScreenSaverGameInfo", "never");                                                       // :267
  c.marquee = es.get_bool("ScreenSaverMarquee", true);                                                            // :265
  c.datetime = es.get_bool("ScreenSaverDateTime", false);                                                         // :262
  c.date_format = str_of(es, "ScreenSaverDateFormat", "%Y-%m-%d");                                                // :263
  c.time_format = str_of(es, "ScreenSaverTimeFormat", "%H:%M:%S");                                                // :264
  c.mute = es.get_bool("ScreenSaverVideoMute", false);                                                            // :308
  return c;
}

// ---- picking ------------------------------------------------------------------------------------------------------------------

bool ext_matches(const std::string& path, const std::string& filter) {
  if (filter.empty()) return true;
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos) return false;
  return filter.find(path.substr(dot)) != std::string::npos;
}

namespace {
void walk(const std::string& dir, const std::string& filter, bool recurse, std::vector<std::string>& out, int depth) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name.empty() || name[0] == '.') continue;
    const std::string full = dir + (dir.empty() || dir.back() != '/' ? "/" : "") + name;
    struct stat st;
    if (stat(full.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      if (recurse && depth < 16) walk(full, filter, true, out, depth + 1);
    } else if (S_ISREG(st.st_mode) && ext_matches(full, filter)) {
      out.push_back(full);
    }
  }
  closedir(d);
}
}  // namespace

std::vector<std::string> list_custom(const std::string& dir, const std::string& filter, bool recurse) {
  std::vector<std::string> out;
  if (!is_dir(dir)) return out;
  walk(dir, filter, recurse, out, 0);
  std::sort(out.begin(), out.end());
  return out;
}

std::string pick_custom(const Config& c, const Rng& rng) {
  const std::vector<std::string> files = list_custom(c.dir.empty() ? std::string(kScreenshotsDir) : c.dir, c.filter, c.recurse);
  if (files.empty()) return {};
  return files[std::min(rng(files.size()), files.size() - 1)];
}

std::vector<SysRef> eligible_systems(const std::vector<System>& systems) {
  std::vector<SysRef> out;
  for (const System& s : systems) {
    if (s.gamelist.empty() || s.rom_dir.empty()) continue;
    if (s.theme == "imageviewer" || s.platform.find("imageviewer") != std::string::npos || s.platform.find("ignore") != std::string::npos) continue;
    out.push_back({s.name, s.fullname, s.rom_dir, s.gamelist});
  }
  return out;
}

namespace {
class GamelistSource : public GameSource {
 public:
  explicit GamelistSource(std::vector<SysRef> s) : sys_(std::move(s)) {}
  size_t systems() const override { return sys_.size(); }
  Info info(size_t i) const override { return {sys_[i].name, sys_[i].full}; }
  void entries(size_t i, std::vector<Cand>& out) override {
    const SysRef& s = sys_[i];
    auto cb = [&](const GameEntry& e) {
      if (e.meta.hidden) return;
      Cand c;
      c.rom = resolve_rel_path(e.path, s.rom_dir);
      c.name = std::string(e.name);
      c.begin = e.begin;
      c.end = e.end;
      out.push_back(std::move(c));
    };
    gamelist_scan_file(s.gamelist, EntryFn::of(cb));
  }
  bool details(size_t i, const Cand& c, GameDetails& out) override {
    if (c.end == 0 || c.end > 0xFFFFFFFFull) return false;  // the span does not fit GameMeta: not worth a scan of the file
    GameMeta m;
    m.span_begin = uint32_t(c.begin);
    m.span_end = uint32_t(c.end);
    out = gamelist_details(sys_[i].gamelist, sys_[i].rom_dir, c.rom, m);
    return true;
  }

 private:
  std::vector<SysRef> sys_;
};
}  // namespace

std::unique_ptr<GameSource> make_gamelist_source(std::vector<SysRef> systems) { return std::make_unique<GamelistSource>(std::move(systems)); }

bool Picker::next(bool video, Pick& out) {
  const int k = video ? 1 : 0;
  if (!loaded_) {
    loaded_ = true;
    cands_.assign(src_->systems(), {});
    for (size_t i = 0; i < cands_.size(); ++i) src_->entries(i, cands_[i]);
  }
  bool rebuilt = false;
  for (size_t tries = 0; tries < kMaxTries; ++tries) {
    if (pool_[k].empty()) {  // used up (ES: "All videos used, resetting list")
      if (rebuilt) return false;
      rebuilt = true;
      for (uint32_t s = 0; s < cands_.size(); ++s)
        for (uint32_t i = 0; i < cands_[s].size(); ++i) pool_[k].push_back({s, i});
      if (pool_[k].empty()) return false;
    }
    const size_t at = std::min(rng_(pool_[k].size()), pool_[k].size() - 1);
    const std::pair<uint32_t, uint32_t> e = pool_[k][at];
    pool_[k][at] = pool_[k].back();
    pool_[k].pop_back();
    const GameSource::Cand& c = cands_[e.first][e.second];
    GameDetails d;
    if (!src_->details(e.first, c, d)) continue;
    const std::string& path = video ? d.video : d.image;
    if (path.empty() || !file_exists(path)) continue;  // ES: selectGameMedia checks that the file is there
    const GameSource::Info inf = src_->info(e.first);
    out = Pick();
    out.path = path;
    out.game = c.name;
    out.system = inf.name;
    out.system_full = inf.full;
    out.marquee = d.marquee;
    out.has_game = true;
    return true;
  }
  return false;
}

// ---- timing and look ----------------------------------------------------------------------------------------------------------

float fade_alpha(long t_ms) { return std::clamp(float(t_ms) / float(kFadeMs), 0.0f, 1.0f); }

bool swap_due(TimePoint now, TimePoint shown_at, long swap_ms) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(now - shown_at).count() >= swap_ms;
}

float info_alpha(const std::string& mode, long t, long total) {
  if (mode == "always") return 1.0f;
  if (mode != "start & end") return 0.0f;
  constexpr long kShow = 4000, kFade = 150;  // SUBTITLE_DURATION, SUBTITLE_FADE
  const long end = total - kShow;
  if (end - kFade <= kShow) return 1.0f;  // too short for a gap: stays
  if (t < kShow - kFade) return 1.0f;
  if (t < kShow) return 1.0f - float(t - (kShow - kFade)) / float(kFade);
  if (t < end - kFade) return 0.0f;
  if (t < end) return float(t - (end - kFade)) / float(kFade);
  return 1.0f;
}

// ---- the screensaver ----------------------------------------------------------------------------------------------------------

namespace {

struct Slide {
  Pick pick;
  ImgId id = 0;
};
struct Res {
  bool ok = false;
  Pick pick;
};
struct S {
  bool pending = false, active = false, video = false;
  Config cfg;
  std::shared_ptr<Picker> picker;
  uint64_t gen = 0;
  bool inflight = false, have_next = false, have_cur = false, have_prev = false, fading = false, frame_seen = false;
  Pick next_pick;
  Slide cur, prev;
  TimePoint shown_at{}, fade_at{};
  int fails = 0;
  time_t last_sec = -1;
};
S& st() {
  static S s;
  return s;
}
Rng g_test_rng;

Rng make_rng() {
  if (g_test_rng) return g_test_rng;
  auto eng = std::make_shared<std::mt19937_64>(std::random_device{}());
  return [eng](size_t n) { return n ? size_t((*eng)() % n) : size_t(0); };
}

long ms_since(TimePoint now, TimePoint t) { return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count(); }

uint32_t u32(float v) { return uint32_t(std::max(1L, std::lround(v))); }

/// Size the pictures are decoded into: the screen; for "stretch" (fill) a square of its longer side, so the crop has pixels to spare.
void decode_box(const Ctx& cx, const Config& c, uint32_t& w, uint32_t& h) {
  w = u32(cx.w);
  h = u32(cx.h);
  if (c.stretch && !c.video) w = h = std::max(w, h);
}

bool fallback() {
  on_stop();
  screensaver::set_mode(screensaver::Mode::Black);  // ES: no media, the plain screensaver (black, and the panel goes off)
  oscontract::backlight(4);
  return true;
}

void request_next(Ctx& cx);

void request_marquee(Ctx& cx, const Pick& p) {
  const S& s = st();
  if (s.cfg.marquee && s.cfg.game_name && p.has_game && !p.marquee.empty())
    covers::request(cx.assets, cx.sv->jobs, p.marquee, u32(cx.w * 0.40f), u32(cx.h * 0.22f));
}

void on_picked(Ctx& cx, uint64_t g, const Res& r) {
  S& s = st();
  if (g != s.gen || !s.active) return;
  s.inflight = false;
  const TimePoint now = Clock::now();
  if (!r.ok) {
    if (!s.have_cur) fallback();  // nothing to show at all
    else s.shown_at = now;        // keep what shows, look again after another interval
    return;
  }
  if (s.video) {
    video::Request rq;
    rq.path = r.pick.path;
    rq.box_w = u32(cx.w);
    rq.box_h = u32(cx.h);
    rq.cover = s.cfg.stretch;
    rq.mute = s.cfg.mute;
    rq.loop = false;  // the end starts the next one (ES setOnVideoEnded)
    rq.vlc_cmdline = cx.conf.get_or("vlc.commandline", "");
    rq.vlc_options = cx.conf.get_or("vlc.options", "");
    if (!video::play(rq)) {
      if (!s.have_cur) fallback();
      else s.shown_at = now;
      return;
    }
    s.cur.pick = r.pick;
    s.have_cur = true;
    s.shown_at = now;
    s.frame_seen = false;
    s.fading = false;
    request_marquee(cx, r.pick);
    return;
  }
  s.next_pick = r.pick;
  s.have_next = true;
  uint32_t bw, bh;
  decode_box(cx, s.cfg, bw, bh);
  covers::request(cx.assets, cx.sv->jobs, r.pick.path, bw, bh);
}

void request_next(Ctx& cx) {
  S& s = st();
  if (s.inflight) return;
  s.inflight = true;
  const uint64_t g = s.gen;
  auto res = std::make_shared<Res>();
  const Config cfg = s.cfg;
  const std::shared_ptr<Picker> picker = s.picker;
  const Rng rng = make_rng();
  Ctx* c = &cx;
  cx.sv->jobs.post(
      [res, cfg, picker, rng] {
        if (cfg.custom) {
          res->pick.path = pick_custom(cfg, rng);
          res->ok = !res->pick.path.empty();
        } else {
          res->ok = picker && picker->next(cfg.video, res->pick);
        }
      },
      [res, g, c] { on_picked(*c, g, *res); });
}

bool begin(Ctx& cx) {
  S& s = st();
  s.cfg = load_config(cx.es, s.video);
  if (s.video && video::unavailable()) return fallback();
  if (!s.cfg.custom) {
    std::vector<SysRef> refs = eligible_systems(cx.systems);
    if (refs.empty()) return fallback();
    s.picker = std::make_shared<Picker>(make_gamelist_source(std::move(refs)), make_rng());
  }
  s.active = true;
  s.have_cur = s.have_prev = s.have_next = s.fading = s.frame_seen = s.inflight = false;
  s.fails = 0;
  request_next(cx);
  return false;
}

bool poll_slide(Ctx& cx, TimePoint now) {
  S& s = st();
  bool redraw = false;
  if (s.have_next) {
    uint32_t bw, bh;
    decode_box(cx, s.cfg, bw, bh);
    ImgId id;
    if (covers::ready(cx.assets, s.next_pick.path, bw, bh, id)) {
      s.prev = s.cur;
      s.have_prev = s.have_cur;
      s.cur.pick = s.next_pick;
      s.cur.id = id;
      s.have_cur = true;
      s.have_next = false;
      s.fails = 0;
      s.shown_at = now;
      s.fade_at = now;
      s.fading = true;
      request_marquee(cx, s.cur.pick);
      redraw = true;
    } else if (covers::failed(s.next_pick.path)) {  // not a picture: another one
      s.have_next = false;
      if (++s.fails >= 5) {
        if (!s.have_cur) return fallback();
        s.shown_at = now;
      } else {
        request_next(cx);
      }
    }
  }
  if (s.fading) {
    redraw = true;
    if (ms_since(now, s.fade_at) >= kFadeMs) {
      s.fading = false;
      s.have_prev = false;
    }
  }
  if (s.have_cur && !s.have_next && !s.inflight && swap_due(now, s.shown_at, s.cfg.swap_ms)) request_next(cx);
  return redraw;
}

bool poll_video(Ctx& cx, TimePoint now) {
  S& s = st();
  bool redraw = false;
  if (video::take_failed()) {
    s.fails++;
    if (!s.have_cur || s.fails >= 5) return fallback();
    s.have_cur = false;  // this one failed: another
    request_next(cx);
    return redraw;
  }
  if (s.have_cur && !s.frame_seen && video::has_frame()) {
    s.frame_seen = true;
    s.fails = 0;
    s.fade_at = now;
    s.fading = true;
    redraw = true;
  }
  if (s.fading) {
    redraw = true;
    if (ms_since(now, s.fade_at) >= kFadeMs) s.fading = false;
  }
  const bool ended = video::take_ended();
  if (s.have_cur && !s.inflight && (ended || swap_due(now, s.shown_at, s.cfg.swap_ms))) request_next(cx);
  return redraw;
}

}  // namespace

void request_start(bool video) {
  S& s = st();
  s.pending = true;
  s.video = video;
}

void on_stop() {
  S& s = st();
  const bool was_video = s.video && (s.active || s.have_cur);
  ++s.gen;
  s.pending = s.active = false;
  s.inflight = s.have_next = s.have_cur = s.have_prev = s.fading = s.frame_seen = false;
  s.picker.reset();
  s.cur = Slide();
  s.prev = Slide();
  s.last_sec = -1;
  if (was_video) video::stop();
}

bool active() { return st().pending || st().active; }

bool poll(Ctx& cx, TimePoint now) {
  S& s = st();
  if (!s.pending && !s.active) return false;
  if (s.pending) {
    s.pending = false;
    return begin(cx);
  }
  bool redraw = s.video ? poll_video(cx, now) : poll_slide(cx, now);
  if (!s.active) return true;  // fell back to black meanwhile
  if (s.cfg.datetime) {
    const time_t t = time(nullptr);
    if (t != s.last_sec) {
      s.last_sec = t;
      redraw = true;
    }
  }
  return redraw;
}

std::optional<TimePoint> deadline(TimePoint now) {
  const S& s = st();
  if (!s.active) return std::nullopt;
  std::optional<TimePoint> d;
  auto want = [&](TimePoint t) {
    if (t <= now) t = now + std::chrono::milliseconds(20);  // never a deadline in the past: the loop would spin
    if (!d || t < *d) d = t;
  };
  if (s.fading) want(now + std::chrono::milliseconds(16));
  if (s.have_cur && !s.inflight && !s.have_next) want(s.shown_at + std::chrono::milliseconds(s.cfg.swap_ms));
  if (s.cfg.datetime) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    want(now + std::chrono::milliseconds(1000 - ts.tv_nsec / 1000000 + 20));
  }
  return d;
}

// ---- drawing ------------------------------------------------------------------------------------------------------------------

namespace {

Color tint(float alpha) { return Color{255, 255, 255, uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f)}; }

/// A line of text with a dark shadow (ES: the glow). `cy` is the centre of its box; `align`: 0 left (x), 1 centred on x.
void put_text(Assets& a, std::vector<Op>& out, const std::string& s, float px, Color c, float x, float cy, int align, float max_w, float alpha) {
  if (s.empty() || alpha <= 0) return;
  const ImgId sh = a.text_ellipsis(s, px, Color{0, 0, 0, 110}, max_w);
  const ImgId id = a.text_ellipsis(s, px, c, max_w);
  const Img& im = a.img(id);
  const float w = float(im.w), h = float(im.h);
  const float left = align == 1 ? x - w / 2.0f : x, top = std::round(cy - h / 2.0f);
  out.push_back(Op::image(sh, std::round(left) + 1.0f, top + 1.0f, w, h, tint(alpha * 0.8f)));
  out.push_back(Op::image(id, std::round(left), top, w, h, tint(alpha)));
}

void draw_overlay(Ctx& cx, std::vector<Op>& out, const S& s, float alpha, TimePoint now) {
  Assets& a = cx.assets;
  const float W = cx.w, H = cx.h;
  const float sz = std::floor(H / 16.0f);
  if (s.cfg.datetime) {  // ES GameScreenSaverBase: date above time at the top left, over everything, opaque
    const time_t t = time(nullptr);
    tm lt;
    localtime_r(&t, &lt);
    char db[96], tb[96];
    if (strftime(db, sizeof db, s.cfg.date_format.c_str(), &lt) == 0) db[0] = 0;
    if (strftime(tb, sizeof tb, s.cfg.time_format.c_str(), &lt) == 0) tb[0] = 0;
    const float margin = sz / 2.0f, dpx = std::floor(sz * 0.66f), fh = a.letter_height(sz);
    put_text(a, out, db, dpx, Color{0xD0, 0xD0, 0xD0, 255}, margin, margin + dpx * 0.33f, 0, W - 2 * margin, 1.0f);
    put_text(a, out, tb, sz, Color{255, 255, 255, 255}, margin, margin + dpx * 0.66f * 1.3f + fh / 2.0f, 0, W - 2 * margin, 1.0f);
  }
  if (!s.cfg.game_name || !s.have_cur || !s.cur.pick.has_game) return;
  const Pick& p = s.cur.pick;
  if (s.cfg.video) alpha *= info_alpha(s.cfg.game_info, ms_since(now, s.shown_at), s.cfg.swap_ms);
  if (alpha <= 0) return;
  const float h4 = std::floor(H / 4.0f), fh = a.letter_height(sz);
  bool marq = false;
  if (s.cfg.marquee && !p.marquee.empty()) {  // ES: the marquee image instead of the name, at 16 % of the height
    ImgId id;
    const uint32_t bw = u32(W * 0.40f), bh = u32(H * 0.22f);
    if (covers::ready(a, p.marquee, bw, bh, id) && !a.evicted(id)) {
      float x, y, w, h;
      video::place(float(a.img(id).w), float(a.img(id).h), W * 0.5f - float(bw) / 2.0f, H * 0.16f - float(bh) / 2.0f, float(bw), float(bh), false, x, y, w, h);
      out.push_back(Op::image(id, std::round(x), std::round(y), w, h, tint(alpha)));
      marq = true;
    }
  }
  if (!marq) put_text(a, out, p.game, sz, Color{255, 255, 255, 255}, W / 2.0f, (H - h4 - fh / 2.0f) + (h4 - fh / 2.0f) / 2.0f, 1, W * 0.92f, alpha);
  put_text(a, out, p.system_full, std::floor(sz * 0.66f), Color{0xD0, 0xD0, 0xD0, 255}, W / 2.0f, (H - h4 + fh / 2.0f) + (h4 + fh / 2.0f) / 2.0f, 1, W * 0.92f, alpha);
}

void draw_image(Ctx& cx, std::vector<Op>& out, ImgId id, float alpha, bool fill) {
  const Assets& a = cx.assets;
  if (id >= a.count() || a.evicted(id)) return;
  float x, y, w, h;
  video::place(float(a.img(id).w), float(a.img(id).h), 0, 0, cx.w, cx.h, fill, x, y, w, h);
  if (fill) out.push_back(Op::clip(0, 0, cx.w, cx.h));
  out.push_back(Op::image(id, x, y, w, h, tint(alpha)));
  if (fill) out.push_back(Op::unclip());
}

}  // namespace

void draw(Ctx& cx, std::vector<Op>& out) {
  const S& s = st();
  const TimePoint now = Clock::now();
  out.push_back(Op::rect(0, 0, cx.w, cx.h, {0, 0, 0, 255}));
  if (!s.active) return;
  const float fade = s.fading ? fade_alpha(ms_since(now, s.fade_at)) : 1.0f;
  if (s.video) {
    if (video::has_frame() && s.frame_seen) {
      uint32_t fw, fh;
      video::frame_dims(fw, fh);
      float x, y, w, h;
      video::place(float(fw), float(fh), 0, 0, cx.w, cx.h, s.cfg.stretch, x, y, w, h);
      if (s.cfg.stretch) out.push_back(Op::clip(0, 0, cx.w, cx.h));
      out.push_back(Op::image(video::frame_id(cx.assets), x, y, w, h, tint(fade)));
      if (s.cfg.stretch) out.push_back(Op::unclip());
      draw_overlay(cx, out, s, fade, now);
    } else if (s.cfg.datetime) {
      draw_overlay(cx, out, s, 0.0f, now);
    }
    return;
  }
  if (s.have_prev && s.fading) draw_image(cx, out, s.prev.id, 1.0f, s.cfg.stretch);
  if (s.have_cur) draw_image(cx, out, s.cur.id, fade, s.cfg.stretch);
  draw_overlay(cx, out, s, s.have_cur ? fade : 0.0f, now);
}

// ---- tests --------------------------------------------------------------------------------------------------------------------

void set_rng_for_test(Rng rng) { g_test_rng = std::move(rng); }
bool busy_for_test() { return st().inflight; }
void reset_for_test() {
  on_stop();
  st() = S();
  g_test_rng = nullptr;
}

}  // namespace ssmedia
}  // namespace zf
