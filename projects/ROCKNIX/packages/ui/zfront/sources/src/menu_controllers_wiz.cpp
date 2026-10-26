// The two full-screen screens of the controller section: the device picker (ES GuiDetectDevice) and the mapping wizard (ES
// GuiInputConfig). Both listen to the pad through `Pad::set_capture`; everything proportional to the screen like the menus.
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "i18n.hpp"
#include "menu_controllers.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "scene.hpp"
#include "util.hpp"

namespace zf::ctl {

namespace {

constexpr float kIdleCancelSeconds = 90.0f;  // a wizard nobody touches for this long closes without saving
constexpr float kProgressTick = 0.05f;       // redraw pace of the hold-to-skip bar

const void* g_tap_owner = nullptr;  // the PadTap whose sink is installed (only one at a time)

// Sentence case for labels the framework does not build itself: first letter upper case, the rest lower case, but single
// letters (A, B, X, Y) and words with digits (L2, R3) keep their case.
std::string sentence(std::string_view s) {
  std::string out;
  bool cap = true;  // start of a sentence
  size_t i = 0;
  while (i < s.size()) {
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '/') ++j;
    std::string_view w = s.substr(i, j - i);
    // a lone B/X/Y is a button name; a lone "A" is the article (ES: "WITH A CONTROLLER", "HOLD A BUTTON")
    bool keep = (w.size() == 1 && w[0] != 'A') || std::any_of(w.begin(), w.end(), [](char c) { return c >= '0' && c <= '9'; });
    for (size_t k = 0; k < w.size(); ++k) {
      char c = w[k];
      if (!keep && !(cap && k == 0) && c >= 'A' && c <= 'Z') c = char(c + 32);
      out.push_back(c);
    }
    if (!w.empty()) cap = w.back() == '.' || w.back() == '!' || w.back() == '?';
    if (j < s.size()) out.push_back(s[j]);
    i = j + 1;
  }
  return out;
}

void centred(std::vector<Op>& out, const ui::Txt& t, float cx, float cy, Color tint) { ui::put_text(out, t, cx - t.w / 2.0f, cy, tint); }

void set_text(Assets& a, ui::Txt& t, std::string& shown, const std::string& s, int px, float max_w) {
  if (shown != s || !t.alive(a)) {
    shown = s;
    t.set(a, s, px, max_w);
  }
}

const ui::Prompt kHelpReview[] = {{"A", N_("OK")}, {"B", N_("CANCEL")}};
const ui::Prompt kHelpReviewHotkey[] = {{"A", N_("SET SELECT AS HOTKEY")}, {"X", N_("DO NOT ASSIGN HOTKEY")}, {"B", N_("CANCEL")}};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// helpers shared with the menus

std::vector<JoyInfo> connected_pads(Ctx& cx) {
  if (cx.pad) return cx.pad->joysticks();
  std::vector<JoyInfo> v;
  if (std::getenv("ZF_CTL_DEMO")) {  // harness only: two fake pads so the screens can be reviewed
    v.push_back({0, "19009b4d4b4800000111000000010000", "retrogame_joypad", 18, 0, 4});
    v.push_back({1, "030000005e040000120b000001000000", "Xbox Wireless Controller", 15, 1, 6});
  }
  return v;
}

bool pad_is_configured(const JoyInfo& j) {
  PadConfig c;
  c.load_file(paths().user_input);
  char buf[512];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) {
    buf[n] = 0;
    c.load_file(path_join(path_dir(buf), "es_input.cfg"));
  }
  return c.find(j.guid, j.name) >= 0;
}

void PadTap::install(Ctx& cx, CaptureSink sink) {
  if (!cx.pad) return;
  pad_ = cx.pad;
  pad_->set_capture(std::move(sink));
  g_tap_owner = this;
}

void PadTap::release() {
  if (pad_ && g_tap_owner == this) {
    pad_->set_capture({});
    g_tap_owner = nullptr;
  }
  pad_ = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// DetectScreen

namespace {
const ui::Prompt kHelpDetect[] = {{"B", N_("CANCEL")}};
}

DetectScreen::DetectScreen(Ctx&) {}

bool DetectScreen::on_capture(const CaptureEvent& e) {
  if (e.kind != InKind::Button) return false;
  if (e.value && !holding_ && !picked_) {
    holding_ = true;
    held_dev_ = e.dev;
    held_guid_ = *e.guid;
    held_name_ = *e.name;
    held_s_ = 0;
    dirty_ = true;
  } else if (!e.value && holding_ && e.dev == held_dev_) {
    holding_ = false;
    held_s_ = 0;
    dirty_ = true;
  }
  oscontract::input(Clock::now());
  return false;  // never consumed: a pad that has a mapping still navigates (B cancels)
}

Transition DetectScreen::handle(Action a, Ctx& cx) {
  if (!installed_) {
    installed_ = true;
    tap_.install(cx, [this](const CaptureEvent& e) { return on_capture(e); });
  }
  if (a == Action::B || a == Action::Start || a == Action::Hotkey) return Transition::pop();
  return Transition::none();
}

bool DetectScreen::tick(float dt, Ctx& cx) {
  if (!installed_) {
    installed_ = true;
    tap_.install(cx, [this](const CaptureEvent& e) { return on_capture(e); });
  }
  bool changed = dirty_;
  dirty_ = false;
  if (holding_) {
    held_s_ += dt;
    changed = true;
    if (held_s_ >= kHoldSeconds) {
      holding_ = false;
      picked_ = true;
    }
  }
  return changed;
}

std::optional<float> DetectScreen::wake_in() const {
  if (holding_) return kProgressTick;
  if (picked_) return 0.0f;
  return std::nullopt;
}

Transition DetectScreen::take_transition(Ctx& cx) {
  if (!picked_) return Transition::none();
  picked_ = false;
  int b = -1, h = -1, ax = -1;
  for (const JoyInfo& j : connected_pads(cx))
    if (j.guid == held_guid_ && j.name == held_name_) {
      b = j.buttons, h = j.hats, ax = j.axes;
      break;
    }
  // Replace: B in the wizard goes back to the page that opened the picker. The new screen installs its own sink on its
  // first tick, after this one is gone (see PadTap).
  tap_.release();
  return Transition::replace(std::make_unique<ConfigScreen>(cx, held_guid_, held_name_, b, h, ax));
}

void DetectScreen::draw(Ctx& cx, std::vector<Op>& out) {
  if (!installed_) tick(0, cx);
  Assets& a = cx.assets;
  if (lay_w_ != cx.w || lay_h_ != cx.h) {
    m_ = ui::Metrics::from(cx.th, cx.w, cx.h);
    st_ = ui::Style::from(cx.th);
    lay_w_ = cx.w, lay_h_ = cx.h;
    t_title_.reset(), t_count_.reset(), t_msg_.reset(), t_msg2_.reset(), t_held_.reset();
    shown_count_ = size_t(-1);
    shown_held_.clear();
  }
  const ui::Metrics& m = m_;
  const ui::Style& st = st_;
  float inset = std::round(m.pill_h * ui::kPillInset);
  out.push_back(Op::rect(0, 0, m.W, m.H, st.bg));
  if (!t_title_.alive(a)) t_title_.set(a, sentence(tr("CONFIGURE INPUT")), m.title_px, m.col_w * 0.72f);
  ui::put_text(out, t_title_, m.col_x + inset, m.header_cy, st.txt);
  status_bar(cx.th, a, m.W, m.H, cx.status, out);
  float title_bottom = m.header_cy + t_title_.h / 2.0f;
  float first_pill_top = m.list_top + (m.row_h - m.pill_h) / 2.0f;
  out.push_back(Op::rect(m.col_x, std::round((title_bottom + first_pill_top) / 2.0f), m.col_w, 1, st.line));

  // pad count (re-read on every layout, hot-plug while the screen is open shows after the next resize/redraw)
  size_t n = connected_pads(cx).size();
  if (n != shown_count_ || !t_count_.alive(a)) {
    shown_count_ = n;
    std::string s = n == 0 ? sentence(tr("NO GAMEPADS DETECTED")) : std::to_string(n) + " " + sentence(n == 1 ? tr("GAMEPAD DETECTED") : tr("GAMEPADS DETECTED"));
    t_count_.set(a, s, m.small_px, m.col_w - 2 * inset);
  }
  if (!t_msg_.alive(a)) t_msg_.set_wrap(a, sentence(tr("HOLD A BUTTON ON YOUR DEVICE TO CONFIGURE IT.")), m.em, m.col_w - 2 * inset, 3, 1.25f);
  if (!t_msg2_.alive(a)) t_msg2_.set(a, sentence(tr("PRESS B TO CANCEL.")), m.small_px, m.col_w - 2 * inset);
  if (shown_held_ != held_name_ || !t_held_.alive(a)) {
    shown_held_ = held_name_;
    t_held_.set(a, held_name_, ui::px_floor(float(m.em) * 1.2f), m.col_w - 2 * inset);
  }

  float cxm = m.W / 2.0f;
  float y0 = m.list_top, L = m.list_h;
  centred(out, t_count_, cxm, y0 + 0.16f * L, st.dim);
  centred(out, t_msg_, cxm, y0 + 0.42f * L, st.txt);
  centred(out, t_msg2_, cxm, y0 + 0.62f * L, st.dim);
  if (holding_ || picked_) {
    centred(out, t_held_, cxm, y0 + 0.78f * L, st.cyan_dark);
    float bw = std::round(m.col_w * 0.4f), bh = std::max(std::round(0.012f * m.H), 3.0f);
    float bx = std::round(cxm - bw / 2.0f), by = std::round(y0 + 0.88f * L);
    shapes_.pill(a, out, bx, by, bw, bh, st.track);
    float f = std::min(held_s_ / kHoldSeconds, 1.0f);
    if (picked_) f = 1.0f;
    if (f > 0) shapes_.pill(a, out, bx, by, std::max(std::round(bw * f), bh), bh, st.cyan);
  }
  help_.draw(a, cx.th, m, st, kHelpDetect, 1, out);
}

// ---------------------------------------------------------------------------------------------------------------
// ConfigScreen

ConfigScreen::ConfigScreen(Ctx& cx, std::string guid, std::string name, int buttons, int hats, int axes)
    : guid_(std::move(guid)), name_(std::move(name)), wiz_(buttons, hats, axes) {
  invert_ = cx.es.get_bool("InvertButtons", true);
}

void ConfigScreen::review_press(const char* name) {
  // ES: BUTTON_OK = invert ? "a" : "b", BUTTON_BACK = invert ? "b" : "a" (names of the NEW mapping)
  std::string ok = invert_ ? "a" : "b", back = invert_ ? "b" : "a";
  if (name == ok) want_ = Want::SaveSelect;
  else if (name == back) want_ = Want::Cancel;
  else if (std::string_view(name) == "x" && !wiz_.has_hotkey()) want_ = Want::SaveNone;
}

bool ConfigScreen::on_capture(const CaptureEvent& e) {
  if (*e.guid != guid_ || *e.name != name_) return false;
  TimePoint now = Clock::now();
  bool was_asleep = oscontract::idle().active();
  oscontract::input(now);  // our consumed events never reach the loop's activity stamp
  idle_s_ = 0;
  if (was_asleep) {
    oscontract::screensaver_stop();  // the first press only wakes the panel
    return true;
  }
  needs_redraw_ = true;
  if (wiz_.phase() == InputWizard::Phase::Rows) {
    wiz_.input(e.kind, e.id, e.value);
  } else if (e.value != 0) {
    if (const char* n = wiz_.resolve(e.kind, e.id, e.value)) review_press(n);
  }
  return true;
}

bool ConfigScreen::save(Ctx& cx, Hotkey choice) {
  std::string err;
  const std::string path = paths().user_input;
  if (!write_device_config(path, guid_, name_, wiz_.result(choice), err)) {
    cx.sv->toasts.push(tr("Could not save the controller configuration") + std::string(": ") + err, 4000);
    return false;
  }
  saved_ = true;
  if (cx.pad) cx.pad->reload_config();
  // ES: Scripting::fireEvent config-changed / controls-changed, then the `onfinish` commands of es_input.cfg
  oscontract::fire_event("config-changed");
  oscontract::fire_event("controls-changed");
  std::vector<std::string> cmds = onfinish_commands(path);
  if (!cmds.empty()) {
    std::shared_ptr<Services> sv = cx.sv;
    cx.sv->jobs.post([sv, cmds] {
      for (const std::string& c : cmds) sv->cmd.run(c, 30000);
    });
  }
  cx.sv->toasts.push(tr("Controller configuration saved"));
  return true;
}

Transition ConfigScreen::handle(Action a, Ctx& cx) {
  if (!drawn_once_) {
    drawn_once_ = true;
    tap_.install(cx, [this](const CaptureEvent& e) { return on_capture(e); });
  }
  idle_s_ = 0;
  if (a == Action::B || a == Action::Start) return Transition::pop();
  if (wiz_.phase() == InputWizard::Phase::Rows) {
    // keyboard / another pad: X skips a row that may be skipped, Left goes back one row
    if (a == Action::X) wiz_.skip_row();
    else if (a == Action::Left || a == Action::Y) wiz_.back_row();
    return Transition::none();
  }
  if (a == Action::A) {
    if (save(cx, Hotkey::Select)) return Transition::pop();
  } else if (a == Action::X && !wiz_.has_hotkey()) {
    if (save(cx, Hotkey::None)) return Transition::pop();
  } else if (a == Action::Left || a == Action::Y) {
    wiz_.back_row();
  }
  return Transition::none();
}

bool ConfigScreen::tick(float dt, Ctx& cx) {
  if (!drawn_once_) {
    drawn_once_ = true;
    tap_.install(cx, [this](const CaptureEvent& e) { return on_capture(e); });
  }
  idle_s_ += dt;
  if (idle_s_ >= kIdleCancelSeconds && want_ == Want::None) {
    want_ = Want::Cancel;
    cx.sv->toasts.push(tr("Controller configuration cancelled"));
  }
  wiz_.tick(dt);
  bool changed = wiz_.take_changed() || needs_redraw_;
  needs_redraw_ = false;
  return changed;
}

std::optional<float> ConfigScreen::wake_in() const {
  if (want_ != Want::None) return 0.0f;
  float w = std::max(kIdleCancelSeconds - idle_s_, 0.0f) + 0.01f;
  if (wiz_.holding() && wiz_.skippable(wiz_.row()) && wiz_.phase() == InputWizard::Phase::Rows) w = std::min(w, kProgressTick);
  return w;
}

Transition ConfigScreen::take_transition(Ctx& cx) {
  Want w = want_;
  want_ = Want::None;
  switch (w) {
    case Want::None: return Transition::none();
    case Want::Cancel: return Transition::pop();
    case Want::SaveSelect: return save(cx, Hotkey::Select) ? Transition::pop() : Transition::none();
    case Want::SaveNone: return save(cx, Hotkey::None) ? Transition::pop() : Transition::none();
  }
  return Transition::none();
}

// ---- drawing ----------------------------------------------------------------------------------------------------------

namespace {

struct R {
  float x, y, w, h;
};

// The console, in units of its body height `s`. Part geometry for each wizard row (index = slot).
struct Diagram {
  float bx, by, s;  // top-left of the body, unit length
  R at(float x, float y, float w, float h) const { return {bx + x * s, by + y * s, w * s, h * s}; }
};

void part(Assets& a, ui::Shapes& sh, std::vector<Op>& out, const Diagram& d, size_t slot, Color c) {
  auto bar = [&](float x, float y, float w, float h, float rad) {
    R r = d.at(x, y, w, h);
    sh.rrect(a, out, r.x, r.y, r.w, r.h, int(std::round(rad * d.s)), c);
  };
  auto dot = [&](float cx, float cy, float dia) {
    R r = d.at(cx - dia / 2, cy - dia / 2, dia, dia);
    sh.disc(a, out, r.x, r.y, std::max(int(std::round(r.w)), 2), c);
  };
  const float dpx = 0.32f, dpy = 0.47f, abx = 1.88f, aby = 0.47f, lsx = 0.32f, rsx = 1.88f, sty = 0.84f;
  switch (slot) {
    case 0: bar(dpx - 0.045f, dpy - 0.15f, 0.09f, 0.11f, 0.02f); break;  // up
    case 1: bar(dpx - 0.045f, dpy + 0.04f, 0.09f, 0.11f, 0.02f); break;  // down
    case 2: bar(dpx - 0.15f, dpy - 0.045f, 0.11f, 0.09f, 0.02f); break;  // left
    case 3: bar(dpx + 0.04f, dpy - 0.045f, 0.11f, 0.09f, 0.02f); break;  // right
    case 4: bar(1.22f - 0.10f, 0.865f, 0.20f, 0.07f, 0.035f); break;     // start
    case 5: bar(0.98f - 0.10f, 0.865f, 0.20f, 0.07f, 0.035f); break;     // select
    case 6: dot(abx + 0.17f, aby, 0.15f); break;                         // a / east
    case 7: dot(abx, aby + 0.17f, 0.15f); break;                         // b / south
    case 8: dot(abx, aby - 0.17f, 0.15f); break;                         // x / north
    case 9: dot(abx - 0.17f, aby, 0.15f); break;                         // y / west
    case 10: bar(0.10f, -0.05f, 0.52f, 0.09f, 0.04f); break;             // left shoulder
    case 11: bar(1.58f, -0.05f, 0.52f, 0.09f, 0.04f); break;             // right shoulder
    case 12: bar(0.16f, -0.17f, 0.40f, 0.10f, 0.045f); break;            // left trigger
    case 13: bar(1.64f, -0.17f, 0.40f, 0.10f, 0.045f); break;            // right trigger
    case 14: dot(lsx, sty, 0.17f); break;                                // left thumb (press)
    case 15: dot(rsx, sty, 0.17f); break;                                // right thumb
    case 16: bar(lsx - 0.045f, sty - 0.125f, 0.09f, 0.035f, 0.015f); break;
    case 17: bar(lsx - 0.045f, sty + 0.09f, 0.09f, 0.035f, 0.015f); break;
    case 18: bar(lsx - 0.125f, sty - 0.045f, 0.035f, 0.09f, 0.015f); break;
    case 19: bar(lsx + 0.09f, sty - 0.045f, 0.035f, 0.09f, 0.015f); break;
    case 20: bar(1.10f - 0.12f, 0.02f, 0.24f, 0.06f, 0.03f); break;      // hotkey
    case 21: bar(rsx - 0.045f, sty - 0.125f, 0.09f, 0.035f, 0.015f); break;
    case 22: bar(rsx - 0.045f, sty + 0.09f, 0.09f, 0.035f, 0.015f); break;
    case 23: bar(rsx - 0.125f, sty - 0.045f, 0.035f, 0.09f, 0.015f); break;
    case 24: bar(rsx + 0.09f, sty - 0.045f, 0.035f, 0.09f, 0.015f); break;
    default: break;
  }
}

}  // namespace

void ConfigScreen::draw(Ctx& cx, std::vector<Op>& out) {
  if (!drawn_once_) {
    drawn_once_ = true;
    tap_.install(cx, [this](const CaptureEvent& e) { return on_capture(e); });
  }
  Assets& a = cx.assets;
  if (lay_w_ != cx.w || lay_h_ != cx.h) {
    m_ = ui::Metrics::from(cx.th, cx.w, cx.h);
    st_ = ui::Style::from(cx.th);
    lay_w_ = cx.w, lay_h_ = cx.h;
    t_title_.reset(), t_dev_.reset(), t_label_.reset(), t_state_.reset(), t_note_.reset(), t_sum_.reset();
    shown_label_.clear(), shown_state_.clear(), shown_note_.clear(), shown_sum_.clear();
  }
  const ui::Metrics& m = m_;
  const ui::Style& st = st_;
  float inset = std::round(m.pill_h * ui::kPillInset);
  out.push_back(Op::rect(0, 0, m.W, m.H, st.bg));
  if (!t_title_.alive(a)) t_title_.set(a, sentence(tr("CONFIGURING")), m.title_px, m.col_w * 0.72f);
  ui::put_text(out, t_title_, m.col_x + inset, m.header_cy, st.txt);
  status_bar(cx.th, a, m.W, m.H, cx.status, out);
  float title_bottom = m.header_cy + t_title_.h / 2.0f;
  float first_pill_top = m.list_top + (m.row_h - m.pill_h) / 2.0f;
  out.push_back(Op::rect(m.col_x, std::round((title_bottom + first_pill_top) / 2.0f), m.col_w, 1, st.line));

  const float y0 = m.list_top, L = m.list_h, cxm = m.W / 2.0f;
  const bool review = wiz_.phase() == InputWizard::Phase::Review;

  // device name
  if (!t_dev_.alive(a)) t_dev_.set(a, name_, m.small_px, m.col_w - 2 * inset);
  centred(out, t_dev_, cxm, y0 + 0.04f * L, st.dim);

  // the console
  float box_top = y0 + 0.10f * L, box_h = 0.52f * L;
  float s = std::floor(std::min(box_h / 1.2f, (m.col_w - 2 * inset) / 2.2f));
  Diagram d{std::round(cxm - 1.1f * s), std::round(box_top + (box_h - 1.2f * s) / 2.0f + 0.2f * s), s};
  Color body = st.key, base = st.key_alt;
  {
    R r = d.at(0, 0, 2.2f, 1.0f);
    shapes_.rrect(a, out, r.x, r.y, r.w, r.h, int(std::round(0.18f * s)), body);
    R sc = d.at(0.55f, 0.10f, 1.10f, 0.68f);
    shapes_.rrect(a, out, sc.x, sc.y, sc.w, sc.h, int(std::round(0.05f * s)), st.bg);
    // the parts that are never asked for: d-pad centre, stick bases
    R c = d.at(0.32f - 0.045f, 0.47f - 0.045f, 0.09f, 0.09f);
    out.push_back(Op::rect(std::round(c.x), std::round(c.y), std::round(c.w), std::round(c.h), base));
    for (float sx : {0.32f, 1.88f}) {
      R b = d.at(sx - 0.085f, 0.84f - 0.085f, 0.17f, 0.17f);
      shapes_.disc(a, out, b.x, b.y, std::max(int(std::round(b.w)), 2), ui::with_alpha(base, 0.55f));
    }
  }
  for (size_t i = 0; i < kSlotCount; ++i) {
    Color c = base;
    switch (wiz_.state(i)) {
      case RowState::Set: c = st.dark ? st.cyan_soft : ui::with_alpha(st.cyan, 0.6f); break;
      case RowState::Skipped: c = ui::with_alpha(base, 0.40f); break;
      case RowState::Pending: break;
    }
    if (!review && i == wiz_.row()) c = st.cyan;
    part(a, shapes_, out, d, i, c);
  }

  // progress strip
  {
    float total = std::round(2.2f * s);
    float gap = std::max(std::round(0.003f * m.W), 1.0f);
    float seg = std::floor((total - gap * float(kSlotCount - 1)) / float(kSlotCount));
    if (seg >= 2) {
      float th = std::max(std::round(0.012f * m.H), 3.0f);
      float x = std::round(cxm - (seg * float(kSlotCount) + gap * float(kSlotCount - 1)) / 2.0f);
      float y = std::round(y0 + 0.66f * L);
      for (size_t i = 0; i < kSlotCount; ++i) {
        Color c = st.key_alt;
        if (wiz_.state(i) == RowState::Set) c = st.cyan;
        else if (wiz_.state(i) == RowState::Skipped) c = ui::with_alpha(st.dim, 0.35f);
        if (!review && i == wiz_.row()) c = st.txt;
        out.push_back(Op::rect(x + float(i) * (seg + gap), y, seg, th, c));
      }
    }
  }

  // prompt
  int big = ui::px_floor(float(m.em) * 1.2f);
  std::string label, state, note;
  Color state_col = st.dim;
  if (!review) {
    label = sentence(i18n::ctr("joystick", kSlots[wiz_.row()].label));
    if (wiz_.taken()) state = sentence(tr("ALREADY TAKEN")), state_col = st.danger;
    else if (!wiz_.holding() || !wiz_.skippable(wiz_.row())) state = sentence(tr("PRESS ANYTHING"));
    if (wiz_.skippable(wiz_.row())) note = sentence(tr("HOLD ANY BUTTON TO SKIP"));
  } else {
    label = sentence(tr("ALL BUTTONS ARE DONE"));
    state = std::to_string(wiz_.count(RowState::Set)) + " " + tr("set") + ", " + std::to_string(wiz_.count(RowState::Skipped)) + " " + tr("skipped");
    if (!wiz_.has_hotkey()) note = sentence(tr("NO HOTKEY BUTTON HAS BEEN ASSIGNED. THIS IS REQUIRED FOR EXITING GAMES WITH A CONTROLLER."));
  }
  set_text(a, t_label_, shown_label_, label, big, m.col_w - 2 * inset);
  centred(out, t_label_, cxm, y0 + 0.77f * L, st.cyan_dark);
  if (!state.empty()) {
    set_text(a, t_state_, shown_state_, state, m.small_px, m.col_w - 2 * inset);
    centred(out, t_state_, cxm, y0 + 0.855f * L, state_col);
  } else if (wiz_.holding()) {  // hold-to-skip bar where the state line was
    float bw = std::round(m.col_w * 0.3f), bh = std::max(std::round(0.012f * m.H), 3.0f);
    float bx = std::round(cxm - bw / 2.0f), by = std::round(y0 + 0.855f * L - bh / 2.0f);
    shapes_.pill(a, out, bx, by, bw, bh, st.track);
    float f = wiz_.hold_progress();
    if (f > 0) shapes_.pill(a, out, bx, by, std::max(std::round(bw * f), bh), bh, st.cyan);
  }
  if (!note.empty()) {
    if (shown_note_ != note || !t_note_.alive(a)) {
      shown_note_ = note;
      t_note_.set_wrap(a, note, m.small_px, m.col_w - 2 * inset, 3, 1.2f);
    }
    centred(out, t_note_, cxm, y0 + 0.855f * L + std::round(0.075f * L) + t_note_.h / 2.0f - float(m.small_px) * 0.5f, st.dim);
  }

  if (review) {
    if (wiz_.has_hotkey()) help_.draw(a, cx.th, m, st, kHelpReview, 2, out);
    else help_.draw(a, cx.th, m, st, kHelpReviewHotkey, 3, out);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// registration

ZF_REGISTER_SCREEN("menu.controllers.detect", [](Ctx& cx, const std::string&) { return std::make_unique<DetectScreen>(cx); });

namespace {
std::unique_ptr<Screen> make_config(Ctx& cx, const std::string& arg) {
  size_t sp = arg.find(' ');
  if (sp == std::string::npos || sp == 0) return nullptr;
  std::string guid = arg.substr(0, sp), name = arg.substr(sp + 1);
  int b = -1, h = -1, ax = -1;
  for (const JoyInfo& j : connected_pads(cx))
    if (j.guid == guid && j.name == name) {
      b = j.buttons, h = j.hats, ax = j.axes;
      break;
    }
  return std::make_unique<ConfigScreen>(cx, std::move(guid), std::move(name), b, h, ax);
}
}  // namespace

ZF_REGISTER_SCREEN("menu.controllers.config", [](Ctx& cx, const std::string& arg) { return make_config(cx, arg); });

}  // namespace zf::ctl
