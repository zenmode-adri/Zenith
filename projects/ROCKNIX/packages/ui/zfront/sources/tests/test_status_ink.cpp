// The status bar of every theme draws its icons, percent and hour at one height, on one line: what is drawn is measured, op by op, with the
// software renderer (the one of --preview), on every screen size of the consoles and with charging, low battery, Bluetooth and no WiFi.
// What a bar draws behind its icons (a box, a pill, discs, a frame) it reports itself and is left out. ZF_STATUS_INK_REPORT=1 prints every bar.
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "cpu.hpp"
#include "es_theme.hpp"
#include "gfx.hpp"
#include "remake.hpp"
#include "scene.hpp"
#include "status.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

struct Idle : Screen {
  Transition handle(Action, Ctx&) override { return Transition::none(); }
  void draw(Ctx&, std::vector<Op>&) override {}
};

std::string find_base(const char* name) {
  if (const char* e = std::getenv("ZF_THEME_DIR")) {
    const std::string p = std::string(e) + "/" + name;
    if (fs::exists(p + "/theme.xml")) return p;
  }
  for (const char* d : {"themes/", "../themes/", "./"}) {
    const std::string p = std::string(d) + name;
    if (fs::exists(p + "/theme.xml")) return p;
  }
  return {};
}

struct Span {
  int x0, x1, y0, y1;
};

/// The ink (alpha over half) of one op drawn alone; false when it draws nothing.
bool ink_of(const Op& op, const Assets& a, uint32_t w, uint32_t h, Span& s) {
  const Canvas cv = cpu_render({op}, a, w, h);
  s = {int(w), -1, int(h), -1};
  for (uint32_t y = 0; y < cv.h; ++y)
    for (uint32_t x = 0; x < cv.w; ++x)
      if (cv.px[(size_t(y) * cv.w + x) * 4 + 3] > 127) s.x0 = std::min(s.x0, int(x)), s.x1 = std::max(s.x1, int(x)), s.y0 = std::min(s.y0, int(y)), s.y1 = std::max(s.y1, int(y));
  return s.x1 >= 0;
}

int g_bad = 0, g_bars = 0;

void measure(const std::string& name, const Theme& th, Assets& a, int w, int h, const char* state, const Status& st, bool report) {
  std::vector<remake::Box> quiet;
  std::vector<size_t> bg;
  remake::audit_sink() = &quiet;  // no cached bar: it is built here, and tells what is background
  remake::status_bg_sink() = &bg;
  std::vector<Op> ops;
  status_bar(th, a, float(w), float(h), st, ops);
  remake::audit_sink() = nullptr;
  remake::status_bg_sink() = nullptr;
  const uint32_t ch = uint32_t(std::min(h, h / 5 + 40));
  std::vector<Span> el;  // one per thing drawn: the ops that overlap across, as the level inside the battery, are one
  for (size_t i = 0; i < ops.size(); ++i) {
    if (std::find(bg.begin(), bg.end(), i) != bg.end() || (ops[i].kind != Op::Image && ops[i].kind != Op::Rect)) continue;
    Span s;
    if (!ink_of(ops[i], a, uint32_t(w), ch, s)) continue;
    el.push_back(s);
  }
  std::sort(el.begin(), el.end(), [](const Span& p, const Span& q) { return p.x0 < q.x0; });
  std::vector<Span> m;
  for (const Span& s : el)
    if (!m.empty() && s.x0 <= m.back().x1 + 1) {
      Span& b = m.back();
      b.x1 = std::max(b.x1, s.x1), b.y0 = std::min(b.y0, s.y0), b.y1 = std::max(b.y1, s.y1);
    } else {
      m.push_back(s);
    }
  ++g_bars;
  CHECK(m.size() >= 2);
  int t0 = 1 << 20, t1 = -1, b0 = 1 << 20, b1 = -1;
  for (const Span& s : m) t0 = std::min(t0, s.y0), t1 = std::max(t1, s.y0), b0 = std::min(b0, s.y1), b1 = std::max(b1, s.y1);
  const bool ok = t1 - t0 <= 1 && b1 - b0 <= 1;
  if (!ok) ++g_bad;
  if (!ok || report) {
    std::printf("  %s %-26s %4dx%-4d %-8s", ok ? "  " : "MAL", name.c_str(), w, h, state);
    for (const Span& s : m) std::printf(" [x%d alto %d, %d-%d]", s.x0, s.y1 - s.y0 + 1, s.y0, s.y1);
    std::printf("\n");
  }
}

}  // namespace

int main() {
  const std::string light = find_base("theme-light"), dark = find_base("theme-dark");
  if (light.empty() || dark.empty()) {
    std::printf("sin carpetas del tema: se omite\n");
    return zt::done();
  }
  const bool report = std::getenv("ZF_STATUS_INK_REPORT") != nullptr;
  char tmpl[] = "/tmp/zf-test-status-ink-XXXXXX";
  const std::string root = mkdtemp(tmpl);
  std::vector<std::pair<std::string, std::string>> themes = {{"zenith-minimal", light}, {"zenith-minimal-dark", dark}};
  for (const auto& [from, to] : {std::pair<std::string, std::string>{light, "es-theme-zenith-next"}, std::pair<std::string, std::string>{dark, "es-theme-zenith-next-dark"}}) {
    const std::string d = root + "/next/" + to;
    fs::create_directories(root + "/next");
    fs::copy(fs::canonical(from), d, fs::copy_options::recursive);
    std::ifstream in(d + "/theme.xml");
    std::string xml((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t at = xml.find("<variables>");
    CHECK(at != std::string::npos);
    xml.insert(at + 11, "<zenStyle>next</zenStyle>");
    std::ofstream(d + "/theme.xml") << xml;
    themes.push_back({to.substr(9), d});
  }
  std::vector<estheme::Generated> keep;
  for (const char* folder : {"es-theme-xmb-fcamod", "es-theme-art-book-next", "es-theme-iconic", "es-theme-techdweeb"}) {
    const std::string es = root + "/" + folder;
    fs::create_directories(es);
    std::ofstream(es + "/theme.xml") << "<theme><view name=\"system\"><carousel name=\"systemcarousel\"><type>horizontal</type></carousel></view></theme>";
    estheme::Options opt;
    estheme::Generated gen;
    std::string err;
    CHECK(estheme::generate(es, estheme::Bases{light, dark}, root + "/gen-" + folder, opt, gen, err));
    themes.push_back({std::string(folder).substr(9), gen.dir});
  }
  const int sizes[][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 540}, {720, 720}, {800, 480}, {854, 480}, {1024, 768}, {1280, 720}};
  struct State {
    const char* name;
    int pct;
    bool charging, wifi, bt, battery;
    const char* time;
  };
  const State states[] = {{"cargando", 100, true, true, true, true, "12:04"},
                          {"normal", 62, false, true, false, true, "23:59"},
                          {"baja", 12, false, false, true, true, "10:10"},
                          {"sin-bat", 0, false, true, false, false, "08:00"}};
  for (const auto& [name, dir] : themes)
    for (const auto& sz : sizes) {
      Theme th;
      std::string err;
      CHECK(Theme::load(dir, ratio_name(unsigned(sz[0]), unsigned(sz[1])), th, err));
      Assets a;
      CHECK(a.load_font(th.path(th.str("font")), err));
      for (const State& s : states) {
        Status st;
        st.time = s.time;
        st.wifi = s.wifi;
        st.bluetooth = s.bt;
        if (s.battery) st.battery = Battery{uint8_t(s.pct), s.charging};
        measure(name, th, a, sz[0], sz[1], s.name, st, report);
      }
    }
  // XMB while charging: the PSP's animation lights one more cell each step (more white each step), and without it the cells show the level.
  // Zenith Minimal: the battery is red when low and not charging (the other themes already were), and only then.
  for (const auto& [name, dir] : themes) {
    if (name != "xmb-fcamod" && name != "zenith-minimal-dark" && name != "zenith-minimal") continue;
    for (const auto& sz : {std::array<int, 2>{480, 320}, std::array<int, 2>{720, 720}}) {
      Theme th;
      std::string err;
      CHECK(Theme::load(dir, ratio_name(unsigned(sz[0]), unsigned(sz[1])), th, err));
      Assets a;
      CHECK(a.load_font(th.path(th.str("font")), err));
      const auto count = [&](const Status& st, bool red) {
        std::vector<Op> ops;
        status_bar(th, a, float(sz[0]), float(sz[1]), st, ops);
        const Canvas cv = cpu_render(ops, a, uint32_t(sz[0]), uint32_t(sz[1]));
        int n = 0;
        for (size_t i = 0; i + 3 < cv.px.size(); i += 4) {
          const int r = cv.px[i], g = cv.px[i + 1], b = cv.px[i + 2];
          n += red ? (r > g + 70 && r > b + 50) : (r > 200 && g > 200 && b > 200);
        }
        return n;
      };
      Status st;
      st.time = "12:04";
      st.wifi = true;
      if (name == "xmb-fcamod") {
        st.battery = Battery{62, true};
        int prev = -1;
        for (int8_t f = 0; f <= 3; ++f) {
          st.charge_frame = f;
          const int n = count(st, false);
          if (n <= prev) std::printf("FALLO %s %dx%d: el paso %d de la carga no enciende mas que el anterior (%d <= %d)\n", name.c_str(), sz[0], sz[1], f, n, prev);
          CHECK(n > prev);
          prev = n;
          if (f == 2) {
            Status lvl = st;
            lvl.charge_frame = -1;  // 62 % = two cells, the same as step two
            CHECK_EQ(count(lvl, false), n);
          }
        }
      } else {
        st.battery = Battery{12, false};
        const int low = count(st, true);
        st.battery = Battery{62, false};
        const int normal = count(st, true);
        st.battery = Battery{12, true};
        const int charging = count(st, true);
        if (low == 0 || normal != 0 || charging != 0)
          std::printf("FALLO %s %dx%d: rojo con 12 %% %d, con 62 %% %d, con 12 %% cargando %d\n", name.c_str(), sz[0], sz[1], low, normal, charging);
        CHECK(low > 0 && normal == 0 && charging == 0);
      }
    }
  }
  // On the device every image drawn is uploaded and its pixels released (sdlr.cpp): a bar built again (the minute changed, the charge
  // moved) must not read the pixels of what it measured before. It crashed every minute on the Ultra (8 Nov). Built again after
  // releasing everything, it must lay out exactly as a bar built from scratch.
  for (const auto& [name, dir] : themes) {
    Theme th;
    std::string err;
    CHECK(Theme::load(dir, ratio_name(720, 720), th, err));
    const auto geometry = [&](Assets& a, const Status& st) {
      std::vector<Op> ops;
      status_bar(th, a, 720.0f, 720.0f, st, ops);
      std::string g;
      for (const Op& op : ops) g += std::to_string(int(op.kind)) + ":" + std::to_string(int(op.x)) + "," + std::to_string(int(op.y)) + "," + std::to_string(int(op.w)) + "," + std::to_string(int(op.h)) + " ";
      return g;
    };
    for (const bool charging : {true, false}) {
      Status st;
      st.time = "12:04";
      st.wifi = true;
      st.bluetooth = true;
      st.battery = Battery{100, charging};
      Assets used;
      CHECK(used.load_font(th.path(th.str("font")), err));
      geometry(used, st);
      for (ImgId i = 0; i < used.count(); ++i) used.release_pixels(i);
      const std::string times[] = {"12:05", "12:04"};  // a new hour, and one already measured
      for (const std::string& t : times) {
        st.time = t;
        Assets fresh;
        CHECK(fresh.load_font(th.path(th.str("font")), err));
        const std::string again = geometry(used, st), from_scratch = geometry(fresh, st);
        if (again != from_scratch) std::printf("FALLO %s %s: la barra rehecha tras liberar los pixeles no es la de cero (%s)\n  rehecha %s\n  de cero %s\n", name.c_str(), charging ? "cargando" : "llena", t.c_str(), again.c_str(), from_scratch.c_str());
        CHECK(again == from_scratch);
        for (ImgId i = 0; i < used.count(); ++i) used.release_pixels(i);
      }
    }
  }

  // The loop steps the animation: with XMB and a charging battery a cell more every half second (empty, one, two, three, empty) and a
  // wake-up for the next one; not charging, with motion off or in another theme, the cells show the level and nothing wakes for it.
  for (const auto& [name, dir] : themes) {
    if (name != "xmb-fcamod" && name != "zenith-minimal-dark") continue;
    const auto make = [&](bool motion) {
      Ctx cx;
      std::string err;
      CHECK(Theme::load(dir, ratio_name(720, 720), cx.th, err));
      cx.w = cx.h = 720;
      cx.ui_motion = motion;
      cx.sv->cmd.dry_run = true;
      return std::make_unique<App>(std::move(cx), std::make_unique<Idle>(), make_status_source("/nonexistent"));
    };
    Status chg;
    chg.time = "12:04";
    chg.battery = Battery{62, true};
    const TimePoint t0 = Clock::now();
    const auto at = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };
    {
      auto app = make(true);
      app->freeze_status(chg);
      const bool xmb = name == "xmb-fcamod";
      const int8_t want[] = {0, 0, 1, 1, 2, 3, 0};
      const int ms[] = {0, 499, 500, 999, 1000, 1500, 2000};
      for (size_t i = 0; i < std::size(ms); ++i) {
        app->poll_status(at(ms[i]));
        const int8_t f = app->cx.status.charge_frame, w = xmb ? want[i] : int8_t(-1);
        if (f != w) std::printf("FALLO %s: a los %d ms el paso de la carga es %d y deberia ser %d\n", name.c_str(), ms[i], f, w);
        CHECK_EQ(int(f), int(w));
      }
      const std::optional<TimePoint> dl = app->status_deadline();
      if (xmb) CHECK(dl && *dl <= at(2500));  // the loop wakes for the next cell
      Status full = chg;
      full.battery = Battery{100, false};
      app->freeze_status(full);
      app->poll_status(at(2600));
      CHECK_EQ(int(app->cx.status.charge_frame), -1);  // charged: the level again
      const std::optional<TimePoint> dl2 = app->status_deadline();
      CHECK(!dl2 || *dl2 > at(60000));  // and no more wake-ups for it
    }
    {
      auto app = make(true);  // full and still plugged in ("Full"): the cells stay full
      Status full = chg;
      full.battery = Battery{100, true};
      app->freeze_status(full);
      app->poll_status(at(0));
      app->poll_status(at(600));
      CHECK_EQ(int(app->cx.status.charge_frame), -1);
    }
    {
      auto app = make(false);  // motion off: no animation
      app->freeze_status(chg);
      app->poll_status(at(0));
      app->poll_status(at(600));
      CHECK_EQ(int(app->cx.status.charge_frame), -1);
    }
  }
  std::error_code ec;
  fs::remove_all(root, ec);
  std::printf("barras medidas: %d, con alturas distintas: %d\n", g_bars, g_bad);
  CHECK_EQ(g_bad, 0);
  return zt::done();
}
