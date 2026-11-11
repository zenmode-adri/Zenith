// `menu.game.stats`: GAMES > PLAY STATISTICS. Read-only: the time played in all, the last five weeks (from the play log, playstats.hpp),
// the ten most played games and the time per system (from the gamelists). Drawn by hand, like the battery screens, on the same grid as the
// remade themes and with the colours of whatever theme is on.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>

#include "app.hpp"
#include "batest.hpp"
#include "i18n.hpp"
#include "playstats.hpp"
#include "power.hpp"
#include "registry.hpp"
#include "remake.hpp"
#include "scene.hpp"
#include "services.hpp"
#include "ui_style.hpp"
#include "util.hpp"

namespace zf {
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

// A container on the lines of the cells (card, pill): the test measures its four edges. A card that holds things is tagged "tarjeta".
void box(float x, float y, float w, float h, const char* tag, bool cols = false) {
  if (remake::audit_sink()) remake::audit(x, y, w, h, tag, 'B', cols, true);
}

// Cells that hold `px` of text: a line of text takes 1.3 times its size.
int cells_of(const Grid& g, float px) { return int(std::ceil(px * 1.3f / float(g.u) - 0.01f)); }

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

std::string sc(const char* key) { return i18n::sentence_case(tr(key)); }

// One line of the list. Bar: label, time, a bar; Game: name, time, "system - plays"; Total: the big number.
struct Row {
  enum Kind { Total, Head, Bar, Game, Note } kind = Bar;
  std::string text, value, sub;
  float frac = 0;
};

// BATTERY USE: the games that used the most per hour, with how long a full charge lasts at that rate. Without sessions measured on the
// battery the section says so instead of showing a made-up number.
void battery_rows(const std::vector<System>& systems, const std::vector<playstats::LogEntry>& log, std::vector<Row>& r) {
  r.push_back({Row::Head, sc("BATTERY USE (FULL CHARGE)"), {}, {}, 0});
  const std::vector<batest::Drainer> top = batest::top_drainers(log, 5);
  if (top.empty()) {
    r.push_back({Row::Note, sc("NOT ENOUGH DATA YET"), {}, {}, 0});
    return;
  }
  for (const batest::Drainer& d : top) {
    std::string name = path_stem(d.rom), sys = d.system;
    for (const System& s : systems) {
      if (s.name != d.system) continue;
      if (!s.fullname.empty()) sys = s.fullname;
      for (const Game& g : s.games)
        if (g.path == d.rom && !g.name.empty()) name = g.name;
    }
    char b[24];
    std::snprintf(b, sizeof b, "%.1f %%/h", d.pct_per_hour);
    batest::Rate rt;
    rt.source = batest::Rate::Game, rt.pct_per_hour = d.pct_per_hour;
    r.push_back({Row::Game, name, batest::format_left(batest::hours_left(rt, 100)), sys + " \xC2\xB7 " + b, 0});
  }
}

std::vector<Row> make_rows(const playstats::Stats& s) {
  using playstats::format_duration;
  std::vector<Row> r;
  r.push_back({Row::Total, sc("TOTAL TIME PLAYED"), format_duration(s.total_secs), s.total_plays ? std::to_string(s.total_plays) + "\xC3\x97" : std::string(), 0});
  uint64_t wmax = 1;
  for (uint64_t w : s.week_secs) wmax = std::max(wmax, w);
  r.push_back({Row::Head, sc("LAST FIVE WEEKS"), {}, {}, 0});
  const std::string names[playstats::kWeeks] = {sc("THIS WEEK"), sc("LAST WEEK"), sc("2 WEEKS AGO"), sc("3 WEEKS AGO"), sc("4 WEEKS AGO")};
  for (int i = 0; i < playstats::kWeeks; ++i) r.push_back({Row::Bar, names[i], format_duration(s.week_secs[i]), {}, float(s.week_secs[i]) / float(wmax)});
  if (!s.top.empty()) {
    r.push_back({Row::Head, sc("MOST PLAYED GAMES"), {}, {}, 0});
    for (const playstats::GameTime& g : s.top) r.push_back({Row::Game, g.name, format_duration(g.secs), g.system + " \xC2\xB7 " + std::to_string(g.plays) + "\xC3\x97", 0});
  }
  if (!s.systems.empty()) {
    r.push_back({Row::Head, sc("TIME PER SYSTEM"), {}, {}, 0});
    const uint64_t top = std::max<uint64_t>(1, s.systems[0].second);
    for (const auto& sy : s.systems) r.push_back({Row::Bar, sy.first, format_duration(sy.second), {}, float(sy.second) / float(top)});
  }
  return r;
}

class StatsScreen : public Screen {
 public:
  // The log (it only grows) and every game of every system are gone through by the worker, on its own copy of the systems: opening
  // the screen never holds a frame. Tests and the preview harness (no services, or a dry run) do it in place.
  explicit StatsScreen(Ctx& cx) : d_(std::make_shared<Data>()) {
    auto d = d_;
    auto systems = std::make_shared<std::vector<System>>(cx.systems);
    auto work = [d, systems] {
      const std::vector<playstats::LogEntry> log = playstats::parse_log(load_log());
      const playstats::Stats st = playstats::compute(playstats::collect(*systems), log, ::time(nullptr));
      if (st.empty()) return;
      d->rows = make_rows(st);
      if (power::read_now().have) battery_rows(*systems, log, d->rows);  // a console without a battery has no such section
    };
    if (!cx.sv || cx.sv->cmd.dry_run) {
      work();
      rows_ = std::move(d->rows);
      loading_ = false;
      return;
    }
    cx.sv->jobs.post(work, [d] { d->ready = true; });
  }
  bool is_menu() const override { return true; }
  Transition handle(Action a, Ctx& cx) override {
    take();
    switch (a) {
      case Action::B: return Transition::pop();
      case Action::Start: return Transition::close_menus();
      case Action::Down:
        if (top_ + 1 < rows_.size() && !fits_from(top_, Grid(cx.w, cx.h))) ++top_;
        break;
      case Action::Up:
        if (top_ > 0) --top_;
        break;
      default: break;
    }
    return Transition::none();
  }
  void draw(Ctx& cx, std::vector<Op>& ops) override {
    const Grid g(cx.w, cx.h);
    const ui::Style st = ui::Style::from(cx.th);
    Assets& a = cx.assets;
    take();
    head(cx, st, g, sc("PLAY STATISTICS"), ops);
    const float m = g.col(0), w = g.span(12);
    const Color acc = st.dark ? st.cyan : st.cyan_dark;
    if (loading_) {
      label(a, ops, sc("PLEASE WAIT"), m, g.h * 0.45f, g.F(16, 13), st.dim, w);
    } else if (rows_.empty()) {
      label(a, ops, sc("NOTHING PLAYED YET. PLAY A GAME AND IT WILL SHOW UP HERE."), m, g.h * 0.45f, g.F(16, 13), st.dim, w);
    } else {
      float y = float(g.content_top());
      const float limit = float(g.help_top());
      for (size_t i = top_; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        const float h = height(r, g);
        if (y + h > limit + 0.5f) break;
        draw_row(a, ops, st, g, r, y, h, m, w, acc);
        y += h;
      }
    }
    help_bar(cx.th, a, g.w, g.h, help_prompts(), ops);
  }

 private:
  struct Data {
    std::vector<Row> rows;
    bool ready = false;  // set on the main thread, when the worker is done
  };
  void take() {
    if (!loading_ || !d_->ready) return;
    rows_ = std::move(d_->rows);
    loading_ = false;
  }
  static std::string load_log() {
    std::string t;
    read_file(playstats::log_path(), t);
    return t;
  }
  static float height(const Row& r, const Grid& g) {
    const int p1 = g.F(15, 12), p2 = g.F(12, 11);
    switch (r.kind) {
      case Row::Total: return float(g.U(cells_of(g, float(g.F(13, 11) + g.F(24, 16))) + 3));  // the card (two lines, a cell above, under and between) and a cell of air
      case Row::Head: return float(g.U(cells_of(g, float(p2)) + 2));
      default: return float(g.U(std::max(cells_of(g, float(p1 + p2)) + 1, cells_of(g, float(p1)) + 3)));
    }
  }
  // Is the end of the list on screen when it starts at row `from`?
  bool fits_from(size_t from, const Grid& g) const {
    float y = float(g.content_top());
    for (size_t i = from; i < rows_.size(); ++i) {
      y += height(rows_[i], g);
      if (y > float(g.help_top()) + 0.5f) return false;
    }
    return true;
  }
  void draw_row(Assets& a, std::vector<Op>& ops, const ui::Style& st, const Grid& g, const Row& r, float y, float h, float m, float w, Color acc) const {
    const float right = m + w;
    const int p1 = g.F(15, 12), p2 = g.F(12, 11);
    switch (r.kind) {
      case Row::Total: {
        const float hc = h - float(g.U(1)), q1 = float(g.F(13, 11)), q2 = float(g.F(24, 16));
        g_sh.rrect(a, ops, m, y, w, hc, g.U(2), st.dark ? Color{255, 255, 255, 12} : Color{255, 255, 255, 255});
        box(m, y, w, hc, "tarjeta", true);
        const float px = m + float(g.U(2)), cy2 = y + hc - float(g.U(1)) - q2 * 0.65f;
        label(a, ops, r.text, px, y + float(g.U(1)) + q1 * 0.65f, int(q1), st.dim, w * 0.7f);
        label(a, ops, r.value, px, cy2, int(q2), st.txt, w * 0.7f);
        label(a, ops, r.sub, right - float(g.U(2)), cy2, p1, st.dim, w * 0.25f, true);
        break;
      }
      case Row::Head:
        label(a, ops, r.text, m, y + h - float(g.U(1)) - float(p2) * 0.65f, p2, st.dim, w);
        break;
      case Row::Note:
        label(a, ops, r.text, m, y + h * 0.5f, g.F(14, 12), st.dim, w);
        break;
      case Row::Bar:
      case Row::Game: {
        const float l1 = y + float(p1) * 0.65f;
        const float vw = a.measure(r.value, float(p1));
        label(a, ops, r.value, right, l1, p1, st.txt, 0, true);
        label(a, ops, r.text, m, l1, p1, st.txt, w - vw - float(g.U(2)));
        if (r.kind == Row::Game) {
          label(a, ops, r.sub, m, y + float(p1) * 1.3f + float(p2) * 0.65f, p2, st.dim, w);
        } else {
          const float bh = float(g.U(1)), by = y + float(g.U(cells_of(g, float(p1)) + 1));
          g_sh.pill(a, ops, m, by, w, bh, ui::with_alpha(st.dim, 0.18f));
          box(m, by, w, bh, "pastilla");
          if (r.frac > 0) g_sh.pill(a, ops, m, by, std::max(bh, w * r.frac), bh, acc);
        }
        break;
      }
    }
  }
  static const HelpPrompts& help_prompts() {
    static HelpPrompts p;
    static std::string c, b;
    c = i18n::sentence_case(tr("CHOOSE")), b = i18n::sentence_case(tr("BACK"));
    p = {{"updown", c.c_str()}, {"B", b.c_str()}};
    return p;
  }
  std::shared_ptr<Data> d_;
  bool loading_ = true;
  std::vector<Row> rows_;
  size_t top_ = 0;
};

}  // namespace

ZF_REGISTER_SCREEN("menu.game.stats", [](Ctx& cx, const std::string&) -> std::unique_ptr<Screen> { return std::make_unique<StatsScreen>(cx); });

}  // namespace zf
