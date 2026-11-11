// `menu.battery`: the charge, what the console is using now, how it has gone down, the power saver; `menu.battery.usage`: what used the most since the last full charge.
// Drawn by hand (a graph is not a row of the menu framework) on the same grid as the remade themes, with the colours of whatever theme is on.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.hpp"
#include "i18n.hpp"
#include "menu_system.hpp"
#include "power.hpp"
#include "registry.hpp"
#include "remake.hpp"
#include "scene.hpp"
#include "services.hpp"
#include "ui_prefs.hpp"
#include "ui_style.hpp"
#include "util.hpp"

namespace zf {

// ScreenSaverTime (ms): 0 (or less) = never. A value under a minute (ES's slider allows it) is not "never": it counts as the
// minute it rounds to (at least 1) and shows in seconds.
long saver_time_minutes(long ms) { return ms <= 0 ? 0 : std::max(1L, (ms + 30000) / 60000); }

std::string saver_time_label(long ms) {
  if (ms <= 0) return tr("Never");
  if (ms < 60000) return std::to_string((ms + 500) / 1000) + " s";
  return std::to_string(saver_time_minutes(ms)) + " min";
}

namespace {

using remake::Grid;

ui::Shapes g_sh;

void label(Assets& a, std::vector<Op>& ops, std::string_view s, float x, float cy, int px, Color c, float max_w = 0, bool right = false) {
  if (s.empty()) return;
  ImgId id = max_w > 0 ? a.text_ellipsis(s, float(px), c, max_w) : a.text(s, float(px), c);
  const float tw = float(a.img(id).w), th = float(a.img(id).h);
  ops.push_back(Op::image(id, std::round(right ? x - tw : x), std::round(cy - th / 2.0f), tw, th));
  if (remake::audit_sink()) remake::audit(right ? x - tw : x, cy - th / 2.0f, tw, th, "\"" + std::string(s.substr(0, 18)) + "\"", right ? 'R' : 'L');
}

// A container on the lines of the cells (card, row, pill): the test measures its four edges. Cards that hold things are tagged "tarjeta".
void box(float x, float y, float w, float h, const char* tag, bool cols = false) {
  if (remake::audit_sink()) remake::audit(x, y, w, h, tag, 'B', cols, true);
}

// The first line of cells at or after `m + off` (a text that follows another one starts there).
float up(const Grid& g, float m, float off) { return m + float(g.u) * std::ceil(off / float(g.u) - 0.01f); }
// The last line of cells at or before `r - off` (a right-aligned text that stands to the left of another one ends there).
float back(const Grid& g, float r, float off) { return r - float(g.u) * std::ceil(off / float(g.u) - 0.01f); }

std::string duration(float hours) {
  const long m = std::lround(hours * 60.0f);
  char b[48];
  if (m >= 60) std::snprintf(b, sizeof b, "%ld h %ld min", m / 60, m % 60);
  else std::snprintf(b, sizeof b, "%ld min", m);
  return b;
}

Color card_of(const ui::Style& st) { return st.dark ? Color{255, 255, 255, 12} : Color{255, 255, 255, 255}; }

void head(Ctx& cx, const ui::Style& st, const Grid& g, std::string_view title, std::vector<Op>& ops) {
  ops.push_back(Op::rect(0, 0, g.w, g.h, st.bg));
  remake::menu_backdrop(cx.th, cx.assets, g.w, g.h, ops);
  const size_t stack_from = ops.size();
  status_bar(cx.th, cx.assets, g.w, g.h, cx.status, ops);
  const ui::Metrics mt = ui::Metrics::from(cx.th, g.w, g.h);
  const float x = mt.title_left;
  const float tmax = ui::title_max_w(st, mt, ops, stack_from, g.w * 0.55f);
  int tpx = mt.title_px;
  ImgId id = cx.assets.text_ellipsis(title, float(tpx), st.txt, tmax);
  while (tpx > 8 && float(cx.assets.img(id).h) > float(g.bar_h())) id = cx.assets.text_ellipsis(title, float(--tpx), st.txt, tmax);  // inside the bar's four cells
  const float tw = float(cx.assets.img(id).w), th = float(cx.assets.img(id).h);
  ops.push_back(Op::image(id, x, std::round(mt.header_cy - th / 2.0f), tw, th));
  ui::header_box() = {true, x, x + tw, mt.header_cy - th / 2.0f, mt.header_cy + th / 2.0f, float(g.content_top())};
}

// ---- usage -----------------------------------------------------------------------------------------------------------------------------------

class UsageScreen : public Screen {
 public:
  explicit UsageScreen(Ctx&) : sum_(power::summary_now()) {}
  bool is_menu() const override { return true; }
  Transition handle(Action a, Ctx& cx) override {
    (void)cx;
    const int n = int(sum_.rows.size());
    switch (a) {
      case Action::B: return Transition::pop();
      case Action::Start: return Transition::close_menus();
      case Action::Down:
        if (n) cur_ = (cur_ + 1) % n;
        break;
      case Action::Up:
        if (n) cur_ = (cur_ + n - 1) % n;
        break;
      default: break;
    }
    return Transition::none();
  }
  void draw(Ctx& cx, std::vector<Op>& ops) override {
    const Grid g(cx.w, cx.h);
    const ui::Style st = ui::Style::from(cx.th);
    Assets& a = cx.assets;
    head(cx, st, g, tr("What used the most"), ops);
    const float m = g.col(0), R = m + g.span(12), w = R - m;
    const int npx = g.F(13, 11), bpx = g.F(16, 13);
    const float note_h = float(g.U(int(std::ceil(float(npx) * 1.3f / float(g.u)))));
    const float note_cy = float(g.content_top()) + note_h / 2.0f;
    label(a, ops, tr("Since the last full charge. Estimated."), m, note_cy, npx, st.dim, w);  // under the title
    const float y0 = float(g.content_top()) + note_h + g.gutter();
    if (!sum_.valid) {
      label(a, ops, tr("Nothing measured yet. It fills in as you play."), m, g.h * 0.45f, bpx, st.dim, w);
    } else {
      const int pc_cells = std::max(g.snap(58.0f * g.s, 8) / g.u, int(std::ceil(float(bpx + npx) * 1.3f / float(g.u))) + 3);
      const float pitch = float(g.U(pc_cells)), rh = pitch - float(g.U(1));
      const int rows = std::max(1, int((float(g.help_top()) - y0) / pitch));
      const int n = int(sum_.rows.size());
      const int start = std::clamp(cur_ - rows / 2, 0, std::max(0, n - rows));
      const float top = sum_.rows.empty() ? 1.0f : std::max(sum_.rows[0].pct, 1.0f);
      for (int i = start; i < std::min(n, start + rows); ++i) {
        const power::Row& r = sum_.rows[size_t(i)];
        const float y = y0 + float(i - start) * pitch;
        const bool on = i == cur_;
        if (on) {
          g_sh.rrect(a, ops, m, y, w, rh, g.U(2), st.cyan_soft);
          box(m, y, w, rh, "tarjeta", true);
        }
        const Color col = on ? st.cyan_dark : st.txt;
        const int ic_c = (pc_cells - 1) % 2 == 0 ? 6 : 5;
        const float ic = float(g.U(ic_c)), ix = m + float(g.U(2)), iy = y + float(g.U((pc_cells - 1 - ic_c) / 2)), cy = iy + ic / 2.0f;
        g_sh.rrect(a, ops, ix, iy, ic, ic, g.U(2), st.dark ? Color{255, 255, 255, 16} : Color{0, 0, 0, 12});
        box(ix, iy, ic, ic, "pastilla");
        static const char* const kIcons[] = {"games", "screen", "library", "battery"};
        ImgId id;
        if (a.svg(cx.th.path(std::string("assets/menu/menu-icon-") + kIcons[int(r.kind)] + ".svg"), uint32_t(ic * 0.6f), id))
          ops.push_back(Op::image(id, std::round(ix + ic / 2.0f - float(a.img(id).w) / 2.0f), std::round(cy - float(a.img(id).h) / 2.0f), float(a.img(id).w), float(a.img(id).h), col));
        const float tx = ix + ic + float(g.U(2)), right = R - float(g.U(2));
        const std::string name = r.kind == power::Row::Game ? r.name : r.kind == power::Row::Screen ? std::string(tr("Screen")) : r.kind == power::Row::Menus ? std::string(tr("Menus and system")) : std::string(tr("Asleep"));
        char pc[16];
        std::snprintf(pc, sizeof pc, "%d %%", int(std::lround(r.pct)));
        const float l1 = y + rh * 0.30f;
        label(a, ops, pc, right, l1, bpx, col, 0, true);
        const std::string t = duration(float(r.secs) / 3600.0f);
        const float t_right = back(g, right, a.measure(pc, float(bpx)) + float(g.U(2)));
        const float time_w = a.measure(t, float(npx));
        label(a, ops, t, t_right, l1, npx, on ? col : st.dim, 0, true);
        label(a, ops, name, tx, l1, bpx, col, t_right - time_w - float(g.U(2)) - tx);
        const float bw = right - tx, bh = float(g.U(1)), by = y + float(g.U(int(std::lround(float(pc_cells - 1) * 0.64f))));
        g_sh.pill(a, ops, tx, by, bw, bh, ui::with_alpha(st.dim, 0.18f));
        box(tx, by, bw, bh, "pastilla");
        g_sh.pill(a, ops, tx, by, std::max(bh, bw * r.pct / top), bh, on ? col : st.dark ? st.cyan : st.cyan_dark);
      }
    }
    help_bar(cx.th, a, g.w, g.h, help_prompts(), ops);
  }

 private:
  static const HelpPrompts& help_prompts() {
    static HelpPrompts p;
    static std::string c, b;
    c = i18n::sentence_case(tr("CHOOSE")), b = i18n::sentence_case(tr("BACK"));
    p = {{"updown", c.c_str()}, {"B", b.c_str()}};
    return p;
  }
  power::Summary sum_;
  int cur_ = 0;
};

// ---- the main screen -------------------------------------------------------------------------------------------------------------------------

constexpr float kRefreshSecs = 5.0f;

class BatteryScreen : public Screen {
 public:
  explicit BatteryScreen(Ctx& cx) { refresh(cx); }
  bool is_menu() const override { return true; }
  std::optional<float> wake_in() const override { return std::max(kRefreshSecs - age_, 0.05f); }
  bool tick(float dt, Ctx& cx) override {
    age_ += dt;
    // a charger plugged or pulled (the bar already shows it): the numbers and the word "Charging" follow now, not at the next 5 s
    const std::optional<Battery> bar = ui_prefs::raw_battery();
    const bool bar_moved = !(bar == seen_);
    seen_ = bar;
    if (bar_moved || age_ >= kRefreshSecs) {
      refresh(cx);
      redraw_ = true;
    }
    return false;
  }
  bool take_redraw() override {
    const bool r = redraw_;
    redraw_ = false;
    return r;
  }
  Transition handle(Action a, Ctx& cx) override {
    switch (a) {
      case Action::B: return Transition::pop();
      case Action::Start: return Transition::close_menus();
      case Action::Down: cur_ = (cur_ + 1) % kRows; break;
      case Action::Up: cur_ = (cur_ + kRows - 1) % kRows; break;
      case Action::Left:
      case Action::Right:
        if (cur_ == kSaver) power::set_saver(cx, !power::saver_on(cx));
        else if (cur_ == kAuto) step_auto(cx, a == Action::Right ? 1 : -1);
        else if (cur_ == kScreen) step_screen(cx, a == Action::Right ? 1 : -1);
        break;
      case Action::A:
        if (cur_ == kUsage) return Transition::open("menu.battery.usage");
        if (cur_ == kSaver) power::set_saver(cx, !power::saver_on(cx));
        else if (cur_ == kAuto) step_auto(cx, 1);
        else if (cur_ == kScreen) step_screen(cx, 1);
        else if (cur_ == kMore) return Transition::open("menu.g.power");
        break;
      default: break;
    }
    return Transition::none();
  }
  void draw(Ctx& cx, std::vector<Op>& ops) override {
    const Grid g(cx.w, cx.h);
    const ui::Style st = ui::Style::from(cx.th);
    Assets& a = cx.assets;
    head(cx, st, g, tr("Battery and power"), ops);
    const bool saver = power::saver_on(cx);
    const Color green = {0x35, 0xB8, 0x6B, 255};
    const Color acc = saver ? green : (st.dark ? st.cyan : st.cyan_dark);
    const float m = g.col(0), R = m + g.span(12), gap = g.gutter(), pad = float(g.U(2));
    const float top = float(g.content_top()), ch = float(g.snap(104.0f * g.s, 12));
    const float cw = g.span(6);
    const int rad = g.U(2);
    // ---- level
    g_sh.rrect(a, ops, m, top, cw, ch, rad, card_of(st));
    box(m, top, cw, ch, "tarjeta", true);
    char pc[16];
    std::snprintf(pc, sizeof pc, "%d%%", now_.percent);
    const int big = g.F(44, 26);
    label(a, ops, pc, m + pad, top + ch * 0.32f, big, st.txt);
    const float tx = up(g, m, pad + a.measure(pc, float(big)) + float(g.U(2)));
    const float tmax = m + cw - pad - tx;
    const float left = power::hours_left(now_);
    if (now_.charging) {
      label(a, ops, tr("Charging"), tx, top + ch * 0.32f, g.F(15), st.txt, tmax);
    } else if (left != power::kNoHours) {
      label(a, ops, tr("About") + std::string(" ") + duration(left), tx, top + ch * 0.22f, g.F(15), st.txt, tmax);
      label(a, ops, tr("left, at this rate"), tx, top + ch * 0.44f, g.F(13, 11), st.dim, tmax);
    }
    const float bx = m + pad, bw = cw - 2 * pad, bh = float(g.U(2)), by = top + ch - pad - bh;
    g_sh.pill(a, ops, bx, by, bw, bh, ui::with_alpha(st.dim, 0.25f));
    box(bx, by, bw, bh, "pastilla");
    g_sh.pill(a, ops, bx, by, std::max(bh, bw * float(now_.percent) / 100.0f), bh, now_.percent <= 15 && !now_.charging ? st.danger : acc);
    // ---- now
    const float c2 = g.col(6), r2 = c2 + cw - pad;
    g_sh.rrect(a, ops, c2, top, cw, ch, rad, card_of(st));
    box(c2, top, cw, ch, "tarjeta", true);
    label(a, ops, tr("Using now"), c2 + pad, top + ch * 0.19f, g.F(13, 11), st.dim, cw * 0.6f);
    char w[48], vi[64];
    if (now_.watts > 0.01f) {
      std::snprintf(w, sizeof w, "%.1f W", double(now_.watts));
      std::snprintf(vi, sizeof vi, "%.2f V \xC2\xB7 %d mA", double(now_.volts), int(std::lround(now_.amps * 1000.0f)));
    } else {
      std::snprintf(w, sizeof w, "--");
      std::snprintf(vi, sizeof vi, "%.2f V", double(now_.volts));
    }
    label(a, ops, w, c2 + pad, top + ch * 0.48f, g.F(28, 18), st.txt);
    label(a, ops, vi, c2 + pad, top + ch * 0.79f, g.F(13, 11), st.dim, cw * 0.62f);
    if (sum_.awake_secs > 0) {
      label(a, ops, tr("Since full"), r2, top + ch * 0.19f, g.F(13, 11), st.dim, cw * 0.36f, true);
      label(a, ops, duration(float(sum_.awake_secs) / 3600.0f), r2, top + ch * 0.48f, g.F(15, 12), st.txt, cw * 0.4f, true);
      char u[32];
      std::snprintf(u, sizeof u, "%d %%", sum_.used_pct);
      label(a, ops, u, r2, top + ch * 0.79f, g.F(13, 11), st.dim, 0, true);
    }
    // ---- settings, from the bottom up: what is left is for the graph
    const int rc = std::max(g.snap(std::max(36.0f * g.s, float(g.F(16, 13)) + 12.0f * g.s), 5) / g.u, 5);
    const float row_h = float(g.U(rc));
    const float rows_h = row_h * float(kRows - 1);
    const float rows_top = float(g.help_top()) - rows_h;
    const float gy = top + ch + gap, gh = rows_top - gy - gap;
    const float gw = R - m;
    // ---- graph: the first row, A shows what used the most
    if (cur_ == kUsage) {
      g_sh.rrect(a, ops, m, gy, gw, gh, rad, acc);
      g_sh.rrect(a, ops, m + 2, gy + 2, gw - 4, gh - 4, rad - 1, st.dark ? st.bg : Color{255, 255, 255, 255});
    } else {
      g_sh.rrect(a, ops, m, gy, gw, gh, rad, st.dark ? card_of(st) : Color{255, 255, 255, 255});
    }
    box(m, gy, gw, gh, "tarjeta", true);
    const float isz = float(g.U(2));
    label(a, ops, tr("Last hours"), m + pad, gy + float(g.U(3)), g.F(13, 11), st.dim, g.w * 0.3f);
    label(a, ops, tr("What used the most"), R - pad - isz - float(g.U(1)), gy + float(g.U(3)), g.F(13, 11), cur_ == kUsage ? st.txt : st.dim, g.w * 0.45f, true);
    g_sh.icon(a, ops, ui::Icon::ChevronRight, R - pad - isz, gy + float(g.U(2)), int(isz), cur_ == kUsage ? st.txt : st.dim);
    const float px = m + pad, pw = gw - 2 * pad, py = gy + float(g.U(5)), ph = gh - float(g.U(5)) - float(g.U(4));
    if (ph >= float(g.U(4))) {
      if (sum_.curve.size() >= 2 && sum_.span_hours > 0) for (const float f : {0.0f, 0.5f, 1.0f}) ops.push_back(Op::rect(px, std::round(py + ph * f), pw, 1, ui::with_alpha(st.dim, 0.14f)));
      if (sum_.curve.size() >= 2 && sum_.span_hours > 0) {
        const float cols = std::floor(pw / float(std::max(2, g.P(3))));
        const float col_w = pw / cols;
        size_t k = 0;
        for (int i = 0; i < int(cols); ++i) {
          const float hours_ago = sum_.span_hours * (1.0f - (float(i) + 0.5f) / cols);
          while (k + 1 < sum_.curve.size() && sum_.curve[k + 1].first >= hours_ago) ++k;
          float v = float(sum_.curve[k].second);
          if (k + 1 < sum_.curve.size() && sum_.curve[k].first > sum_.curve[k + 1].first) {
            const float t = (sum_.curve[k].first - hours_ago) / (sum_.curve[k].first - sum_.curve[k + 1].first);
            v = v + (float(sum_.curve[k + 1].second) - v) * std::clamp(t, 0.0f, 1.0f);
          }
          const float hh = ph * v / 100.0f;
          const float xa = std::round(px + float(i) * col_w), xb = std::round(px + float(i + 1) * col_w);  // whole pixels: columns that overlap would show as stripes
          ops.push_back(Op::rect(xa, std::round(py + ph - hh), xb - xa, std::round(hh), ui::with_alpha(acc, 0.20f)));
          ops.push_back(Op::rect(xa, std::round(py + ph - hh), xb - xa, float(std::max(2, g.P(2))), acc));
        }
        char ago[32];
        std::snprintf(ago, sizeof ago, "%s", duration(sum_.span_hours).c_str());
        label(a, ops, ago, px, py + ph + float(g.U(2)), g.F(12, 11), st.dim);
        label(a, ops, tr("Now"), px + pw, py + ph + float(g.U(2)), g.F(12, 11), st.dim, 0, true);
      } else {
        label(a, ops, tr("Nothing measured yet. It fills in as you play."), px, py + ph / 2.0f, g.F(13, 11), st.dim, pw);
      }
    }
    // ---- rows
    const std::string auto_v = power::saver_auto(cx) > 0 ? std::to_string(power::saver_auto(cx)) + " %" : std::string(tr("Never"));
    const std::string ss_v = saver_time_label(cx.es.get_int("ScreenSaverTime", 300000));
    struct Line {
      int id;
      const char* t;
      std::string v;
    } rows[] = {{kSaver, tr("Power saver"), ""}, {kAuto, tr("Turn on by itself at"), auto_v}, {kScreen, tr("Screen off after"), ss_v}, {kMore, tr("More settings"), ""}};
    const int fs = g.F(16, 13);
    int idx = 0;
    for (const Line& r : rows) {
      const float y = rows_top + float(idx) * row_h, cy = y + row_h / 2.0f;
      const bool on = cur_ == r.id;
      if (on) {
        g_sh.pill(a, ops, m, y, gw, row_h, saver && r.id == kSaver ? ui::with_alpha(green, 0.25f) : st.cyan_soft);
        box(m, y, gw, row_h, "tarjeta", true);
      }
      const Color col = on && !(saver && r.id == kSaver) ? st.cyan_dark : st.txt;
      label(a, ops, r.t, m + pad, cy, fs, col, gw * 0.6f);
      if (r.id == kSaver) {
        const int shc = rc % 2 == 0 ? 4 : 3;
        const float sw = float(g.U(2 * shc)), shh = float(g.U(shc)), sx = R - pad - sw, sy = y + float(g.U((rc - shc) / 2));
        g_sh.pill(a, ops, sx, sy, sw, shh, saver ? green : ui::with_alpha(st.dim, 0.45f));
        box(sx, sy, sw, shh, "pastilla");
        g_sh.disc(a, ops, saver ? sx + sw - shh + 2 : sx + 2, sy + 2, int(shh) - 4, Color{255, 255, 255, 255});
      } else {
        const bool chev = r.id == kMore;
        label(a, ops, r.v, chev ? R - pad - isz - float(g.U(1)) : R - pad, cy, g.F(15, 13), on ? col : st.dim, 0, true);
        if (chev) g_sh.icon(a, ops, ui::Icon::ChevronRight, R - pad - isz, cy - isz / 2.0f, int(isz), on ? col : st.dim);
      }
      ++idx;
    }
    help_bar(cx.th, a, g.w, g.h, help_prompts(cur_ == kSaver || cur_ == kAuto || cur_ == kScreen), ops);
  }

 private:
  enum { kUsage, kSaver, kAuto, kScreen, kMore, kRows };
  // Left / Right change the switch and the two steppers: the help says so on those rows only
  static const HelpPrompts& help_prompts(bool adjust) {
    static HelpPrompts p;
    static std::string c, s, j, b;
    c = i18n::sentence_case(tr("CHOOSE")), s = i18n::sentence_case(tr("SELECT")), j = i18n::sentence_case(tr("ADJUST")), b = i18n::sentence_case(tr("BACK"));
    p = {{"updown", c.c_str()}, {"A", s.c_str()}};
    if (adjust) p.push_back({"leftright", j.c_str()});
    p.push_back({"B", b.c_str()});
    return p;
  }
  void refresh(Ctx&) {
    now_ = power::read_now();
    sum_ = power::summary_now();
    age_ = 0;
    seen_ = ui_prefs::raw_battery();
    if (seen_) now_.charging = seen_->charging;  // the same word as the bar's bolt (the charger's `online`, not the battery's status)
  }
  void step_auto(Ctx& cx, int d) {
    static const int kVals[] = {0, 15, 20, 30};
    int i = 0;
    for (int k = 0; k < 4; ++k)
      if (kVals[k] == power::saver_auto(cx)) i = k;
    power::set_saver_auto(cx, kVals[(i + 4 + d) % 4]);
  }
  void step_screen(Ctx& cx, int d) {
    static const long kVals[] = {1, 2, 5, 10, 30, 0};
    const long cur = saver_time_minutes(cx.es.get_int("ScreenSaverTime", 300000));
    int i = 2;
    for (int k = 0; k < 6; ++k)
      if (kVals[k] == cur) i = k;
    cx.es.set_int("ScreenSaverTime", kVals[(i + 6 + d) % 6] * 60000);
    cx.es.save();
  }
  power::Now now_;
  power::Summary sum_;
  std::optional<Battery> seen_;
  float age_ = 0;
  bool redraw_ = false;
  int cur_ = kSaver;
};

}  // namespace

ZF_REGISTER_SCREEN("menu.battery", [](Ctx& cx, const std::string& arg) -> std::unique_ptr<Screen> {
  if (!power::read_now().have) return Registry::instance().make("menu.g.power", cx, arg);  // no battery: only the settings
  return std::make_unique<BatteryScreen>(cx);
});
ZF_REGISTER_SCREEN("menu.battery.usage", [](Ctx& cx, const std::string&) -> std::unique_ptr<Screen> { return std::make_unique<UsageScreen>(cx); });

}  // namespace zf
