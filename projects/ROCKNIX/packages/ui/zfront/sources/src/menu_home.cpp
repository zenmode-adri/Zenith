// `menu.home`: the in-game HOME panel (homemenu.hpp). It only exists inside the `zfront --home-panel` child that the frontend starts
// while a game is paused; its answer to the parent is the exit code (`homemenu::set_panel_result` + `exit_code_of`).
//
// What the user sees (variant A of mockups/menu-home.html): the paused game as the background (the picture the parent took with grim, passed as
// `--home-shot`), darkened to ~55 %, and over it a side panel on the left with the Zenith DARK look whatever the theme (the palette of the bedside
// clock, dock_screen.cpp). The panel slides in over ~0.18 s (not at all when `Ctx::ui_motion` is off). Top to bottom:
//
//   <game name>                     --home-title
//   <system>               <battery>  --home-sub, read_battery
//   RESUME                  (the cursor starts here; B and START do the same)
//   SAVE STATE / LOAD STATE only with the argument "states" (RetroArch with network_cmd_enable on; the parent decides)
//   ---
//   SCREEN BRIGHTNESS       slider, same sysfs write and `system.brightness` setting as the SCREEN menu
//   SYSTEM VOLUME           slider, the `volume` script through `sound::set_volume`
//   ---
//   QUIT GAME               red; asks first, like SAVE STATE and LOAD STATE (the question replaces the rows inside the panel, same palette)
//
// Own screen instead of a `Menu`: `Menu` and its skins lay out one full-width column, take their colours from the active theme and open their
// confirmations as `ConfirmDialog`s in the theme's colours (over this dark panel they would be a light dialog on a dark panel). So this follows
// dock_screen.cpp: every size is whole cells of `remake::Grid` or a `ui::Level` text size (TEXT SIZE included), boxes are reported to the layout
// auditor, texts are `ui::Txt` slots that are rendered again only when their text or size changes (no allocation on a warm frame).
// The rows scroll when TEXT SIZE is large and they do not fit. HIGH CONTRAST: opaque panel, white ink.
//
// Every way out ends the child (`Transition::quit()`): the parent resumes the paused game whatever happens, so a crash here is harmless.
#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

#include "app.hpp"
#include "homemenu.hpp"
#include "i18n.hpp"
#include "menu_sound.hpp"
#include "menu_system.hpp"
#include "motion.hpp"
#include "registry.hpp"
#include "remake.hpp"
#include "status.hpp"
#include "ui_prefs.hpp"
#include "ui_style.hpp"
#include "util.hpp"

namespace zf {

namespace {

using homemenu::RowId;
using remake::Grid;

// ---- the look: fixed, the same on every theme ---------------------------------------------------------------------------------------
constexpr Color kPanelBg = {22, 27, 34, 235};  // kPanel of the clock, at the boceto's .92
constexpr Color kHi = {230, 233, 238, 255};    // rows and title: the boceto's #e6e9ee
constexpr Color kTxt = {176, 184, 196, 255};   // kTxt of the clock: system, battery, help
constexpr Color kDim = {104, 112, 124, 255};   // kDim of the clock
constexpr Color kAccent = {34, 211, 238, 255}; // kAccent of the clock (#22d3ee)
constexpr Color kLine = {255, 255, 255, 28};   // kLine of the clock
constexpr Color kTrack = {255, 255, 255, 44};  // the empty part of a slider
constexpr Color kDanger = {255, 123, 123, 255};  // QUIT GAME (#ff7b7b)
constexpr Color kNoShot = {20, 24, 30, 255};   // behind the panel when the parent could not take a picture
constexpr Color kWhite = {255, 255, 255, 255};
constexpr float kSlide = 0.18f;                // seconds the panel takes to come in
constexpr float kCommitDelay = 0.5f;           // seconds without adjusting before the brightness is saved to the settings
constexpr uint8_t kDimAlpha = 140;             // ~55 % black over the game

struct Backlight {
  std::string path;
  int max = 0, pct = 100;
};

// `ZF_BATTERY_ROOT` is the tests' fake root (the same one the battery screens read); on the console it is not set.
std::string sys_root() {
  const char* r = std::getenv("ZF_BATTERY_ROOT");
  return std::string(r ? r : "") + "/sys";
}

Backlight first_backlight() {
  Backlight b;
  if (DIR* d = ::opendir((sys_root() + "/class/backlight").c_str())) {
    std::vector<std::string> names;
    while (const dirent* e = ::readdir(d))
      if (e->d_name[0] != '.') names.emplace_back(e->d_name);
    ::closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
      const std::string dir = sys_root() + "/class/backlight/" + n;
      std::string mx, cur;
      long max = 0, c = 0;
      if (!read_file(dir + "/max_brightness", mx) || !parse_int(trim(mx), max) || max <= 0) continue;
      if (read_file(dir + "/brightness", cur)) parse_int(trim(cur), c);
      b.path = dir + "/brightness";
      b.max = int(max);
      b.pct = std::min(100, std::max(5, int(float(c) / float(max) * 100.0f + 0.5f)));
      break;
    }
  }
  return b;
}

// A text image kept between frames: rendered again only when the text, the size or the Assets change.
struct Line {
  uint64_t owner = 0;
  std::string s;
  int px = 0;
  float max_w = 0;
  ui::Txt t;
  const ui::Txt& set(Assets& a, std::string_view str, int px_, float max_w_ = 0.0f) {
    if (owner != a.uid() || px != px_ || max_w != max_w_ || s != str || !t.alive(a)) {
      owner = a.uid();
      s.assign(str);
      px = px_;
      max_w = max_w_;
      t.set(a, str, px_, max_w_);
    }
    return t;
  }
};

// The same for a word-wrapped block.
struct WrapLine {
  uint64_t owner = 0;
  std::string s;
  int px = 0;
  float max_w = 0;
  ui::Txt t;
  const ui::Txt& set(Assets& a, std::string_view str, int px_, float max_w_) {
    if (owner != a.uid() || px != px_ || max_w != max_w_ || s != str || !t.alive(a)) {
      owner = a.uid();
      s.assign(str);
      px = px_;
      max_w = max_w_;
      t.set_wrap(a, str, px_, max_w_, 6, 1.25f);
    }
    return t;
  }
};

struct Row {
  RowId id = RowId::Resume;
  bool slider = false, danger = false;
  std::string label;
  int min = 0, max = 100, v = 0;
  std::string vtxt;
  Line l_label, l_val;
  bool selectable() const { return id != RowId::Separator; }
};

enum class Ask : uint8_t { None, Save, Load, Quit };

// What depends on the screen, TEXT SIZE and the language: recomputed when one changes.
struct Lay {
  uint64_t owner = 0;
  float w = 0, h = 0;
  unsigned gen = ~0u, lang = ~0u;
  float u = 0, mx = 0;
  float pw = 0, cx0 = 0, cw = 0, inset = 0;
  int px_title = 0, px_row = 0, px_small = 0, px_help = 0, px_foot = 0;
  float row_h = 0, slider_h = 0, sep_h = 0, title_h = 0, meta_h = 0, help_h = 0, chip_h = 0;
  float y_title = 0, y_meta = 0, y_list = 0, list_h = 0, y_help = 0;
  float sys_max_w = 0, val_w = 0;
  bool bat_long = false;
};

class HomePanel : public Screen {
 public:
  explicit HomePanel(bool states) : states_(states), info_(homemenu::panel_info()) {
    if (std::optional<Battery> b = read_battery(sys_root())) {
      bat_ = *b;
      have_bat_ = true;
    }
    bl_ = first_backlight();
  }

  Profile repeat_profile() const override { return kMenu; }  // bounded: a held direction never reaches a row per frame
  bool is_menu() const override { return true; }

  Transition handle(Action a, Ctx& cx) override {
    ensure_rows(cx);
    if (ask_ != Ask::None) return handle_ask(a, cx);
    switch (a) {
      case Action::B:
      case Action::Start:
        return finish(homemenu::PanelResult::Resume, cx);
      case Action::Up:
        move(-1);
        break;
      case Action::Down:
        move(1);
        break;
      case Action::Left:
        if (cur_ >= 0 && rows_[size_t(cur_)].slider) adjust(rows_[size_t(cur_)], -1, cx);
        break;
      case Action::Right:
        if (cur_ >= 0 && rows_[size_t(cur_)].slider) adjust(rows_[size_t(cur_)], 1, cx);
        break;
      case Action::A:
        if (cur_ >= 0) return activate(rows_[size_t(cur_)], cx);
        break;
      default:
        break;
    }
    return Transition::none();
  }

  bool tick(float dt, Ctx& cx) override {
    ensure_rows(cx);
    start(cx);
    bool moving = false;
    if (t_ < 1.0f && drawn_) {  // the clock of the slide starts with the first frame: what happened before it (loading) does not eat it
      t_ = std::min(1.0f, t_ + dt / kSlide);
      moving = t_ < 1.0f;
    } else if (t_ < 1.0f) {
      moving = true;
    }
    if (pending_) {
      pending_left_ -= dt;
      if (pending_left_ <= 0) flush_commit(cx);
    }
    return moving;
  }

  std::optional<float> wake_in() const override {
    if (!pending_) return std::nullopt;
    return std::max(pending_left_, 0.0f) + 0.002f;
  }

  void on_dismiss(Ctx& cx) override { flush_commit(cx); }

  void draw(Ctx& cx, std::vector<Op>& out) override {
    ensure_rows(cx);
    start(cx);
    load_shot(cx);
    drawn_ = true;
    ensure_layout(cx);
    Assets& a = cx.assets;
    const bool hc = ui::access().contrast;
    const float e = cx.ui_motion ? motion::ease_out(t_) : 1.0f;
    const float dx = std::round(-(1.0f - e) * L_.pw);
    const bool rest = dx == 0.0f;  // only a panel at rest is measured by the layout auditor (it is off the margins while it slides)
    const bool audit = rest && remake::audit_sink() != nullptr;
    const Color ink = hc ? kWhite : kHi, soft = hc ? kWhite : kTxt;

    if (bg_)
      out.push_back(Op::image(bg_, 0, 0, cx.w, cx.h));
    else
      out.push_back(Op::rect(0, 0, cx.w, cx.h, kNoShot));
    out.push_back(Op::rect(0, 0, cx.w, cx.h, Color{8, 10, 14, uint8_t(float(kDimAlpha) * e)}));
    out.push_back(Op::rect(dx, 0, L_.pw, cx.h, hc ? Color{22, 27, 34, 255} : kPanelBg));
    out.push_back(Op::rect(dx + L_.pw - 1.0f, 0, 1, cx.h, hc ? Color{255, 255, 255, 90} : kLine));

    const float tx = dx + L_.cx0 + L_.inset, text_w = L_.cw - 2.0f * L_.inset;
    {  // the game
      const ui::Txt& t = l_title_.set(a, title_s_, L_.px_title, text_w);
      ui::put_text(out, t, tx, L_.y_title + L_.title_h / 2.0f, ink);
      if (audit && !t.blank) remake::audit(L_.cx0 + L_.inset, L_.y_title + (L_.title_h - t.h) / 2.0f, t.w, t.h, "titulo", 'L', false);
    }
    {  // system on the left, battery on the right
      const float cy = L_.y_meta + L_.meta_h / 2.0f;
      const std::string& bs = L_.bat_long ? bat_long_s_ : bat_short_s_;
      if (have_bat_) {
        const ui::Txt& b = l_bat_.set(a, bs, L_.px_small);
        ui::put_text_right(out, b, dx + L_.cx0 + L_.cw - L_.inset, cy, bat_.charging && !hc ? kAccent : soft);
        if (audit && !b.blank) remake::audit(L_.cx0 + L_.cw - L_.inset - b.w, cy - b.h / 2.0f, b.w, b.h, "valor de bateria", 'R', false);
      }
      if (!sub_s_.empty()) {
        const ui::Txt& s = l_sub_.set(a, sub_s_, L_.px_small, L_.sys_max_w);
        ui::put_text(out, s, tx, cy, soft);
        if (audit && !s.blank) remake::audit(L_.cx0 + L_.inset, cy - s.h / 2.0f, s.w, s.h, "\"sistema\"", 'L', false);
      }
    }

    out.push_back(Op::clip(dx, L_.y_list, L_.pw, L_.list_h));
    if (ask_ == Ask::None)
      draw_rows(cx, out, dx, audit, hc);
    else
      draw_ask(cx, out, dx, audit, hc);
    out.push_back(Op::unclip());

    if (ui_prefs::help_prompts()) draw_help(cx, out, dx, audit, hc);
  }

 private:
  // ---- state ------------------------------------------------------------------------------------------------------------------------
  bool states_;
  homemenu::PanelInfo info_;
  Battery bat_;
  bool have_bat_ = false;
  Backlight bl_;
  std::vector<Row> rows_;
  int cur_ = -1;
  Ask ask_ = Ask::None;
  int ask_cur_ = 1;  // 0 YES, 1 NO: the focus starts on NO
  float t_ = 0.0f;   // 0..1 through the slide
  bool started_ = false, drawn_ = false, shot_tried_ = false;
  ImgId bg_ = 0;
  // brightness setting waiting for the quiet period
  bool pending_ = false;
  float pending_left_ = 0.0f;
  int pending_v_ = 0;
  // geometry and texts
  Lay L_;
  std::vector<float> top_, hgt_;
  float content_h_ = 0, scroll_ = 0;
  std::string title_s_, sub_s_, bat_short_s_, bat_long_s_;
  std::string q_save_, q_load_, q_quit_, yes_s_, no_s_, k_a_, k_b_, k_lr_, h_select_, h_resume_, h_adjust_, h_back_;
  Line l_title_, l_sub_, l_bat_, l_yes_, l_no_, l_ka_, l_kb_, l_klr_, l_hselect_, l_hresume_, l_hadjust_, l_hback_;
  WrapLine l_q_;
  ui::Shapes sh_;

  // ---- setup ------------------------------------------------------------------------------------------------------------------------
  void start(const Ctx& cx) {
    if (started_) return;
    started_ = true;
    if (!cx.ui_motion) t_ = 1.0f;
  }

  // The picture of the game is decoded here, on the first frame and before it is shown (not on the job thread): the first frame already has it.
  void load_shot(Ctx& cx) {
    if (shot_tried_) return;
    shot_tried_ = true;
    if (info_.shot.empty()) return;
    ImgId id = 0;
    if (cx.assets.file(info_.shot, id)) {
      cx.assets.pin(id);
      bg_ = id;
    }
  }

  void ensure_rows(Ctx& cx) {
    if (!rows_.empty()) return;
    for (RowId id : homemenu::panel_rows(states_, !bl_.path.empty())) {
      Row r;
      r.id = id;
      if (id == RowId::Brightness) {
        r.slider = true;
        r.min = 5;
        r.v = bl_.pct;
      } else if (id == RowId::Volume) {
        r.slider = true;
        r.v = std::min(100, std::max(0, sound::volume(cx)));
      }
      r.danger = id == RowId::Quit;
      if (r.slider) r.vtxt = std::to_string(r.v) + "%";
      rows_.push_back(std::move(r));
    }
    cur_ = 0;  // RESUME
    texts_gen_ = ~0u;
  }
  unsigned texts_gen_ = ~0u;

  static std::string shown(const char* msg) { return i18n::sentence_case(tr(msg)); }

  void build_texts() {
    for (Row& r : rows_) {
      switch (r.id) {
        case RowId::Resume: r.label = shown(N_("RESUME")); break;
        case RowId::SaveState: r.label = shown(N_("SAVE STATE")); break;
        case RowId::LoadState: r.label = shown(N_("LOAD STATE")); break;
        case RowId::Brightness: r.label = shown(N_("SCREEN BRIGHTNESS")); break;
        case RowId::Volume: r.label = shown(N_("SYSTEM VOLUME")); break;
        case RowId::Quit: r.label = shown(N_("QUIT GAME")); break;
        case RowId::Separator: break;
      }
    }
    title_s_ = info_.title.empty() ? shown(N_("HOME MENU")) : info_.title;
    sub_s_ = info_.sub;
    if (have_bat_) {
      bat_short_s_ = std::to_string(int(bat_.percent)) + "%";
      bat_long_s_ = bat_.charging ? bat_short_s_ + " " + shown(N_("CHARGING")) : bat_short_s_;
      // with the mean rate of this game (measured by the parent from the start of the session): "62% · ~2 h 40 min" when it fits
      if (!bat_.charging && info_.left_min >= 0) {
        const int m = info_.left_min;
        char b[48];
        if (m >= 60) std::snprintf(b, sizeof b, "~%d h %d min", m / 60, m % 60);
        else std::snprintf(b, sizeof b, "~%d min", m);
        bat_long_s_ = bat_short_s_ + " \xC2\xB7 " + b;
      }
    }
    q_save_ = shown(N_("OVERWRITE THE SAVE STATE?"));
    q_load_ = shown(N_("LOAD THE SAVE STATE? UNSAVED PROGRESS IS LOST."));
    q_quit_ = shown(N_("REALLY QUIT THE GAME?"));
    yes_s_ = shown(N_("YES"));
    no_s_ = shown(N_("NO"));
    k_a_ = "A", k_b_ = "B", k_lr_ = "< >";  // names of the buttons: not translated
    h_select_ = shown(N_("SELECT"));
    h_resume_ = shown(N_("RESUME"));
    h_adjust_ = shown(N_("ADJUST"));
    h_back_ = shown(N_("BACK"));
  }

  void ensure_layout(Ctx& cx) {
    Assets& a = cx.assets;
    if (L_.owner == a.uid() && L_.w == cx.w && L_.h == cx.h && L_.gen == ui::access().gen && L_.lang == i18n::generation()) return;
    if (texts_gen_ != i18n::generation()) {
      build_texts();
      texts_gen_ = i18n::generation();
    }
    const Grid g(cx.w, cx.h);
    L_ = Lay();
    L_.owner = a.uid();
    L_.w = cx.w;
    L_.h = cx.h;
    L_.gen = ui::access().gen;
    L_.lang = i18n::generation();
    const float u = float(g.u);
    L_.u = u;
    L_.mx = float(g.mx);
    // about half the screen (the boceto: 46 %); a 480-wide screen needs more for the longest labels at the common text sizes
    const float frac = cx.w <= 480.0f ? 0.60f : 0.50f;
    L_.cx0 = L_.mx;
    L_.cw = std::max(std::floor((frac * cx.w - 2.0f * L_.mx) / u), 12.0f) * u;
    L_.pw = L_.cw + 2.0f * L_.mx;
    L_.inset = 2.0f * u;
    L_.px_title = g.L(ui::Level::Title);
    L_.px_row = g.L(ui::Level::Row);
    L_.px_small = g.L(ui::Level::Small);
    L_.px_help = g.L(ui::Level::Help);
    L_.px_foot = g.L(ui::Level::Foot);
    const int th = g.cells_over(float(L_.px_row) * 1.25f);  // a line of row text, in cells
    L_.row_h = float(th + 4) * u;
    L_.slider_h = float(th + 6) * u;  // two cells over the text, a cell, the track, two cells under
    L_.sep_h = 3.0f * u;
    L_.title_h = float(g.cells_over(float(L_.px_title) * 1.3f)) * u;
    L_.meta_h = float(g.cells_over(float(L_.px_small) * 1.4f)) * u;
    L_.help_h = float(std::max(3, g.cells_over(float(L_.px_help) * 1.7f))) * u;
    L_.chip_h = std::min(L_.help_h, float(g.cells_over(float(L_.px_foot) * 1.5f)) * u);
    L_.y_title = float(g.top());
    L_.y_meta = L_.y_title + L_.title_h;
    L_.y_list = L_.y_meta + L_.meta_h + 2.0f * u;
    L_.y_help = float(g.bottom()) - L_.help_h;
    L_.list_h = std::max(L_.y_help - 2.0f * u - L_.y_list, 0.0f);
    // the battery goes on the right of the system's line; with "CHARGING" if the system's name still has room
    const float text_w = L_.cw - 2.0f * L_.inset;
    const float sub_w = sub_s_.empty() ? 0.0f : a.measure(sub_s_, float(L_.px_small));
    L_.bat_long = false;
    float bat_w = 0.0f;
    if (have_bat_) {
      bat_w = a.measure(bat_short_s_, float(L_.px_small));
      const float long_w = a.measure(bat_long_s_, float(L_.px_small));
      if (long_w > bat_w && sub_w + long_w + 2.0f * u <= text_w) L_.bat_long = true, bat_w = long_w;
    }
    L_.val_w = a.measure("100%", float(L_.px_row));
    L_.sys_max_w = std::max(text_w - bat_w - (have_bat_ ? 2.0f * u : 0.0f), u);
    // the rows
    top_.assign(rows_.size(), 0.0f);
    hgt_.assign(rows_.size(), 0.0f);
    float y = 0;
    for (size_t i = 0; i < rows_.size(); ++i) {
      top_[i] = y;
      hgt_[i] = rows_[i].id == RowId::Separator ? L_.sep_h : rows_[i].slider ? L_.slider_h : L_.row_h;
      y += hgt_[i];
    }
    content_h_ = y;
    reveal();
  }

  // ---- input ------------------------------------------------------------------------------------------------------------------------
  void move(int dir) {
    const int n = int(rows_.size());
    int i = cur_;
    for (int k = 0; k < n; ++k) {
      i = (i + dir + n) % n;
      if (rows_[size_t(i)].selectable()) {
        cur_ = i;
        break;
      }
    }
    reveal();
  }

  // The view moves by the least that shows the row under the cursor completely (the first row brings the top of the list with it).
  void reveal() {
    if (cur_ < 0 || size_t(cur_) >= top_.size() || L_.list_h <= 0) return;
    const float t = top_[size_t(cur_)], b = t + hgt_[size_t(cur_)];
    if (t < scroll_) scroll_ = t;
    if (b > scroll_ + L_.list_h) scroll_ = b - L_.list_h;
    scroll_ = std::min(std::max(scroll_, 0.0f), std::max(content_h_ - L_.list_h, 0.0f));
    if (cur_ == 0) scroll_ = 0;
  }

  void adjust(Row& r, int dir, Ctx& cx) {
    const int v = std::min(std::max(r.v + dir, r.min), r.max);
    if (v == r.v) return;
    r.v = v;
    r.vtxt = std::to_string(v) + "%";
    if (r.id == RowId::Brightness) {
      bl_.pct = v;
      msys::sysfs_write(cx, bl_.path, std::to_string(v * bl_.max / 100));  // like the OS script `brightness set`
      pending_ = true;
      pending_v_ = v;
      pending_left_ = kCommitDelay;
    } else if (r.id == RowId::Volume) {
      sound::set_volume(cx, v);
    }
  }

  void flush_commit(Ctx& cx) {
    if (!pending_) return;
    pending_ = false;
    pending_left_ = 0.0f;
    msys::put(cx, "system.brightness", std::to_string(pending_v_), true);
  }

  Transition finish(homemenu::PanelResult r, Ctx& cx) {
    homemenu::set_panel_result(r);
    flush_commit(cx);  // a brightness that has not been written yet
    // the volume script runs on the job thread: give it a moment to finish before this process ends
    for (int i = 0; i < 100 && !cx.sv->jobs.idle(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      cx.sv->jobs.drain();
    }
    return Transition::quit();
  }

  Transition activate(Row& r, Ctx& cx) {
    switch (r.id) {
      case RowId::Resume: return finish(homemenu::PanelResult::Resume, cx);
      case RowId::SaveState: ask_ = Ask::Save; break;
      case RowId::LoadState: ask_ = Ask::Load; break;
      case RowId::Quit: ask_ = Ask::Quit; break;
      default: return Transition::none();  // a slider: A does nothing
    }
    ask_cur_ = 1;
    return Transition::none();
  }

  Transition handle_ask(Action a, Ctx& cx) {
    switch (a) {
      case Action::B:
        ask_ = Ask::None;
        break;
      case Action::Up:
      case Action::Down:
      case Action::Left:
      case Action::Right:
        ask_cur_ = 1 - ask_cur_;
        break;
      case Action::A: {
        const Ask what = ask_;
        ask_ = Ask::None;
        if (ask_cur_ != 0) break;  // NO
        return finish(what == Ask::Save ? homemenu::PanelResult::SaveState : what == Ask::Load ? homemenu::PanelResult::LoadState : homemenu::PanelResult::Quit, cx);
      }
      default:
        break;
    }
    return Transition::none();
  }

  // ---- drawing ----------------------------------------------------------------------------------------------------------------------
  // The row's box: the selected one has the accent ring and the accent at 8 % inside (the boceto's .row.sel).
  void row_box(Assets& a, std::vector<Op>& out, float x, float y, float w, float h, bool sel, bool hc) {
    if (!sel) return;
    const int r = int(std::min(h / 2.0f, 2.0f * L_.u));
    const int t = std::max(1, int(std::lround(L_.u * 0.4f)));
    sh_.rrect(a, out, x, y, w, h, r, Color{kAccent[0], kAccent[1], kAccent[2], uint8_t(hc ? 56 : 20)});
    sh_.ring(a, out, x, y, w, h, r, float(t), kAccent);
  }

  void draw_rows(Ctx& cx, std::vector<Op>& out, float dx, bool audit, bool hc) {
    Assets& a = cx.assets;
    const float u = L_.u, x = dx + L_.cx0, lx = x + L_.inset;
    const Color ink = hc ? kWhite : kHi;
    for (size_t i = 0; i < rows_.size(); ++i) {
      Row& r = rows_[i];
      const float y = std::round(L_.y_list + top_[i] - scroll_), h = hgt_[i];
      if (y + h <= L_.y_list || y >= L_.y_list + L_.list_h) continue;
      const bool whole = y >= L_.y_list - 0.5f && y + h <= L_.y_list + L_.list_h + 0.5f;
      if (r.id == RowId::Separator) {
        out.push_back(Op::rect(lx, std::round(y + h / 2.0f), L_.cw - 2.0f * L_.inset, 1, hc ? Color{255, 255, 255, 90} : kLine));
        continue;
      }
      const bool sel = int(i) == cur_;
      row_box(a, out, x, y, L_.cw, h, sel, hc);
      const Color c = r.danger ? kDanger : sel ? kAccent : ink;
      const ui::Txt& t = r.l_label.set(a, r.label, L_.px_row, L_.cw - 2.0f * L_.inset - (r.slider ? L_.val_w + 2.0f * u : 0.0f));
      const bool reported = audit && whole;
      if (reported) remake::audit(L_.cx0, y, L_.cw, h, "tarjeta", 'B', false, true);
      if (!r.slider) {
        ui::put_text(out, t, lx, y + h / 2.0f, c);
        if (reported && !t.blank) remake::audit(L_.cx0 + L_.inset, y + (h - t.h) / 2.0f, t.w, t.h, "\"" + r.label.substr(0, 18) + "\"", 'L', false);
        continue;
      }
      // a slider: the name and the number on one line, the track under it
      const float ty = y + 2.0f * u, th = h - 6.0f * u;  // th: the text line (cells) of the row
      const float cy = ty + th / 2.0f;
      ui::put_text(out, t, lx, cy, c);
      const ui::Txt& v = r.l_val.set(a, r.vtxt, L_.px_row);
      ui::put_text_right(out, v, x + L_.cw - L_.inset, cy, sel ? kAccent : (hc ? kWhite : kTxt));
      const float tw = L_.cw - 2.0f * L_.inset, tx = lx, trk_y = ty + th + u, trk_h = u;
      sh_.pill(a, out, tx, trk_y, tw, trk_h, hc ? Color{255, 255, 255, 90} : kTrack);
      const float fw = std::floor(tw * float(r.v - r.min) / float(std::max(r.max - r.min, 1)));
      if (fw >= trk_h)
        sh_.pill(a, out, tx, trk_y, fw, trk_h, kAccent);
      else if (fw > 0.0f)
        out.push_back(Op::rect(tx, trk_y, fw, trk_h, kAccent));
      if (reported) {
        if (!t.blank) remake::audit(L_.cx0 + L_.inset, cy - t.h / 2.0f, t.w, t.h, "\"" + r.label.substr(0, 18) + "\"", 'L', false);
        if (!v.blank) remake::audit(L_.cx0 + L_.cw - L_.inset - v.w, cy - v.h / 2.0f, v.w, v.h, "valor de \"" + r.label.substr(0, 18) + "\"", 'R', false);
        remake::audit(tx - dx, trk_y, tw, trk_h, "barra", 'B', false, false);
      }
    }
  }

  // The question in place of the rows: the text and, under it, YES and NO as two rows (the focus starts on NO).
  void draw_ask(Ctx& cx, std::vector<Op>& out, float dx, bool audit, bool hc) {
    Assets& a = cx.assets;
    const float u = L_.u, x = dx + L_.cx0, lx = x + L_.inset;
    const std::string& q = ask_ == Ask::Save ? q_save_ : ask_ == Ask::Load ? q_load_ : q_quit_;
    const ui::Txt& t = l_q_.set(a, q, L_.px_row, L_.cw - 2.0f * L_.inset);
    const float y0 = L_.y_list;
    if (!t.blank) out.push_back(Op::image(t.id, std::round(lx), y0, t.w, t.h, hc ? kWhite : kHi));
    if (audit && !t.blank && y0 + t.h <= L_.y_list + L_.list_h) remake::audit(L_.cx0 + L_.inset, y0, t.w, t.h, "\"" + q.substr(0, 18) + "\"", 'L', false);
    float y = y0 + std::ceil(t.h / u) * u + 2.0f * u;
    for (int k = 0; k < 2; ++k) {
      const bool sel = ask_cur_ == k;
      const std::string& s = k == 0 ? yes_s_ : no_s_;
      Line& l = k == 0 ? l_yes_ : l_no_;
      row_box(a, out, x, y, L_.cw, L_.row_h, sel, hc);
      const ui::Txt& lt = l.set(a, s, L_.px_row, L_.cw - 2.0f * L_.inset);
      const Color c = (k == 0 && ask_ == Ask::Quit) ? kDanger : sel ? kAccent : (hc ? kWhite : kHi);
      ui::put_text(out, lt, lx, y + L_.row_h / 2.0f, c);
      if (audit && y + L_.row_h <= L_.y_list + L_.list_h) {
        remake::audit(L_.cx0, y, L_.cw, L_.row_h, "tarjeta", 'B', false, true);
        if (!lt.blank) remake::audit(L_.cx0 + L_.inset, y + (L_.row_h - lt.h) / 2.0f, lt.w, lt.h, "\"" + s.substr(0, 18) + "\"", 'L', false);
      }
      y += L_.row_h;
    }
  }

  // "A Select   B Resume" (with "< > Adjust" first on a slider; the confirmation has "A Select   B Back"): a keycap ring and the word.
  struct Item {
    Line* key;
    const std::string* key_s;
    Line* label;
    const std::string* label_s;
  };
  void draw_help(Ctx& cx, std::vector<Op>& out, float dx, bool audit, bool hc) {
    Assets& a = cx.assets;
    Item items[3];
    int n = 0;
    if (ask_ != Ask::None) {
      items[n++] = {&l_ka_, &k_a_, &l_hselect_, &h_select_};
      items[n++] = {&l_kb_, &k_b_, &l_hback_, &h_back_};
    } else {
      if (cur_ >= 0 && rows_[size_t(cur_)].slider) items[n++] = {&l_klr_, &k_lr_, &l_hadjust_, &h_adjust_};
      items[n++] = {&l_ka_, &k_a_, &l_hselect_, &h_select_};
      items[n++] = {&l_kb_, &k_b_, &l_hresume_, &h_resume_};
    }
    const float u = L_.u, chip_h = L_.chip_h, pad = u, gap_in = 2.0f * u, gap_out = 4.0f * u;
    float widths[3];
    float total = 0;
    int first = 0;
    for (int i = 0; i < n; ++i) {
      const ui::Txt& k = items[i].key->set(a, *items[i].key_s, L_.px_foot);
      const ui::Txt& l = items[i].label->set(a, *items[i].label_s, L_.px_help);
      widths[i] = std::max(chip_h, std::ceil((k.w + 2.0f * pad) / u) * u) + gap_in + l.w;
    }
    auto sum = [&](int from) {
      float s = 0;
      for (int i = from; i < n; ++i) s += widths[i] + (i > from ? gap_out : 0.0f);
      return s;
    };
    const float room = L_.cw - 2.0f * L_.inset;
    while (first < n - 1 && sum(first) > room) ++first;  // what does not fit is left out, from the first
    total = sum(first);
    float x = dx + L_.cx0 + L_.inset;
    const float cy = L_.y_help + L_.help_h / 2.0f;
    if (audit) remake::audit(L_.cx0 + L_.inset, L_.y_help, total, L_.help_h, "ayuda", 'C', false);
    for (int i = first; i < n; ++i) {
      const ui::Txt& k = items[i].key->set(a, *items[i].key_s, L_.px_foot);
      const ui::Txt& l = items[i].label->set(a, *items[i].label_s, L_.px_help);
      const float cw = std::max(chip_h, std::ceil((k.w + 2.0f * pad) / u) * u);
      sh_.ring(a, out, x, std::round(cy - chip_h / 2.0f), cw, chip_h, int(chip_h / 2.0f), float(std::max(1, int(std::lround(u * 0.4f)))), hc ? kWhite : kTxt);
      ui::put_text(out, k, std::round(x + (cw - k.w) / 2.0f), cy, hc ? kWhite : kHi);
      ui::put_text(out, l, std::round(x + cw + gap_in), cy, hc ? kWhite : kTxt);
      x += widths[i] + gap_out;
    }
  }
};

}  // namespace

ZF_REGISTER_SCREEN("menu.home", [](Ctx&, const std::string& arg) { return std::make_unique<HomePanel>(arg == "states"); });

}  // namespace zf
