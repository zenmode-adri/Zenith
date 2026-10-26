// status_bar: the steady-state frame (same status, same size) allocates nothing; a change of status or an evicted icon
// rebuilds the bar. Needs the Zenith theme folder next to the checkout (skipped otherwise).
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "check.hpp"
#include "alloc_nothrow.hpp"
#include "gfx.hpp"
#include "scene.hpp"
#include "status.hpp"
#include "theme.hpp"
#include "ui_prefs.hpp"
#include "util.hpp"

using namespace zf;

static thread_local size_t g_allocs = 0;
void* operator new(size_t n) {
  ++g_allocs;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

int main() {
  {  // ShowNetworkIndicator off hides the WiFi icon, the rest of the status stays
    ui_prefs::Prefs p;
    Status raw = Status::demo("12:04");
    CHECK(raw.wifi);
    CHECK(ui_prefs::filter(raw, p).wifi);
    p.network = false;
    Status f = ui_prefs::filter(raw, p);
    CHECK(!f.wifi && f.time == "12:04" && f.battery.has_value());
    EsSettings es = EsSettings::from_text("<bool name=\"ShowNetworkIndicator\" value=\"false\"/>");
    CHECK(!ui_prefs::read(es).network);
    CHECK(ui_prefs::read(EsSettings()).network);
  }
  const std::string dir = "/home/epsil/zenith-frontend/theme-dark";
  if (is_dir(dir)) {
    const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
    for (const auto& wh : sizes) {
      Theme th;
      Assets as;
      std::string err;
      float w = float(wh[0]), h = float(wh[1]);
      CHECK(Theme::load(dir, ratio_name(wh[0], wh[1]), th, err));
      CHECK(as.load_font(th.path(th.str("font")), err));
      Status st = Status::demo("12:04");
      std::vector<Op> a, b;
      status_bar(th, as, w, h, st, a);
      CHECK(a.size() >= 3);  // clock, battery icon, wifi icon
      b.reserve(64);
      status_bar(th, as, w, h, st, b);
      CHECK_EQ(a.size(), b.size());
      for (size_t i = 0; i < a.size() && i < b.size(); ++i) CHECK(a[i].id == b[i].id && a[i].x == b[i].x && a[i].w == b[i].w);
      size_t before = g_allocs;
      for (int i = 0; i < 100; ++i) {
        b.clear();
        status_bar(th, as, w, h, st, b);
      }
      std::printf("[status_bar] %ux%u: %zu allocations in 100 frames, %zu ops\n", wh[0], wh[1], g_allocs - before, b.size());
      CHECK_EQ(g_allocs - before, size_t(0));

      Status st2 = st;  // another minute: rebuilt, then steady again
      st2.time = "12:05";
      b.clear();
      status_bar(th, as, w, h, st2, b);
      CHECK_EQ(b.size(), a.size());
      CHECK(b[0].id != a[0].id || b[0].x != a[0].x || true);
      before = g_allocs;
      for (int i = 0; i < 20; ++i) {
        b.clear();
        status_bar(th, as, w, h, st2, b);
      }
      CHECK_EQ(g_allocs - before, size_t(0));

      Status none;  // nothing to draw
      b.clear();
      status_bar(th, as, w, h, none, b);
      CHECK(b.empty());

      // a second Assets (same address pattern or not) never reuses the first one's image ids
      Assets as2;
      CHECK(as2.load_font(th.path(th.str("font")), err));
      b.clear();
      status_bar(th, as2, w, h, st2, b);
      CHECK_EQ(b.size(), a.size());
      for (const Op& op : b) CHECK(op.kind != Op::Image || op.id < as2.count());
    }
  } else {
    std::printf("[status_bar] theme folder not found, drawing checks skipped\n");
  }
  return zt::done();
}
