// fix5/ram: the RAM given back when a game starts (Assets::drop_unpinned, details cache, failed covers), the expiry of failed
// covers, px_rect edges, resize without extra copies, the wake gate, Theme::f warning once, the launched game found by path and
// the cut-ellipsis cache. (The loop of sdlr.cpp itself -- batch cleared after a launch, negative texture cache, VmRSS log -- needs
// a window and the device.)
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "covers.hpp"
#include "cpu.hpp"
#include "detailed.hpp"
#include "gamemeta.hpp"
#include "gfx.hpp"
#include "launch.hpp"
#include "resize.hpp"
#include "theme.hpp"
#include "theme_warn.hpp"
#include "util.hpp"
#include "wakegate.hpp"

using namespace zf;

namespace {

bool make_png(const std::string& path, uint32_t w, uint32_t h, uint8_t r) {
  Canvas c;
  c.w = w;
  c.h = h;
  c.px.assign(size_t(w) * h * 4, 255);
  for (size_t i = 0; i < c.px.size(); i += 4) c.px[i] = r;
  std::string err;
  return save_png(c, path, err);
}

// Finding 1 (priority): everything unpinned goes, pinned stays, and the picture is drawable again afterwards.
void test_drop_unpinned() {
  const std::string base = "/tmp/zf_test_fix5ram_" + std::to_string(getpid());
  const std::string pa = base + "_a.png", pb = base + "_b.png", pc = base + "_c.png";
  CHECK(make_png(pa, 100, 100, 10) && make_png(pb, 100, 100, 20) && make_png(pc, 100, 100, 30));
  const size_t kImg = 100 * 100 * 4;
  Assets a;
  a.track_evictions(true);
  ImgId ia = 0, ib = 0, ic = 0;
  CHECK(a.file(pa, ia) && a.file(pb, ib) && a.file(pc, ic));
  a.pin(ia);
  a.release_pixels(ib);  // uploaded to a texture: only the budget counts it
  // ic still has its CPU copy (never uploaded): it must go too
  CHECK_EQ(a.bytes(), 3 * kImg);
  CHECK_EQ(a.drop_unpinned(), 2 * kImg);
  CHECK(a.evicted(ib) && a.evicted(ic));
  CHECK(!a.evicted(ia) && a.has_pixels(ia));  // pinned: pixels kept (the textures are rebuilt from them)
  CHECK_EQ(a.bytes(), kImg);
  CHECK_EQ(a.cpu_bytes(), kImg);
  std::vector<ImgId> gone;
  a.take_evicted(gone);  // the renderer destroys just these textures
  CHECK_EQ(gone.size(), size_t(2));
  a.take_evicted(gone);
  CHECK(gone.empty());
  CHECK_EQ(a.drop_unpinned(), size_t(0));  // idempotent

  // the screen redraws after the restore: the same pictures come back by key, with pixels
  ImgId nb = 0, nc = 0, again = 0;
  CHECK(a.file(pb, nb) && nb != ib && a.has_pixels(nb));
  CHECK(a.file(pc, nc) && nc != ic && a.has_pixels(nc));
  CHECK(a.file(pa, again) && again == ia);
  std::vector<Op> ops = {Op::image(ia, 0, 0, 10, 10), Op::image(nb, 10, 0, 10, 10)};
  Canvas cv = cpu_render(ops, a, 20, 10);
  CHECK(cv.px[0] == 10 && cv.px[(size_t(10) * 4)] == 20);
  std::remove(pa.c_str());
  std::remove(pb.c_str());
  std::remove(pc.c_str());
}

// The cached gamelist details are dropped (the cache has at most 16 entries but each holds a description).
void test_details_cache() {
  System sys;
  sys.name = "gbc";
  for (const char* n : {"Alpha", "Bravo"}) {
    Game g;
    g.name = n;
    g.path = std::string("/nonexistent/") + n + ".gb";
    sys.games.push_back(g);
  }
  detailed_set_demo(false);
  detailed_cache_clear();
  DetailedData d;
  CHECK(detailed_for(sys, 0, true, d) && detailed_for(sys, 1, true, d));
  CHECK_EQ(detailed_cache_size(), size_t(2));
  detailed_cache_clear();
  CHECK_EQ(detailed_cache_size(), size_t(0));
  CHECK(detailed_for(sys, 0, true, d));  // rebuilt lazily
  CHECK_EQ(detailed_cache_size(), size_t(1));
  // review z3 #3: an edit or a scrape (games_changed) drops what was read before, so the new description shows
  CHECK(detailed_for(sys, 1, true, d));
  CHECK_EQ(detailed_cache_size(), size_t(2));
  games_changed();
  CHECK(detailed_for(sys, 1, true, d));
  CHECK_EQ(detailed_cache_size(), size_t(1));
  detailed_cache_clear();
}

double g_now = 100.0;
double fake_now() { return g_now; }

// Finding 11: a cover that failed is tried again after 60 s.
void test_failed_expiry() {
  covers::set_clock_for_test(fake_now);
  g_now = 100.0;
  const std::string p = "/tmp/zf_fix5ram_missing_cover.png";
  CHECK(!covers::failed(p));
  covers::mark_failed(p);
  CHECK(covers::failed(p));
  g_now = 159.0;
  CHECK(covers::failed(p));
  g_now = 161.0;
  CHECK(!covers::failed(p));  // expired: the next request decodes it again
  covers::mark_failed(p);
  CHECK(covers::failed(p));
  covers::clear_failed();
  CHECK(!covers::failed(p));
  covers::set_clock_for_test(nullptr);
}

// Finding 1 (z5): both edges rounded, so neighbours that share an edge in float share it in pixels.
void test_px_rect() {
  const PxRect a = px_rect(Op::rect(10.4f, 3.4f, 10.4f, 5.4f, Color{255, 255, 255, 255}));
  const PxRect b = px_rect(Op::rect(20.8f, 8.8f, 4.0f, 4.0f, Color{255, 255, 255, 255}));
  CHECK_EQ(a.x + a.w, b.x);  // rounding x and w apart gave 20 against 21: a 1 px gap
  CHECK_EQ(a.y + a.h, b.y);
  const PxRect c = px_rect(Op::rect(0.5f, 0.5f, 0.2f, 0.2f, Color{255, 255, 255, 255}));
  CHECK(c.w >= 0 && c.h >= 0);
}

// Finding 13: the moving overload gives the same pixels and takes the source over.
void test_resize_move() {
  std::vector<uint8_t> src(7 * 5 * 4);
  uint32_t x = 12345;
  for (uint8_t& v : src) {
    x = x * 1664525u + 1013904223u;
    v = uint8_t(x >> 24);
  }
  const std::vector<uint8_t> want = resize_rgba(src.data(), 7, 5, 3, 4, Filter::Triangle);
  std::vector<uint8_t> copy = src;
  const std::vector<uint8_t> got = resize_rgba(std::move(copy), 7, 5, 3, 4, Filter::Triangle);
  CHECK(got == want);
  CHECK(copy.empty());  // freed after the vertical pass
  std::vector<uint8_t> same = src;
  const std::vector<uint8_t> kept = resize_rgba(std::move(same), 7, 5, 7, 5, Filter::CatmullRom);
  CHECK(kept == src);
  CHECK(resize_rgba(src.data(), 7, 5, 7, 5, Filter::CatmullRom) == src);
}

// Finding 5: no SDL_PushEvent once the gate is closed, and closing waits for a push in flight.
void test_wake_gate() {
  WakeGate g;
  int n = 0;
  CHECK(!g.notify([&] { ++n; }));  // closed: SDL is not up yet / any more
  CHECK_EQ(n, 0);
  g.set_up(true);
  CHECK(g.notify([&] { ++n; }));
  CHECK_EQ(n, 1);

  std::atomic<int> pushes{0};
  std::atomic<bool> stop{false};
  std::thread w([&] {
    while (!stop.load()) g.notify([&] { pushes.fetch_add(1); });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  g.set_up(false);  // the game starts: SDL_Quit follows
  const int at_close = pushes.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK_EQ(pushes.load(), at_close);  // nothing after the close returned
  stop = true;
  w.join();
}

// Finding 9: one stderr line per missing variable, not one per call.
void test_theme_warn_once() {
  Theme t;
  const size_t before = theme_warn::count();
  for (int i = 0; i < 500; ++i) t.f("zz_fix5ram_missing_a");
  CHECK_EQ(theme_warn::count(), before + 1);
  t.f("zz_fix5ram_missing_b");
  CHECK_EQ(theme_warn::count(), before + 2);
  CHECK(t.f("zz_fix5ram_missing_a") == 0.0f);
}

// Finding 10: the launched game is found by system name and path after the lists changed.
void test_find_by_path() {
  set_writes_blocked(true);
  Ctx cx;
  System s1, s2;
  s1.name = "gb";
  s2.name = "gbc";
  for (const char* n : {"x", "y"}) {
    Game g;
    g.name = n;
    g.path = std::string("/roms/gbc/") + n + ".gb";
    s2.games.push_back(g);
  }
  cx.systems.push_back(s1);
  cx.systems.push_back(s2);
  size_t si = 9, gi = 9;
  CHECK(launch::find_game(cx, "gbc", "/roms/gbc/y.gb", si, gi) && si == 1 && gi == 1);
  // a reload put another system first and another game before: the old indexes (1, 1) would hit the wrong game
  System s0;
  s0.name = "nes";
  cx.systems.insert(cx.systems.begin(), s0);
  Game g0;
  g0.name = "new";
  g0.path = "/roms/gbc/new.gb";
  cx.systems[2].games.insert(cx.systems[2].games.begin(), g0);
  CHECK(launch::find_game(cx, "gbc", "/roms/gbc/y.gb", si, gi) && si == 2 && gi == 2);
  CHECK(!launch::find_game(cx, "gbc", "/roms/gbc/gone.gb", si, gi));
  CHECK(!launch::find_game(cx, "psx", "/roms/gbc/y.gb", si, gi));
  launch::Outcome o;
  o.after.count_play = true;
  launch::apply_after_path(cx, "gbc", "/roms/gbc/y.gb", o);
  CHECK_EQ(cx.systems[2].games[2].meta.playcount, uint32_t(1));
  CHECK_EQ(cx.systems[2].games[1].meta.playcount, uint32_t(0));  // its neighbour is untouched
  launch::apply_after_path(cx, "gbc", "/roms/gbc/gone.gb", o);   // gone: nothing happens
  set_writes_blocked(false);
}

std::string theme_dir() {
  const char* e = std::getenv("ZF_THEME_DIR");
  return e ? e : "/usr/share/zfront/theme-dark";
}

// z3 #2: the cut text is the same as before (cached per face/size/width/text) and fits.
void test_ellipsis_cache() {
  Theme th;
  Assets a;
  std::string err;
  if (!Theme::load(theme_dir(), "1/1", th, err) || !a.load_font(th.path(th.str("font")), err)) {
    std::fprintf(stderr, "test_fix5_ram: theme not found (%s): ellipsis test skipped\n", err.c_str());
    return;
  }
  const std::string text = "A very long game title that certainly does not fit in the row of the list";
  const Color c = {255, 255, 255, 255};
  ImgId i1 = a.text_ellipsis(text, 20, c, 160);
  const size_t n1 = a.count();
  ImgId i2 = a.text_ellipsis(text, 20, c, 160);
  CHECK_EQ(i1, i2);
  CHECK_EQ(a.count(), n1);
  CHECK(float(a.img(i1).w) <= 160.0f + 1.0f);
  ImgId i3 = a.text_ellipsis(text, 20, c, 300);  // another width is another cut
  CHECK(a.img(i3).w > a.img(i1).w);
  CHECK_EQ(a.ellipsis_cache_size(), size_t(2));  // one per (width, text): the second call of the first width hit the cache
  a.drop_unpinned();  // the cache goes with the images
  CHECK_EQ(a.ellipsis_cache_size(), size_t(0));
  ImgId i4 = a.text_ellipsis(text, 20, c, 160);
  CHECK(a.img(i4).w == a.img(i1).w && !a.evicted(i4));
}

}  // namespace

int main() {
  test_drop_unpinned();
  test_details_cache();
  test_failed_expiry();
  test_px_rect();
  test_resize_move();
  test_wake_gate();
  test_theme_warn_once();
  test_find_by_path();
  test_ellipsis_cache();
  return zt::done();
}
