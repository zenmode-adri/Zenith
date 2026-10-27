#include "menu.hpp"
#include "motion.hpp"

#include <algorithm>
#include <cmath>

#include "i18n.hpp"
#include "registry.hpp"
#include "scene.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf {

using ui::Icon;
using ui::Prompt;

struct Menu::Token {
  bool rebuild = false;  // `refresh()` was called
  bool values = false;   // a dialog changed a value: re-read the getters
};

namespace {

constexpr float kCommitDelay = 0.5f;  // seconds without adjusting before a slider's `commit` runs

const Prompt kHelpAction[] = {{"updown", N_("CHOOSE")}, {"A", N_("SELECT")}, {"B", N_("BACK")}};
const Prompt kHelpToggle[] = {{"updown", N_("CHOOSE")}, {"A", N_("CHANGE")}, {"B", N_("BACK")}};
const Prompt kHelpSlider[] = {{"updown", N_("CHOOSE")}, {"leftright", N_("ADJUST")}, {"B", N_("BACK")}};
const Prompt kHelpChoice[] = {{"updown", N_("CHOOSE")}, {"leftright", N_("CHANGE")}, {"A", N_("SELECT")}, {"B", N_("BACK")}};
const Prompt kHelpPlain[] = {{"updown", N_("CHOOSE")}, {"B", N_("BACK")}};

bool selectable_kind(EntryKind k) { return k != EntryKind::Info && k != EntryKind::Header && k != EntryKind::Separator; }

std::string entry_key(const Entry& e) { return e.id.empty() ? e.label : e.id; }

}  // namespace

// ---- builder ---------------------------------------------------------------------------------------------------------

Entry& MenuBuilder::push(EntryKind k, std::string label) {
  out_.emplace_back();
  Entry& e = out_.back();
  e.kind = k;
  e.label = std::move(label);
  return e;
}

Entry& MenuBuilder::add_action(std::string label, Act a) {
  Entry& e = push(EntryKind::Action, std::move(label));
  e.on_a = std::move(a);
  return e;
}

Entry& MenuBuilder::submenu(std::string label, std::string screen, std::string arg) {
  Entry& e = push(EntryKind::Action, std::move(label));
  e.show_arrow = true;
  e.hidden = !Registry::instance().has(screen);
  e.on_a = [screen = std::move(screen), arg = std::move(arg)](Ctx&) { return Transition::open(screen, arg); };
  return e;
}

Entry& MenuBuilder::toggle(std::string label, std::function<bool()> get, std::function<void(bool)> set) {
  Entry& e = push(EntryKind::Toggle, std::move(label));
  e.get_b = std::move(get);
  e.set_b = std::move(set);
  return e;
}

Entry& MenuBuilder::slider(std::string label, int min, int max, int step, std::function<int()> get, std::function<void(int)> set) {
  Entry& e = push(EntryKind::Slider, std::move(label));
  e.min = min;
  e.max = std::max(max, min);
  e.step = std::max(step, 1);
  e.i = min;
  e.get_i = std::move(get);
  e.set_i = std::move(set);
  return e;
}

Entry& MenuBuilder::choice(std::string label, std::vector<Option> options, std::function<std::string()> get, std::function<void(const std::string&)> set) {
  Entry& e = push(EntryKind::Choice, std::move(label));
  e.options = std::move(options);
  e.get_s = std::move(get);
  e.set_s = std::move(set);
  e.show_arrow = true;
  return e;
}

Entry& MenuBuilder::text(std::string label, std::function<std::string()> get, std::function<void(const std::string&)> set) {
  Entry& e = push(EntryKind::Text, std::move(label));
  e.get_s = std::move(get);
  e.set_s = std::move(set);
  return e;
}

Entry& MenuBuilder::info(std::string label, std::string value) {
  Entry& e = push(EntryKind::Info, std::move(label));
  e.value_text = std::move(value);
  return e;
}

Entry& MenuBuilder::header(std::string label) { return push(EntryKind::Header, std::move(label)); }
Entry& MenuBuilder::separator() { return push(EntryKind::Separator, std::string()); }

std::unique_ptr<Screen> make_menu(MenuSpec spec) { return std::make_unique<Menu>(std::move(spec)); }

// ---- Menu: state -----------------------------------------------------------------------------------------------------

Menu::Menu(MenuSpec spec) : spec_(std::move(spec)), tok_(std::make_shared<Token>()), lang_(i18n::language()) {}

void Menu::set_title(std::string t) {
  spec_.title = std::move(t);
  spec_.title_key = nullptr;
  t_title_.reset();
}
Menu::~Menu() = default;

Refresh Menu::refresher() {
  std::weak_ptr<Token> w = tok_;
  return [w] {
    if (std::shared_ptr<Token> t = w.lock()) t->rebuild = true;
  };
}

bool Menu::focusable(size_t i) const { return i < e_.size() && e_[i].enabled && selectable_kind(e_[i].kind); }

void Menu::refresh_entry(Entry& e) {
  switch (e.kind) {
    case EntryKind::Toggle:
      e.b = e.get_b ? e.get_b() : false;
      break;
    case EntryKind::Slider: {
      int v = e.get_i ? e.get_i() : e.i;
      v = std::min(std::max(v, e.min), e.max);
      if (v != e.i || e.value_text.empty()) {
        e.i = v;
        e.value_text = e.fmt ? e.fmt(v) : std::to_string(v) + e.unit_text;
        e.t_value.reset();
        e.t_label.reset();
        e.value_w = 0;
      }
      break;
    }
    case EntryKind::Choice: {
      std::string cur = e.get_s ? e.get_s() : std::string();
      int idx = -1;
      for (size_t k = 0; k < e.options.size(); ++k)
        if (e.options[k].value == cur) idx = int(k);
      std::string shown = idx >= 0 ? e.options[size_t(idx)].label : cur;
      if (idx != e.sel || shown != e.value_text) {
        e.sel = idx;
        e.value_text = shown;
        e.t_value.reset();
        e.t_label.reset();
      }
      break;
    }
    case EntryKind::Text: {
      std::string cur = e.get_s ? e.get_s() : std::string();
      if (cur != e.value_text) {
        e.value_text = cur;
        e.t_value.reset();
        e.t_label.reset();
      }
      break;
    }
    default:
      break;
  }
}

void Menu::refresh_values() {
  for (Entry& e : e_) refresh_entry(e);
}

void Menu::rebuild(Ctx& cx) {
  std::string keep;
  size_t keep_idx = 0;
  bool had = cur_ >= 0 && size_t(cur_) < e_.size();
  if (had) {
    keep = entry_key(e_[size_t(cur_)]);
    keep_idx = size_t(cur_);
  }
  e_.clear();
  MenuBuilder b(cx, e_, refresher());
  if (spec_.build) spec_.build(b, cx);
  e_.erase(std::remove_if(e_.begin(), e_.end(), [](const Entry& e) { return e.hidden; }), e_.end());
  has_icons_ = false;
  for (const Entry& e : e_) has_icons_ = has_icons_ || !e.icon_name.empty();
  refresh_values();
  tok_->rebuild = false;
  cur_ = -1;
  if (had) {
    for (size_t i = 0; i < e_.size(); ++i)
      if (focusable(i) && entry_key(e_[i]) == keep) {
        cur_ = int(i);
        break;
      }
    if (cur_ < 0) {  // the entry vanished: nearest selectable one at or after the old position
      for (size_t i = std::min(keep_idx, e_.size()); i < e_.size(); ++i)
        if (focusable(i)) {
          cur_ = int(i);
          break;
        }
      for (size_t i = std::min(keep_idx, e_.size()); cur_ < 0 && i-- > 0;)
        if (focusable(i)) cur_ = int(i);
    }
  }
  if (cur_ < 0)
    for (size_t i = 0; i < e_.size(); ++i)
      if (focusable(i)) {
        cur_ = int(i);
        break;
      }
  have_layout_ = false;
  hint_for_ = -2;
  layout(cx);
  retarget(false);
}

bool Menu::sync(Ctx& cx) {
  bool changed = false;
  if (lang_ != i18n::language()) {  // the interface language changed under an open menu: entries and title follow
    lang_ = i18n::language();
    if (spec_.title_key) spec_.title = tr(spec_.title_key);
    t_title_.reset();
    if (opened_) tok_->rebuild = true;
    changed = true;
  }
  if (!opened_) {
    opened_ = true;
    rebuild(cx);
    if (spec_.on_open) spec_.on_open(cx, refresher());
    changed = true;
  }
  if (tok_->rebuild) {
    rebuild(cx);
    changed = true;
  }
  if (tok_->values) {
    tok_->values = false;
    refresh_values();
    changed = true;
  }
  if (!have_layout_ || cx.w != lay_w_ || cx.h != lay_h_) {
    layout(cx);
    changed = true;
  }
  return changed;
}

void Menu::layout(Ctx& cx) {
  m_ = ui::Metrics::from(cx.th, cx.w, cx.h);
  st_ = ui::Style::from(cx.th);
  lay_w_ = cx.w, lay_h_ = cx.h;
  top_.assign(e_.size(), 0.0f);
  hgt_.assign(e_.size(), 0.0f);
  float y = 0;
  for (size_t i = 0; i < e_.size(); ++i) {
    float h = m_.row_h;
    if (e_[i].kind == EntryKind::Header) h = std::round(m_.row_h * 0.9f);
    if (e_[i].kind == EntryKind::Separator) h = std::round(m_.row_h * 0.4f);
    top_[i] = y;
    hgt_[i] = h;
    y += h;
  }
  content_h_ = y;
  for (Entry& e : e_) e.t_label.reset(), e.t_value.reset(), e.t_icon.reset(), e.value_w = 0;
  t_title_.reset();
  t_hint_.reset();
  hint_for_ = -2;
  have_layout_ = true;
  retarget(!placed_);
}

void Menu::retarget(bool snap) {
  float vp = m_.list_h;
  float maxs = std::max(content_h_ - vp, 0.0f);
  if (cur_ >= 0 && size_t(cur_) < top_.size()) {
    float c = top_[size_t(cur_)] + hgt_[size_t(cur_)] / 2.0f;
    float raw = std::min(std::max(c - vp / 2.0f, 0.0f), maxs);
    float t = raw;
    if (raw < maxs) {  // start the view on an entry boundary: no half-cut row at the top
      t = 0;
      for (size_t i = 0; i < top_.size(); ++i) {
        if (top_[i] <= raw + 0.5f) t = top_[i];
        else break;
      }
    }
    scroll_t_ = t;
    sel_y_t_ = top_[size_t(cur_)];
  } else {
    scroll_t_ = std::min(std::max(scroll_t_, 0.0f), maxs);
  }
  if (snap || !placed_) {
    scroll_ = scroll_t_;
    sel_y_ = sel_y_t_;
    placed_ = true;
  }
}

void Menu::move(int dir, bool wrap) {
  if (cur_ < 0 || e_.empty()) return;
  int n = int(e_.size());
  int i = cur_;
  for (int k = 0; k < n; ++k) {
    i += dir;
    if (i < 0 || i >= n) {
      if (!wrap) return;
      i = (i + n) % n;
    }
    if (focusable(size_t(i))) {
      if (i != cur_) {
        flush_commit();
        cur_ = i;
        hint_for_ = -2;
        retarget(false);
      }
      return;
    }
  }
}

void Menu::page(int dir) {
  int steps = std::max(m_.lines - 1, 1);
  for (int k = 0; k < steps; ++k) {
    int before = cur_;
    move(dir, false);
    if (cur_ == before) break;
  }
}

void Menu::flush_commit() {
  if (!pending_) return;
  std::function<void()> f = std::move(pending_);
  pending_ = nullptr;
  pending_left_ = 0;
  f();
}

std::optional<float> Menu::wake_in() const {
  if (!pending_) return std::nullopt;
  return std::max(pending_left_, 0.0f) + 0.002f;
}

bool Menu::tick(float dt, Ctx& cx) {
  bool moving = sync(cx);
  if (!placed_) return moving;
  bool a = ui::approach(scroll_, scroll_t_, dt);
  bool b = ui::approach(sel_y_, sel_y_t_, dt);
  if (pending_) {
    pending_left_ -= dt;
    if (pending_left_ <= 0) flush_commit();
  }
  bool knobs = false;
  if (cx.ui_motion) {  // the knob of a switch slides to its new side
    for (Entry& e : e_) {
      if (e.kind != EntryKind::Toggle) continue;
      const float to = e.b ? 1.0f : 0.0f;
      if (e.kn < 0.0f) {
        e.kn = to;
      } else if (e.kn != to) {
        e.kn = motion::approach(e.kn, to, kKnobRate, dt);
        if (std::fabs(e.kn - to) < 0.02f) e.kn = to;
        knobs = true;
      }
    }
  }
  return moving || a || b || knobs;
}

// ---- Menu: input -----------------------------------------------------------------------------------------------------

void Menu::adjust(Entry& e, int dir, Ctx& cx) {
  (void)cx;
  switch (e.kind) {
    case EntryKind::Toggle: {
      bool want = dir > 0;
      if (e.b != want) {
        e.b = want;
        if (e.set_b) e.set_b(want);
        refresh_values();
      }
      break;
    }
    case EntryKind::Slider: {
      int v = std::min(std::max(e.i + dir * e.step, e.min), e.max);
      if (v != e.i) {
        e.i = v;
        if (e.set_i) e.set_i(v);
        if (e.commit_i) {
          std::function<void(int)> f = e.commit_i;
          pending_ = [f, v] { f(v); };
          pending_left_ = kCommitDelay;
        }
        e.value_text.clear();  // forces the text to be rebuilt even if the setter clamps
        refresh_values();
      }
      break;
    }
    case EntryKind::Choice: {
      int n = int(e.options.size());
      if (!e.cycle || n < 2) break;
      int idx = e.sel < 0 ? (dir > 0 ? 0 : n - 1) : (e.sel + dir + n) % n;
      if (e.set_s) e.set_s(e.options[size_t(idx)].value);
      refresh_values();
      break;
    }
    default:
      break;
  }
}

Transition Menu::activate(Entry& e, Ctx& cx) {
  flush_commit();
  switch (e.kind) {
    case EntryKind::Action: {
      if (!e.on_a) return Transition::none();
      if (!e.confirm_msg.empty()) {
        ui::ConfirmOpts o;
        o.danger = e.is_danger;
        return ui::confirm(e.confirm_msg, e.on_a, o);
      }
      Transition t = e.on_a(cx);
      tok_->values = true;
      return t;
    }
    case EntryKind::Toggle:
      adjust(e, e.b ? -1 : 1, cx);
      break;
    case EntryKind::Choice: {
      if (e.options.empty()) break;
      auto set = e.set_s;
      auto tok = tok_;
      std::string cur = e.sel >= 0 ? e.options[size_t(e.sel)].value : std::string();
      return ui::choose(e.label, e.options, cur, [set, tok](Ctx&, const std::string& v) {
        if (set) set(v);
        tok->values = true;
      });
    }
    case EntryKind::Text: {
      auto set = e.set_s;
      auto tok = tok_;
      ui::KeyboardOpts o;
      o.title = e.label;
      o.password = e.password;
      return ui::keyboard(o, e.get_s ? e.get_s() : std::string(), [set, tok](Ctx&, const std::string& v) {
        if (set) set(v);
        tok->values = true;
      });
    }
    default:
      break;
  }
  return Transition::none();
}

Transition Menu::leave(Ctx& cx) {
  flush_commit();
  if (spec_.on_close) spec_.on_close(cx);
  return Transition::pop();
}

void Menu::on_dismiss(Ctx& cx) {
  flush_commit();
  if (spec_.on_close) spec_.on_close(cx);
}

Transition Menu::handle(Action a, Ctx& cx) {
  sync(cx);
  switch (a) {
    case Action::Up:
    case Action::Down:
      if (cur_ < 0) {  // nothing selectable: scroll
        float maxs = std::max(content_h_ - m_.list_h, 0.0f);
        scroll_t_ = std::min(std::max(scroll_t_ + (a == Action::Down ? 1.0f : -1.0f) * m_.row_h, 0.0f), maxs);
      } else {
        move(a == Action::Down ? 1 : -1, true);
      }
      break;
    case Action::L:
    case Action::R:
      if (cur_ < 0) {
        float maxs = std::max(content_h_ - m_.list_h, 0.0f);
        scroll_t_ = std::min(std::max(scroll_t_ + (a == Action::R ? 1.0f : -1.0f) * (m_.list_h - m_.row_h), 0.0f), maxs);
      } else {
        page(a == Action::R ? 1 : -1);
      }
      break;
    case Action::Left:
    case Action::Right:
      if (cur_ >= 0) adjust(e_[size_t(cur_)], a == Action::Right ? 1 : -1, cx);
      break;
    case Action::A:
      if (cur_ >= 0) return activate(e_[size_t(cur_)], cx);
      break;
    case Action::B:
      return leave(cx);
    case Action::Start:  // like ES: the whole menu stack closes (each level gets `on_dismiss`)
      return Transition::close_menus();
    default:
      return on_other(a, cx);
  }
  return Transition::none();
}

// ---- Menu: drawing ---------------------------------------------------------------------------------------------------

void Menu::draw_row(Ctx& cx, std::vector<Op>& out, size_t idx, float y, bool sel, float vis) {
  Assets& a = cx.assets;
  Entry& e = e_[idx];
  const ui::Metrics& m = m_;
  const ui::Style& st = st_;
  const int em = m.em;
  float h = hgt_[idx];
  float cy = y + h / 2.0f;
  float inset = std::round(m.pill_h * ui::kPillInset);
  float xl = m.col_x + inset, xr = m.col_x + m.col_w - inset;
  float inner = xr - xl;

  if (e.kind == EntryKind::Separator) {
    out.push_back(Op::rect(xl, std::round(cy), inner, 1, st.line));
    return;
  }
  if (e.kind == EntryKind::Header) {
    if (!e.t_label.alive(a)) e.t_label.set(a, e.label, m.small_px, inner);
    // a header cut by the edge of the list would show half a line of text: it fades with the part that is visible
    ui::put_text(out, e.t_label, xl, y + h * 0.66f, vis >= 1.0f ? st.group_txt : ui::with_alpha(st.group_txt, vis * vis * vis));
    return;
  }

  bool dimmed = !e.enabled;
  float fade = sel ? 0.60f : 0.42f;  // dimmed entries: a little stronger under the cursor so they stay readable
  Color lab = dimmed ? ui::with_alpha(sel ? st.cyan_dark : st.txt, fade) : (e.is_danger ? st.danger : (sel ? st.cyan_dark : st.txt));
  Color val = dimmed ? ui::with_alpha(sel ? st.cyan_dark : st.dim, fade) : (sel ? st.cyan_dark : st.dim);
  Color chev = val;

  // icon column
  float lx = xl;
  if (has_icons_) {
    int isz = ui::px_floor(std::round(float(em) * 1.3f));
    if (!e.icon_name.empty()) {
      if (!e.t_icon.alive(a)) {
        ImgId id = 0;
        e.t_icon.ok = true;
        e.t_icon.blank = !a.svg(cx.th.path("assets/menu/menu-icon-" + e.icon_name + ".svg"), uint32_t(isz), id);
        e.t_icon.id = id;
        if (!e.t_icon.blank) e.t_icon.w = float(a.img(id).w), e.t_icon.h = float(a.img(id).h);
      }
      if (!e.t_icon.blank) out.push_back(Op::image(e.t_icon.id, std::round(xl + (float(isz) - e.t_icon.w) / 2.0f), std::round(cy - e.t_icon.h / 2.0f), e.t_icon.w, e.t_icon.h, lab));
    }
    lx = xl + float(isz) + std::round(0.7f * float(em));
  }

  float gap = std::round(1.0f * float(em));
  float arrow_px = std::round(float(em) * 1.0f);
  float ctrl_w = 0;  // width of the controls at the right, to size the label

  // value text (every kind except toggle)
  bool has_value = e.kind == EntryKind::Action || e.kind == EntryKind::Info || e.kind == EntryKind::Slider || e.kind == EntryKind::Choice || e.kind == EntryKind::Text;
  if (has_value && !e.t_value.alive(a)) {
    std::string v = e.value_text;
    if (e.kind == EntryKind::Text) {
      if (v.empty()) v = "\xE2\x80\x94";
      else if (e.password) {
        v.clear();
        for (size_t k = 0, n = utf8_len(e.value_text); k < n; ++k) v += "\xE2\x80\xA2";
      }
    }
    e.t_value.set(a, v, em, inner * 0.5f);
  }
  float sw_h = std::round(float(em) * 1.25f), sw_w = std::round(sw_h * 1.85f);
  float track_w = std::round(inner * 0.30f);
  if (e.kind == EntryKind::Slider && e.value_w <= 0) {
    // reserve room for the widest value so the track does not move while adjusting
    std::string lo = e.fmt ? e.fmt(e.min) : std::to_string(e.min) + e.unit_text;
    std::string hi = e.fmt ? e.fmt(e.max) : std::to_string(e.max) + e.unit_text;
    e.value_w = std::max(a.measure(lo, float(em)), std::max(a.measure(hi, float(em)), e.t_value.w));
  }
  switch (e.kind) {
    case EntryKind::Toggle:
      ctrl_w = sw_w;
      break;
    case EntryKind::Slider:
      ctrl_w = track_w + gap + e.value_w;
      break;
    case EntryKind::Action:
      ctrl_w = (e.show_arrow ? arrow_px : 0) + (e.t_value.blank ? 0 : e.t_value.w + (e.show_arrow ? 0.4f * float(em) : 0));
      break;
    case EntryKind::Choice:
      ctrl_w = arrow_px + 0.4f * float(em) + e.t_value.w + (sel && e.cycle && e.options.size() > 1 ? arrow_px : 0);
      break;
    default:
      ctrl_w = e.t_value.w;
      break;
  }
  if (!e.t_label.alive(a)) e.t_label.set(a, e.label, em, std::max(xr - lx - ctrl_w - (ctrl_w > 0 ? gap : 0), 3.0f * float(em)));
  ui::put_text(out, e.t_label, lx, cy, lab);

  // controls
  switch (e.kind) {
    case EntryKind::Toggle: {
      float pad = std::max(std::round(sw_h * 0.14f), 2.0f);
      float kd = sw_h - 2.0f * pad;
      const float t = e.kn >= 0.0f ? e.kn : (e.b ? 1.0f : 0.0f);  // 0 = off, 1 = on (the knob in between while it slides)
      shapes_.pill(a, out, xr - sw_w, cy - sw_h / 2.0f, sw_w, sw_h, t >= 1.0f ? st.sw_on : (t <= 0.0f ? st.sw_off : motion::mix(st.sw_off, st.sw_on, t)));
      float kx = (xr - sw_w + pad) + ((xr - pad - kd) - (xr - sw_w + pad)) * t;
      shapes_.disc(a, out, kx, cy - kd / 2.0f, int(kd), st.knob);
      break;
    }
    case EntryKind::Slider: {
      float tr_right = xr - e.value_w - gap;
      float tl = tr_right - track_w;
      float th = std::max(std::round(float(em) * 0.28f), 3.0f);
      float kd = std::round(float(em) * 0.95f);
      float kr = kd / 2.0f;
      float frac = e.max > e.min ? float(e.i - e.min) / float(e.max - e.min) : 0.0f;
      float kx = tl + kr + frac * (track_w - kd);
      shapes_.pill(a, out, tl, cy - th / 2.0f, track_w, th, st.track);
      if (kx - tl > 0) shapes_.pill(a, out, tl, cy - th / 2.0f, std::max(kx - tl, th), th, st.cyan);
      if (sel) shapes_.disc(a, out, kx - kr - 2.0f, cy - kr - 2.0f, int(kd) + 4, st.cyan);
      shapes_.disc(a, out, kx - kr, cy - kr, int(kd), st.knob);
      ui::put_text_right(out, e.t_value, xr, cy, val);
      break;
    }
    case EntryKind::Action: {
      float x = xr;
      if (e.show_arrow) {
        shapes_.icon(a, out, Icon::ChevronRight, x - arrow_px, cy - arrow_px / 2.0f, int(arrow_px), chev);
        x -= arrow_px + 0.4f * float(em);
      }
      ui::put_text_right(out, e.t_value, x, cy, val);
      break;
    }
    case EntryKind::Choice: {
      float x = xr - arrow_px;
      shapes_.icon(a, out, Icon::ChevronRight, x, cy - arrow_px / 2.0f, int(arrow_px), chev);
      x -= 0.4f * float(em);
      ui::put_text_right(out, e.t_value, x, cy, val);
      if (sel && e.cycle && e.options.size() > 1) shapes_.icon(a, out, Icon::ChevronLeft, x - e.t_value.w - arrow_px, cy - arrow_px / 2.0f, int(arrow_px), chev);
      break;
    }
    default:
      ui::put_text_right(out, e.t_value, xr, cy, val);
      break;
  }
}

void Menu::warm(Ctx& cx, std::vector<Op>& out) {
  if (!opened_) {
    opened_ = true;
    rebuild(cx);
  }
  draw(cx, out);
}

void Menu::draw(Ctx& cx, std::vector<Op>& out) {
  sync(cx);
  Assets& a = cx.assets;
  const ui::Metrics& m = m_;
  const ui::Style& st = st_;
  out.push_back(Op::rect(0, 0, m.W, m.H, st.bg));

  // header
  float inset = std::round(m.pill_h * ui::kPillInset);
  if (!t_title_.alive(a)) t_title_.set(a, spec_.title, m.title_px, m.col_w * 0.72f);
  ui::put_text(out, t_title_, m.col_x + inset, m.header_cy, st.txt);
  status_bar(cx.th, a, m.W, m.H, cx.status, out);
  float title_bottom = m.header_cy + t_title_.h / 2.0f;
  float first_pill_top = m.list_top + (m.row_h - m.pill_h) / 2.0f;
  float line_y = std::round((title_bottom + first_pill_top) / 2.0f);
  out.push_back(Op::rect(m.col_x, line_y, m.col_w, 1, st.line));
  ui::HeaderBox& hb = ui::header_box();
  hb = {true, m.col_x + inset, m.col_x + inset + t_title_.w, m.header_cy - t_title_.h / 2.0f, title_bottom, line_y};

  // rows
  out.push_back(Op::clip(m.col_x, m.list_top, m.col_w, m.list_h));
  if (cur_ >= 0 && size_t(cur_) < e_.size()) {
    float py = std::round(m.list_top + sel_y_ - scroll_ + (m.row_h - m.pill_h) / 2.0f);
    shapes_.pill(a, out, m.col_x, py, m.col_w, m.pill_h, e_[size_t(cur_)].is_danger ? st.danger_soft : st.cyan_soft);
  }
  for (size_t i = 0; i < e_.size(); ++i) {
    float y = std::round(m.list_top + top_[i] - scroll_);
    if (y + hgt_[i] <= m.list_top || y >= m.list_top + m.list_h) continue;
    float vis = 1.0f;
    if (e_[i].kind == EntryKind::Header) {
      float top = std::max(y, m.list_top), bot = std::min(y + hgt_[i], m.list_top + m.list_h);
      vis = std::min(std::max((bot - top) / hgt_[i], 0.0f), 1.0f);
    }
    draw_row(cx, out, i, y, int(i) == cur_, vis);
  }
  out.push_back(Op::unclip());

  // scrollbar
  if (content_h_ > m.list_h + 0.5f) {
    float maxs = content_h_ - m.list_h;
    float th = std::max(std::round(m.list_h * m.list_h / content_h_), 2.0f * float(m.em));
    float ty = m.list_top + std::min(std::max(scroll_ / maxs, 0.0f), 1.0f) * (m.list_h - th);
    float bw = std::max(std::round(0.007f * m.H), 3.0f);
    shapes_.pill(a, out, m.col_x + m.col_w + std::max(std::round(inset * 0.35f), 2.0f), ty, bw, th, ui::with_alpha(st.txt, 0.30f));
  }

  // hint of the entry under the cursor + help bar
  const Prompt* hp = kHelpPlain;
  size_t hn = 2;
  if (cur_ >= 0 && size_t(cur_) < e_.size()) {
    const Entry& e = e_[size_t(cur_)];
    switch (e.kind) {
      case EntryKind::Toggle:
        hp = kHelpToggle, hn = 3;
        break;
      case EntryKind::Slider:
        hp = kHelpSlider, hn = 3;
        break;
      case EntryKind::Choice:
        hp = e.cycle ? kHelpChoice : kHelpAction, hn = e.cycle ? 4 : 3;
        break;
      case EntryKind::Action:
      case EntryKind::Text:
        hp = kHelpAction, hn = 3;
        break;
      default:
        break;
    }
    if (hint_for_ != cur_) {
      hint_for_ = cur_;
      t_hint_.reset();
    }
    if (!e.hint_text.empty()) {
      if (!t_hint_.alive(a)) t_hint_.set(a, e.hint_text, m.small_px, m.col_w - 2.0f * inset);
      float list_bottom = m.list_top + m.list_h;
      ui::put_text(out, t_hint_, m.col_x + inset, std::round(list_bottom + (m.help_y - list_bottom) * 0.30f), st.dim);
    }
  }
  help_.draw(a, cx.th, m, st, hp, hn, out);
}

}  // namespace zf
