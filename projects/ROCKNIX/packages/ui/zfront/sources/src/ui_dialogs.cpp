#include "ui_dialogs.hpp"

#include <algorithm>
#include <cmath>

#include "registry.hpp"
#include "scene.hpp"
#include "util.hpp"

namespace zf::ui {

namespace {

void draw_scrim(const Metrics& m, const Style& st, std::vector<Op>& out) { out.push_back(Op::rect(0, 0, m.W, m.H, st.scrim)); }

int card_radius(const Metrics& m) { return int(std::lround(m.pill_h * 0.6f)); }

void card(Shapes& sh, Assets& a, std::vector<Op>& out, const Metrics& m, const Style& st, float x, float y, float w, float h) {
  sh.framed(a, out, x, y, w, h, card_radius(m), st.panel, st.line, 1);
}

float text_inset(const Metrics& m) { return std::round(m.pill_h * kPillInset); }

}  // namespace

Transition leave_with(Transition t, Ctx& cx) {
  switch (t.kind) {
    case Transition::None:
      return Transition::pop();
    case Transition::Push:
      return Transition::replace(std::move(t.screen));
    case Transition::Open: {
      std::unique_ptr<Screen> s = Registry::instance().make(t.name, cx, t.arg);
      if (s) return Transition::replace(std::move(s));
      cx.sv->toasts.push(tr("Not available yet"));
      return Transition::pop();
    }
    default:
      return t;
  }
}

Transition confirm(std::string message, Act on_yes, ConfirmOpts opts) {
  return Transition::push(std::make_unique<ConfirmDialog>(std::move(message), std::move(on_yes), std::move(opts), false));
}

Transition message(std::string text, Act on_ok, std::string title) {
  ConfirmOpts o;
  o.title = std::move(title);
  return Transition::push(std::make_unique<ConfirmDialog>(std::move(text), std::move(on_ok), std::move(o), true));
}

Transition choose(std::string title, std::vector<Option> options, std::string current, std::function<void(Ctx&, const std::string&)> on_pick) {
  return Transition::push(std::make_unique<ChoiceDialog>(std::move(title), std::move(options), std::move(current), std::move(on_pick)));
}

Transition keyboard(KeyboardOpts opts, std::string initial, std::function<void(Ctx&, const std::string&)> on_done) {
  return Transition::push(std::make_unique<KeyboardScreen>(std::move(opts), std::move(initial), std::move(on_done)));
}

// ---- ConfirmDialog ---------------------------------------------------------------------------------------------------

ConfirmDialog::ConfirmDialog(std::string message, Act on_yes, ConfirmOpts opts, bool single_button)
    : msg_(std::move(message)), on_yes_(std::move(on_yes)), o_(std::move(opts)), single_(single_button) {
  focus_ = (single_ || o_.default_yes) ? 0 : 1;
}

Transition ConfirmDialog::handle(Action a, Ctx& cx) {
  switch (a) {
    case Action::Left:
      if (!single_) focus_ = 0;
      break;
    case Action::Right:
      if (!single_) focus_ = 1;
      break;
    case Action::A:
      if (focus_ == 0) return leave_with(on_yes_ ? on_yes_(cx) : Transition::none(), cx);
      return Transition::pop();
    case Action::B:
      return Transition::pop();
    default:
      break;
  }
  return Transition::none();
}

void ConfirmDialog::draw(Ctx& cx, std::vector<Op>& out) {
  Assets& a = cx.assets;
  if (cx.w != lay_w_ || cx.h != lay_h_) {
    lay_w_ = cx.w, lay_h_ = cx.h;
    m_ = Metrics::from(cx.th, cx.w, cx.h);
    st_ = Style::from(cx.th);
    t_msg_.reset(), t_title_.reset(), t_yes_.reset(), t_no_.reset();
  }
  const int em = m_.em;
  float card_w = std::min(std::max(25.0f * float(em), 0.5f * m_.W), std::min(0.92f * m_.W, m_.col_w));
  float pad = std::round(1.3f * float(em));
  float inner = card_w - 2.0f * pad;
  if (!t_msg_.alive(a)) {
    // As many lines as the screen allows (a destructive question must never lose its last line): 90 % of the height
    // minus padding, title and buttons, over the line pitch
    float title_est = o_.title.empty() ? 0.0f : std::floor(float(em) * 1.15f) + std::round(0.55f * float(em));
    float avail = 0.9f * m_.H - 2.0f * pad - title_est - std::round(1.2f * float(em)) - m_.pill_h;
    size_t lines = size_t(std::clamp(avail / (1.25f * 1.2f * float(em)), 4.0f, 24.0f));
    t_msg_.set_wrap(a, msg_, em, inner, lines, 1.25f);
  }
  if (!o_.title.empty() && !t_title_.alive(a)) t_title_.set(a, o_.title, px_floor(std::floor(float(em) * 1.15f)), inner);
  if (!t_yes_.alive(a)) t_yes_.set(a, !o_.yes.empty() ? o_.yes : std::string(single_ ? tr("OK") : tr("YES")), em, inner / 2.0f);
  if (!single_ && !t_no_.alive(a)) t_no_.set(a, !o_.no.empty() ? o_.no : std::string(tr("NO")), em, inner / 2.0f);

  float title_h = o_.title.empty() ? 0.0f : t_title_.h + std::round(0.55f * float(em));
  float bh = m_.pill_h;
  float gap_btn = std::round(1.2f * float(em));
  float card_h = pad + title_h + t_msg_.h + gap_btn + bh + pad;
  float cx0 = std::round((m_.W - card_w) / 2.0f), cy0 = std::round((m_.H - card_h) / 2.0f);

  draw_scrim(m_, st_, out);
  card(shapes_, a, out, m_, st_, cx0, cy0, card_w, card_h);
  float y = cy0 + pad;
  if (!o_.title.empty()) {
    put_text(out, t_title_, cx0 + pad, y + t_title_.h / 2.0f, st_.cyan_dark);
    y += title_h;
  }
  put_text(out, t_msg_, cx0 + std::round((card_w - t_msg_.w) / 2.0f), y + t_msg_.h / 2.0f, st_.txt);
  y += t_msg_.h + gap_btn;

  float bw = std::max(std::round(7.0f * float(em)), std::max(t_yes_.w, single_ ? 0.0f : t_no_.w) + 2.0f * float(em));
  float gap = float(em);
  int n = single_ ? 1 : 2;
  float total = bw * float(n) + gap * float(n - 1);
  float bx = cx0 + std::round((card_w - total) / 2.0f);
  for (int i = 0; i < n; ++i) {
    bool foc = focus_ == i;
    bool red = o_.danger && i == 0;
    Color fill = foc ? (red ? st_.danger : st_.cyan) : st_.btn;
    Color txt = foc ? on_color(fill) : (red ? st_.danger : st_.txt);
    shapes_.pill(a, out, bx, y, bw, bh, fill);
    const Txt& t = i == 0 ? t_yes_ : t_no_;
    put_text(out, t, bx + std::round((bw - t.w) / 2.0f), y + bh / 2.0f, txt);
    bx += bw + gap;
  }
}

// ---- ChoiceDialog ----------------------------------------------------------------------------------------------------

ChoiceDialog::ChoiceDialog(std::string title, std::vector<Option> options, std::string current, std::function<void(Ctx&, const std::string&)> on_pick)
    : title_(std::move(title)), opts_(std::move(options)), pick_(std::move(on_pick)) {
  for (size_t i = 0; i < opts_.size(); ++i)
    if (opts_[i].value == current) cur_value_ = cur_ = int(i);
  t_.resize(opts_.size());
}

void ChoiceDialog::layout(Ctx& cx) {
  if (cx.w == lay_w_ && cx.h == lay_h_) return;
  lay_w_ = cx.w, lay_h_ = cx.h;
  m_ = Metrics::from(cx.th, cx.w, cx.h);
  st_ = Style::from(cx.th);
  for (Txt& t : t_) t.reset();
  t_title_.reset();
  float pad = std::round(float(m_.em));
  float title_h = title_.empty() ? 0.0f : std::round(2.0f * float(m_.em));
  int fit = int(std::floor((0.8f * m_.H - title_h - 2.0f * pad) / m_.row_h));
  vis_ = std::min(int(opts_.size()), std::max(3, fit));
  rows_h_ = float(vis_) * m_.row_h;
  card_h_ = pad + title_h + rows_h_ + pad;
  card_y_ = std::round((m_.H - card_h_) / 2.0f);
  rows_y_ = card_y_ + pad + title_h;
  placed_ = false;
}

namespace {
// target scroll (in pixels) that puts row `cur` in the middle of `vis` rows, snapped to whole rows
float choice_scroll(int cur, int vis, int n, float row_h) {
  float maxs = float(std::max(n - vis, 0)) * row_h;
  float t = (float(cur) - float(vis - 1) / 2.0f) * row_h;
  t = std::min(std::max(t, 0.0f), maxs);
  return std::round(t / row_h) * row_h;
}
}  // namespace

Transition ChoiceDialog::handle(Action a, Ctx& cx) {
  layout(cx);
  int n = int(opts_.size());
  switch (a) {
    case Action::Up:
      if (n > 0) cur_ = (cur_ + n - 1) % n;
      break;
    case Action::Down:
      if (n > 0) cur_ = (cur_ + 1) % n;
      break;
    case Action::L:
    case Action::Left:
      cur_ = std::max(cur_ - std::max(vis_ - 1, 1), 0);
      break;
    case Action::R:
    case Action::Right:
      cur_ = std::min(cur_ + std::max(vis_ - 1, 1), std::max(n - 1, 0));
      break;
    case Action::A:
      if (cur_ >= 0 && cur_ < n) {
        if (pick_) pick_(cx, opts_[size_t(cur_)].value);
        return Transition::pop();
      }
      break;
    case Action::B:
      return Transition::pop();
    default:
      break;
  }
  scroll_t_ = choice_scroll(cur_, vis_, n, m_.row_h);
  return Transition::none();
}

bool ChoiceDialog::tick(float dt, Ctx& cx) {
  layout(cx);
  float st = float(cur_) * m_.row_h;
  if (!placed_) {
    scroll_t_ = choice_scroll(cur_, vis_, int(opts_.size()), m_.row_h);
    scroll_ = scroll_t_;
    sel_y_ = st;
    placed_ = true;
    return false;
  }
  bool a = approach(scroll_, scroll_t_, dt);
  bool b = approach(sel_y_, st, dt);
  return a || b;
}

void ChoiceDialog::draw(Ctx& cx, std::vector<Op>& out) {
  layout(cx);
  Assets& a = cx.assets;
  if (!placed_) tick(0, cx);
  const int em = m_.em;
  float pad = std::round(float(em));
  // card width from the widest option
  float widest = 0;
  for (size_t i = 0; i < opts_.size(); ++i) {
    if (!t_[i].alive(a)) t_[i].set(a, opts_[i].label, em, 0.9f * m_.W - 6.0f * float(em));
    widest = std::max(widest, t_[i].w);
  }
  card_w_ = std::min(std::max(widest + 2.0f * pad + 2.0f * text_inset(m_) + 1.6f * float(em), 14.0f * float(em)), std::min(0.92f * m_.W, m_.col_w));
  card_x_ = std::round((m_.W - card_w_) / 2.0f);
  if (!title_.empty() && !t_title_.alive(a)) t_title_.set(a, title_, em, card_w_ - 2.0f * pad);

  draw_scrim(m_, st_, out);
  card(shapes_, a, out, m_, st_, card_x_, card_y_, card_w_, card_h_);
  if (!title_.empty()) put_text(out, t_title_, card_x_ + pad, card_y_ + pad + std::round(1.0f * float(em)), st_.dim);
  float rx = card_x_ + pad * 0.5f, rw = card_w_ - pad;
  out.push_back(Op::clip(rx, rows_y_, rw, rows_h_));
  Color sel_fill = st_.cyan_soft;
  float py = std::round(rows_y_ + sel_y_ - scroll_ + (m_.row_h - m_.pill_h) / 2.0f);
  shapes_.pill(a, out, rx, py, rw, m_.pill_h, sel_fill);
  int first = std::max(int(std::floor(scroll_ / m_.row_h)), 0);
  int last = std::min(first + vis_ + 1, int(opts_.size()) - 1);
  int isz = px_floor(std::floor(1.1f * float(em)));
  for (int i = first; i <= last; ++i) {
    float cy = std::round(rows_y_ + float(i) * m_.row_h - scroll_ + m_.row_h / 2.0f);
    bool sel = i == cur_;
    Color c = sel ? st_.cyan_dark : st_.txt;
    put_text(out, t_[size_t(i)], rx + text_inset(m_), cy, c);
    if (i == cur_value_) shapes_.icon(a, out, Icon::Check, rx + rw - text_inset(m_) - float(isz), cy - float(isz) / 2.0f, isz, sel ? st_.cyan_dark : st_.cyan);
  }
  out.push_back(Op::unclip());
  // scrollbar
  if (int(opts_.size()) > vis_) {
    float total = float(opts_.size()) * m_.row_h, maxs = total - rows_h_;
    float th = std::max(std::round(rows_h_ * rows_h_ / total), 2.0f * float(em));
    float ty = rows_y_ + (maxs > 0 ? scroll_ / maxs : 0.0f) * (rows_h_ - th);
    float bw = std::max(std::round(0.006f * m_.H), 3.0f);
    shapes_.pill(a, out, card_x_ + card_w_ - std::round((pad * 0.5f + bw) / 2.0f) - bw + 1.0f, ty, bw, th, with_alpha(st_.txt, 0.30f));
  }
}

// ---- KeyboardScreen --------------------------------------------------------------------------------------------------

namespace {

const char* const kLower[4] = {"1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm.-@"};
const char* const kUpper[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM.-@"};
const char* const kSymbols[4] = {"!@#$%^&*()", "-_=+[]{}\\|", ";:'\"<>/?`~", ",.\xE2\x82\xAC\xC2\xA3\xC2\xA5\xC2\xB0\xC2\xA9\xC2\xAE\xC2\xBF\xC2\xA1"};

const Prompt kKeyboardHelp[] = {{"A", N_("SELECT")}, {"B", N_("DELETE")}, {"X", N_("SPACE")}, {"Y", N_("SHIFT")}, {"start", N_("OK")}};

}  // namespace

KeyboardScreen::KeyboardScreen(KeyboardOpts opts, std::string initial, std::function<void(Ctx&, const std::string&)> on_done)
    : o_(std::move(opts)), text_(std::move(initial)), done_(std::move(on_done)) {
  if (o_.max_len == 0) o_.max_len = 128;
  build_layer();
}

void KeyboardScreen::build_layer() {
  rows_.clear();
  const char* const* src = layer_ == 1 ? kUpper : (layer_ == 2 ? kSymbols : kLower);
  for (int r = 0; r < 4; ++r) {
    std::vector<Key> row;
    std::string_view s = src[r];
    for (size_t i = 0; i < s.size();) {
      size_t st = i;
      utf8_next(s, i);
      Key k;
      k.type = Key::Char;
      k.label = std::string(s.substr(st, i - st));
      row.push_back(std::move(k));
    }
    rows_.push_back(std::move(row));
  }
  auto sp = [](Key::Type t, const char* lab, float x0, float w) {
    Key k;
    k.type = t;
    k.label = lab;
    k.x0 = x0;
    k.w = w;
    return k;
  };
  std::vector<Key> bottom;
  bottom.push_back(sp(Key::Shift, tr("SHIFT"), 0.0f, 1.6f));
  bottom.push_back(sp(Key::Sym, layer_ == 2 ? "ABC" : "123", 1.6f, 1.6f));
  bottom.push_back(sp(Key::Space, tr("SPACE"), 3.2f, 3.0f));
  bottom.push_back(sp(Key::Del, tr("DELETE"), 6.2f, 1.6f));
  bottom.push_back(sp(Key::Ok, tr("OK"), 7.8f, 2.2f));
  rows_.push_back(std::move(bottom));
  for (int r = 0; r < 4; ++r)
    for (size_t c = 0; c < rows_[size_t(r)].size(); ++c) {
      rows_[size_t(r)][c].x0 = float(c);
      rows_[size_t(r)][c].w = 1.0f;
    }
  row_ = std::min(row_, 4);
  col_ = std::min(col_, int(rows_[size_t(row_)].size()) - 1);
}

void KeyboardScreen::erase_last() {
  if (text_.empty()) return;
  size_t k = text_.size() - 1;
  while (k > 0 && (static_cast<uint8_t>(text_[k]) & 0xC0) == 0x80) --k;
  text_.resize(k);
}

Transition KeyboardScreen::accept(Ctx& cx) {
  if (done_) done_(cx, text_);
  return Transition::pop();
}

Transition KeyboardScreen::press(const Key& k, Ctx& cx) {
  switch (k.type) {
    case Key::Char:
      if (utf8_len(text_) < o_.max_len) text_ += k.label;
      break;
    case Key::Space:
      if (utf8_len(text_) < o_.max_len) text_ += ' ';
      break;
    case Key::Del:
      erase_last();
      break;
    case Key::Shift:
      layer_ = layer_ == 1 ? 0 : 1;
      build_layer();
      break;
    case Key::Sym:
      layer_ = layer_ == 2 ? 0 : 2;
      build_layer();
      break;
    case Key::Ok:
      return accept(cx);
  }
  return Transition::none();
}

void KeyboardScreen::move_v(int dir) {
  const Key& cur = key_at(row_, col_);
  float center = cur.x0 + cur.w / 2.0f;
  int nr = (row_ + dir + 5) % 5;
  const std::vector<Key>& row = rows_[size_t(nr)];
  int best = 0;
  for (size_t i = 0; i < row.size(); ++i)
    if (center >= row[i].x0 && center < row[i].x0 + row[i].w) best = int(i);
  row_ = nr;
  col_ = best;
}

namespace {
constexpr auto kBlinkPhase = std::chrono::milliseconds(530);  // the caret stays on / off this long
}

bool KeyboardScreen::tick(float, Ctx&) {
  TimePoint now = Clock::now();
  if (now >= next_flip_) {
    caret_on_ = !caret_on_;
    next_flip_ = now + kBlinkPhase;  // after a long sleep one flip is enough: no catching up
    redraw_ = true;
  }
  return false;
}

std::optional<float> KeyboardScreen::wake_in() const {
  float left = std::chrono::duration<float>(next_flip_ - Clock::now()).count();
  return std::max(left, 0.0f) + 0.002f;
}

bool KeyboardScreen::take_redraw() {
  bool r = redraw_;
  redraw_ = false;
  return r;
}

Transition KeyboardScreen::handle(Action a, Ctx& cx) {
  caret_on_ = true;  // the caret is solid while the user is typing
  next_flip_ = Clock::now() + kBlinkPhase;
  int n = int(rows_[size_t(row_)].size());
  switch (a) {
    case Action::Up:
      move_v(-1);
      break;
    case Action::Down:
      move_v(1);
      break;
    case Action::Left:
      col_ = (col_ + n - 1) % n;
      break;
    case Action::Right:
      col_ = (col_ + 1) % n;
      break;
    case Action::A: {
      Key k = key_at(row_, col_);
      return press(k, cx);
    }
    case Action::B:
      if (text_.empty()) return Transition::pop();
      erase_last();
      break;
    case Action::X:
      if (utf8_len(text_) < o_.max_len) text_ += ' ';
      break;
    case Action::Y: {
      Key k;
      k.type = Key::Shift;
      press(k, cx);
      break;
    }
    case Action::L:
      if (o_.password) reveal_ = !reveal_;
      break;
    case Action::Start:
      return accept(cx);
    case Action::Select:
      return Transition::pop();
    default:
      break;
  }
  return Transition::none();
}

void KeyboardScreen::draw(Ctx& cx, std::vector<Op>& out) {
  Assets& a = cx.assets;
  if (cx.w != lay_w_ || cx.h != lay_h_) {
    lay_w_ = cx.w, lay_h_ = cx.h;
    m_ = Metrics::from(cx.th, cx.w, cx.h);
    st_ = Style::from(cx.th);
    t_title_.reset(), t_text_.reset();
    keys_layer_ = -1;
  }
  const int em = m_.em;
  out.push_back(Op::rect(0, 0, m_.W, m_.H, st_.bg));
  float xl = m_.col_x + text_inset(m_);
  if (!t_title_.alive(a)) t_title_.set(a, o_.title, m_.title_px, m_.col_w * 0.7f);
  put_text(out, t_title_, xl, m_.header_cy, st_.txt);
  status_bar(cx.th, a, m_.W, m_.H, cx.status, out);
  {
    float title_bottom = m_.header_cy + t_title_.h / 2.0f;
    float first_pill_top = m_.list_top + (m_.row_h - m_.pill_h) / 2.0f;
    header_box() = {true, xl, xl + t_title_.w, m_.header_cy - t_title_.h / 2.0f, title_bottom, std::round((title_bottom + first_pill_top) / 2.0f)};
  }

  // input field + keys form one block, placed a little above the middle of the free area
  float gap = std::max(2.0f, std::round(0.011f * m_.H));
  float field_h = std::round(m_.pill_h * 1.3f);
  float gx = m_.col_x, gw = m_.col_w;
  float key_w = std::floor((gw - 9.0f * gap) / 10.0f);
  float free_h = m_.help_y - float(m_.foot_px) * 1.3f - m_.list_top;
  float kh = std::floor(std::min((free_h - field_h - 6.0f * gap) / 5.0f, key_w * 0.95f));
  float block_h = field_h + 2.0f * gap + 5.0f * kh + 4.0f * gap;
  float fy = m_.list_top + std::floor(std::max(free_h - block_h, 0.0f) * 0.30f);
  shapes_.framed(a, out, gx, fy, gw, field_h, int(field_h * 0.3f), st_.panel, st_.line, 1);
  // the field's image is rebuilt only when the text or the reveal flag changed (comparing strings allocates nothing; the blink
  // redraws this screen every phase, so a frame at rest must not build a temporary string)
  if (!t_text_.alive(a) || !shown_valid_ || text_ != shown_src_ || reveal_ != shown_rev_) {
    shown_valid_ = true;
    shown_src_ = text_;
    shown_rev_ = reveal_;
    shown_.clear();
    if (o_.password && !reveal_) {
      size_t n = utf8_len(text_);
      for (size_t i = 0; i < n; ++i) shown_ += "\xE2\x80\xA2";
    } else {
      shown_ = text_;
    }
    t_text_.set(a, shown_, em);
  }
  float pad = std::round(field_h * 0.35f);
  float inner = gw - 2.0f * pad;
  float tw = t_text_.blank ? 0.0f : t_text_.w;
  float caret_gap = std::max(2.0f, std::round(0.15f * float(em)));
  float tx = tw + caret_gap > inner ? gx + gw - pad - tw - caret_gap : gx + pad;
  out.push_back(Op::clip(gx + pad, fy, inner, field_h));
  put_text(out, t_text_, tx, fy + field_h / 2.0f, st_.txt);
  float ch = std::round(float(em) * 1.25f), cw = std::max(2.0f, std::round(m_.H / 300.0f));
  if (caret_on_) out.push_back(Op::rect(std::round(tx + tw + caret_gap), std::round(fy + (field_h - ch) / 2.0f), cw, ch, st_.cyan));
  out.push_back(Op::unclip());

  // keys
  float gy = fy + field_h + gap * 2.0f;
  if (keys_layer_ != layer_) {
    t_keys_.assign(45, Txt());
    keys_layer_ = layer_;
  }
  for (int r = 0; r < 5; ++r) {
    const std::vector<Key>& row = rows_[size_t(r)];
    float ky = std::round(gy + float(r) * (kh + gap));
    float total_w = gw - 4.0f * gap;  // bottom row: 5 keys, 4 gaps
    float x_acc = gx;
    for (size_t c = 0; c < row.size(); ++c) {
      const Key& k = row[c];
      float kx, kw;
      if (r < 4) {
        kx = std::round(gx + float(c) * (key_w + gap));
        kw = key_w;
      } else {
        kw = std::floor(total_w * k.w / 10.0f);
        kx = std::round(x_acc);
        x_acc += kw + gap;
        if (c + 1 == row.size()) kw = gx + gw - kx;  // last key reaches the edge
      }
      bool foc = r == row_ && int(c) == col_;
      bool special = r == 4;
      bool lit = (k.type == Key::Shift && layer_ == 1) || (k.type == Key::Sym && layer_ == 2);
      Color fill = special ? st_.key_alt : st_.key;
      Color txt = st_.txt;
      if (k.type == Key::Ok) fill = st_.cyan, txt = on_color(st_.cyan);
      if (lit) txt = st_.cyan_dark;
      int rad = int(kh * 0.28f);
      if (foc) {
        shapes_.framed(a, out, kx, ky, kw, kh, rad, st_.cyan_soft, st_.cyan, 2);
        txt = st_.cyan_dark;
      } else {
        shapes_.rrect(a, out, kx, ky, kw, kh, rad, fill);
        if (lit) shapes_.framed(a, out, kx, ky, kw, kh, rad, fill, st_.cyan_dark, 1);
      }
      Txt& t = t_keys_[size_t(r * 10) + c];
      if (!t.alive(a)) t.set(a, k.label, special ? m_.small_px : em, kw - 4.0f);
      put_text(out, t, kx + std::round((kw - t.w) / 2.0f), ky + kh / 2.0f, txt);
    }
  }
  help_.draw(a, cx.th, m_, st_, kKeyboardHelp, 5, out);
}

}  // namespace zf::ui
