// `menu.savestates`: the savestate manager of a game (ES `GuiSaveState`). Argument: "<system name>|<game path>"
// (`savestates::make_arg`). A bottom sheet over the game list with a strip of cards:
//
//   [ + Start new game ]  [ screenshot / date ]  [ screenshot / date ]  ...
//
// The "start new game" card is always the first one, flush with the left edge; the saved states follow to its right (newest
// first in incremental mode, by slot otherwise), as in ES. A launches with that state, Y deletes, X copies to the next free slot,
// B / L3 / Start close. Screenshots are decoded on the job thread, one job per card (never re-requested while in flight).
//
// All sizes are fractions of the screen (see `layout`): card width = min(0.30 W, 0.38 H), everything else in `em` of the menu
// text. Nothing is allocated while drawing: texts and image ids are cached per card and rebuilt when the files change or the
// screen size does.
#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>
#include <unordered_set>

#include "covers.hpp"
#include "launch.hpp"
#include "launchcheck.hpp"
#include "registry.hpp"
#include "savestates.hpp"
#include "ui_dialogs.hpp"
#include "ui_style.hpp"
#include "util.hpp"

namespace zf {
namespace {

using namespace ui;

const Prompt kHelpPlain[] = {{"A", N_("LAUNCH")}, {"B", N_("BACK")}};
const Prompt kHelpState[] = {{"A", N_("LAUNCH")}, {"B", N_("BACK")}, {"Y", N_("DELETE")}, {"X", N_("COPY TO FREE SLOT")}};

std::unordered_set<std::string>& in_flight() {
  static std::unordered_set<std::string> s;
  return s;
}

// The image of `path` fitted into w x h: true when it is in the cache (`id` set). Starts the decode otherwise.
bool thumb(Ctx& cx, const std::string& path, uint32_t w, uint32_t h, ImgId& id) {
  Assets& a = cx.assets;
  if (a.find_file_fit(path, w, h, id)) return true;
  if (covers::failed(path)) return false;
  if (!cx.async_covers) {  // harness and tests: deterministic, inline
    if (a.file_fit(path, w, h, id)) return true;
    covers::mark_failed(path);
    return false;
  }
  std::string key = path + "\n" + std::to_string(w) + "x" + std::to_string(h);
  if (!in_flight().insert(key).second) return false;
  auto img = std::make_shared<Img>();
  auto ok = std::make_shared<bool>(false);
  Assets* assets = &a;  // `Ctx` owns the assets and the jobs: `done` never runs after the assets are gone
  cx.sv->jobs.post([=] { *ok = Assets::decode_fit(path, w, h, *img); },
                   [=] {
                     in_flight().erase(key);
                     if (*ok) assets->add_file_fit(path, w, h, std::move(*img));
                     else covers::mark_failed(path);
                   });
  return false;
}

std::string stamp(time_t t, bool with_year) {
  struct tm tm;
  ::localtime_r(&t, &tm);
  char b[32];
  std::strftime(b, sizeof b, with_year ? "%Y-%m-%d %H:%M" : "%m-%d %H:%M", &tm);
  return b;
}

struct Card {
  enum Kind { NewGame, NewAuto, State } kind = State;
  savestates::State st;
  std::string line1, line2;
  Txt t1, t2;
  ImgId img = 0;
  bool have_img = false;
};

class SaveStateScreen : public Screen {
 public:
  SaveStateScreen(Ctx& cx, size_t sys, size_t game) : si_(sys), gi_(game) {
    savestates::recover(cx.systems[si_], cx.systems[si_].games[gi_]);
    load(cx, nullptr);
  }

  Transition handle(Action a, Ctx& cx) override {
    sync(cx);
    int n = int(cards_.size());
    switch (a) {
      case Action::Left:
        cur_ = std::max(cur_ - 1, 0);
        break;
      case Action::Right:
        cur_ = std::min(cur_ + 1, n - 1);
        break;
      case Action::L:
      case Action::L2:
        cur_ = std::max(cur_ - std::max(vis_ - 1, 1), 0);
        break;
      case Action::R:
      case Action::R2:
        cur_ = std::min(cur_ + std::max(vis_ - 1, 1), n - 1);
        break;
      case Action::A:
        return launch(cx);
      case Action::B:
      case Action::L3:
      case Action::Start:
        return Transition::pop();
      case Action::Y:
        return remove(cx);
      case Action::X:
        copy(cx);
        break;
      default:
        break;
    }
    return Transition::none();
  }

  bool tick(float dt, Ctx& cx) override {
    if (!laid_) return false;
    return approach(scroll_, scroll_to(), dt);
  }

  void draw(Ctx& cx, std::vector<Op>& out) override {
    sync(cx);
    layout(cx);
    Assets& a = cx.assets;
    const Metrics& m = m_;
    out.push_back(Op::rect(0, 0, m.W, m.H, st_.scrim));
    float r = std::round(m.pill_h * 0.6f);
    // the sheet's side and bottom corners are off screen
    shapes_.framed(a, out, -2.0f * r, sheet_top_, m.W + 4.0f * r, m.H - sheet_top_ + 2.0f * r, int(r), st_.bg, st_.line, 1);

    // title row: title at the left, "n/N" at the right; the game name under it
    float tx = margin_;
    if (!t_title_.alive(a)) t_title_.set(a, tr("SAVESTATE MANAGER"), title_px_, m.W * 0.7f);
    put_text(out, t_title_, tx, title_cy_, st_.txt);
    std::string cnt = std::to_string(cur_ + 1) + "/" + std::to_string(cards_.size());
    if (cnt != cnt_text_ || !t_count_.alive(a)) {
      cnt_text_ = cnt;
      t_count_.set(a, cnt, m.small_px, m.W * 0.2f);
    }
    put_text_right(out, t_count_, m.W - margin_, title_cy_, st_.dim);
    if (!t_game_.alive(a)) t_game_.set(a, game_name_, m.small_px, m.W - 2.0f * margin_);
    put_text(out, t_game_, tx, game_cy_, st_.dim);

    // cards
    out.push_back(Op::clip(0, cards_y_ - 4.0f, m.W, card_h_ + 8.0f));
    int first = std::max(int(std::floor((scroll_ - margin_) / (card_w_ + gap_))), 0);
    for (int i = first; i < int(cards_.size()); ++i) {
      float x = std::round(margin_ + float(i) * (card_w_ + gap_) - scroll_);
      if (x >= m.W) break;
      if (x + card_w_ <= 0) continue;
      draw_card(cx, out, cards_[size_t(i)], x, i == cur_);
    }
    out.push_back(Op::unclip());
    // more cards beyond the edges
    float cy = cards_y_ + card_h_ / 2.0f;
    int isz = px_floor(std::floor(1.1f * float(m.em)));
    // a small disc of the sheet colour behind each chevron keeps it readable over a screenshot
    int disc = isz + int(std::round(0.4f * float(m.em)));
    float dy = std::round(cy - float(disc) / 2.0f), iy = std::round(cy - float(isz) / 2.0f);
    if (scroll_ > 0.5f) {
      shapes_.disc(a, out, 1.0f, dy, disc, st_.bg);
      shapes_.icon(a, out, Icon::ChevronLeft, 1.0f + float(disc - isz) / 2.0f, iy, isz, st_.txt);
    }
    if (scroll_ < max_scroll() - 0.5f) {
      shapes_.disc(a, out, m.W - float(disc) - 1.0f, dy, disc, st_.bg);
      shapes_.icon(a, out, Icon::ChevronRight, m.W - float(disc) - 1.0f + float(disc - isz) / 2.0f, iy, isz, st_.txt);
    }

    bool has_file = cur_ >= 0 && size_t(cur_) < cards_.size() && !cards_[size_t(cur_)].st.path.empty() && cards_[size_t(cur_)].kind == Card::State;
    help_.draw(a, cx.th, m, st_, has_file ? kHelpState : kHelpPlain, has_file ? 4 : 2, out);
  }

  bool overlay() const override { return true; }
  bool closes_on_launch() const override { return true; }
  std::optional<float> wake_in() const override { return std::nullopt; }

  // tests
  size_t size() const { return cards_.size(); }
  int cursor() const { return cur_; }
  const Card& card(size_t i) const { return cards_[i]; }

 private:
  void draw_card(Ctx& cx, std::vector<Op>& out, Card& c, float x, bool sel) {
    Assets& a = cx.assets;
    const Metrics& m = m_;
    float y = cards_y_;
    int rad = int(std::round(m.pill_h * 0.35f));
    Color fill = sel ? st_.cyan_soft : st_.panel;
    Color border = sel ? st_.cyan : st_.line;
    shapes_.framed(a, out, x, y, card_w_, card_h_, rad, fill, border, sel ? 2.0f : 1.0f);
    float tx = x + pad_, ty = y + pad_;
    // thumbnail area
    Color well = mix(st_.bg, st_.txt, st_.dark ? 0.06f : 0.10f);
    shapes_.rrect(a, out, tx, ty, thumb_w_, thumb_h_, std::max(rad / 2, 2), well);
    if (c.kind == Card::State) {
      if (!c.st.shot.empty()) {
        if (!c.have_img || a.evicted(c.img)) c.have_img = thumb(cx, c.st.shot, uint32_t(thumb_w_), uint32_t(thumb_h_), c.img);
        if (c.have_img) {
          const Img& im = a.img(c.img);
          float iw = float(im.w), ih = float(im.h);
          out.push_back(Op::image(c.img, std::round(tx + (thumb_w_ - iw) / 2.0f), std::round(ty + (thumb_h_ - ih) / 2.0f), iw, ih));
        }
      }
    } else {
      // a plus sign: the slot that starts something new
      float arm = std::round(thumb_w_ * 0.16f), th = std::max(std::round(0.012f * m.H), 2.0f);
      float cx0 = tx + thumb_w_ / 2.0f, cy0 = ty + thumb_h_ / 2.0f;
      Color pc = sel ? st_.cyan : st_.dim;
      out.push_back(Op::rect(std::round(cx0 - arm), std::round(cy0 - th / 2.0f), 2.0f * arm, th, pc));
      out.push_back(Op::rect(std::round(cx0 - th / 2.0f), std::round(cy0 - arm), th, 2.0f * arm, pc));
    }
    float lx = tx, w = thumb_w_;
    float y1 = ty + thumb_h_ + std::round(0.35f * float(m.em));
    if (!c.t1.alive(a)) {
      c.t1.set(a, c.line1, m.em, 0);  // a long label ("START NEW GAME" on a narrow card) shrinks instead of being cut
      if (c.t1.w > w) c.t1.set(a, c.line1, std::max(int(std::floor(float(m.em) * w / c.t1.w)), 1), w);
    }
    put_text(out, c.t1, lx, y1 + c.t1.h / 2.0f, sel ? st_.cyan_dark : st_.txt);
    if (!c.line2.empty()) {
      if (!c.t2.alive(a)) c.t2.set(a, c.line2, m.small_px, w);
      put_text(out, c.t2, lx, y1 + c.t1.h + std::round(0.1f * float(m.em)) + c.t2.h / 2.0f, st_.dim);
    }
  }

  void layout(Ctx& cx) {
    if (laid_ && lay_w_ == cx.w && lay_h_ == cx.h) return;
    laid_ = true;
    lay_w_ = cx.w, lay_h_ = cx.h;
    m_ = Metrics::from(cx.th, cx.w, cx.h);
    st_ = Style::from(cx.th);
    const Metrics& m = m_;
    margin_ = std::round(m.margin);
    card_w_ = std::round(std::min(0.30f * m.W, 0.38f * m.H));
    gap_ = std::round(0.5f * float(m.em));
    pad_ = std::round(0.5f * float(m.em));
    thumb_w_ = card_w_ - 2.0f * pad_;
    thumb_h_ = std::round(thumb_w_ * 0.75f);
    float lh1 = m.em * 1.25f, lh2 = m.small_px * 1.25f;
    card_h_ = std::round(pad_ + thumb_h_ + 0.35f * float(m.em) + lh1 + 0.1f * float(m.em) + lh2 + pad_);
    title_px_ = px_floor(std::floor(1.15f * float(m.em)));
    float title_h = float(title_px_) * 1.25f, game_h = float(m.small_px) * 1.25f;
    float bottom = m.help_y - float(m.foot_px) * 1.9f;  // clear air between the cards and the help bar
    cards_y_ = std::round(bottom - card_h_);
    float top_pad = std::round(0.8f * float(m.em));
    game_cy_ = std::round(cards_y_ - 0.5f * float(m.em) - game_h / 2.0f);
    title_cy_ = std::round(game_cy_ - game_h / 2.0f - title_h / 2.0f);
    sheet_top_ = std::round(title_cy_ - title_h / 2.0f - top_pad);
    vis_ = std::max(int(std::floor((m.W - 2.0f * margin_ + gap_) / (card_w_ + gap_))), 1);
    t_title_.reset(), t_count_.reset(), t_game_.reset();
    for (Card& c : cards_) c.t1.reset(), c.t2.reset(), c.have_img = false;
    scroll_ = scroll_to();
  }

  float max_scroll() const {
    float content = margin_ + float(cards_.size()) * (card_w_ + gap_) - gap_ + margin_;
    return std::max(content - m_.W, 0.0f);
  }
  // minimal scroll that keeps the selected card fully visible; the first card sits flush left (scroll 0)
  float scroll_to() const {
    float x = margin_ + float(cur_) * (card_w_ + gap_);
    float lo = x + card_w_ + margin_ - m_.W, hi = x - margin_;
    float s = std::min(std::max(scroll_, lo), hi);
    return std::min(std::max(s, 0.0f), max_scroll());
  }

  void sync(Ctx& cx) {
    if (gen_ != savestates::generation()) {
      const std::string keep = cur_ >= 0 && size_t(cur_) < cards_.size() ? cards_[size_t(cur_)].st.path : std::string();
      load(cx, keep.empty() ? nullptr : &keep);
    }
  }

  void load(Ctx& cx, const std::string* keep) {
    const System& sys = cx.systems[si_];
    const Game& g = sys.games[gi_];
    gen_ = savestates::generation();
    game_name_ = g.name;
    t_game_.reset();
    std::vector<savestates::State> st = savestates::list(sys, g);
    savestates::with_times(st);
    bool inc = savestates::incremental(cx.conf.map());
    if (inc) std::stable_sort(st.begin(), st.end(), [](const savestates::State& a, const savestates::State& b) { return a.mtime > b.mtime; });
    cards_.clear();
    Card nc;
    nc.kind = Card::NewGame;
    nc.line1 = tr("START NEW GAME");
    cards_.push_back(std::move(nc));
    bool has_auto = false;
    for (const auto& s : st) has_auto = has_auto || s.slot == -1;
    if (!has_auto && savestates::setting(cx.conf.map(), sys, g, "autosave") == "1") {
      Card c;
      c.kind = Card::NewAuto;
      c.line1 = tr("START NEW AUTO SAVE");
      cards_.push_back(std::move(c));
    }
    for (const auto& s : st) {
      Card c;
      c.kind = Card::State;
      c.st = s;
      std::string full = stamp(s.mtime, true);
      if (s.slot == -1) {
        c.line1 = tr("AUTO SAVE");
        c.line2 = full;
      } else if (inc) {
        c.line1 = full.substr(0, 10);
        c.line2 = full.substr(11);
      } else {
        c.line1 = std::string(tr("SLOT")) + " " + std::to_string(s.slot);
        c.line2 = full;
      }
      cards_.push_back(std::move(c));
    }
    cur_ = 0;
    if (keep)
      for (size_t i = 0; i < cards_.size(); ++i)
        if (cards_[i].st.path == *keep) cur_ = int(i);
    cur_ = std::min(cur_, int(cards_.size()) - 1);
    if (laid_) scroll_ = scroll_to();
  }

  Transition launch(Ctx& cx) {
    if (cur_ < 0 || size_t(cur_) >= cards_.size()) return Transition::none();
    const Card& c = cards_[size_t(cur_)];
    const System& sys = cx.systems[si_];
    const Game& g = sys.games[gi_];
    savestates::Pick pick = savestates::Pick::NewGame;
    int slot = 0;
    if (c.kind == Card::NewAuto) pick = savestates::Pick::Auto;
    if (c.kind == Card::State) {
      if (c.st.slot == -1) {
        pick = savestates::Pick::Auto;
      } else {
        pick = savestates::Pick::Slot;
        slot = c.st.slot;
      }
    }
    // `savestates::begin` copies state files and arms what `after_game` undoes, so it must run only when the game really starts:
    // with CheckBiosesAtLaunch on and a BIOS missing, it waits for YES (NO leaves the files as they were)
    const size_t si = si_, gi = gi_;
    auto go = [si, gi, pick, slot](Ctx& c) -> Transition {
      if (si >= c.systems.size() || gi >= c.systems[si].games.size()) return Transition::none();
      const System& s = c.systems[si];
      const Game& gm = s.games[gi];
      std::vector<JoyInfo> joys;
      if (c.pad) joys = c.pad->joysticks();
      std::string cmd = launch::build_command(s, gm, c.conf.map(), joys);
      if (cmd.empty()) return Transition::none();
      cmd += savestates::begin(s, gm, c.conf.map(), pick, slot);
      Transition t = Transition::launch(cmd);
      t.sys = int(si);
      t.game = int(gi);
      return t;
    };
    (void)sys;
    (void)g;
    if (launchcheck::bios_ok(cx, si)) return go(cx);
    return launchcheck::ask(go);
  }

  Transition remove(Ctx& cx) {
    if (cur_ < 0 || size_t(cur_) >= cards_.size() || cards_[size_t(cur_)].kind != Card::State) return Transition::none();
    savestates::State target = cards_[size_t(cur_)].st;
    size_t si = si_, gi = gi_;
    return ui::confirm(tr("ARE YOU SURE YOU WANT TO DELETE THIS ITEM?"), [target, si, gi](Ctx& cx2) {
      if (si < cx2.systems.size() && gi < cx2.systems[si].games.size()) savestates::remove_state(cx2.systems[si], cx2.systems[si].games[gi], target);
      return Transition::none();
    });
  }

  void copy(Ctx& cx) {
    if (cur_ < 0 || size_t(cur_) >= cards_.size() || cards_[size_t(cur_)].kind != Card::State) return;
    const System& sys = cx.systems[si_];
    savestates::copy_to_free_slot(sys, sys.games[gi_], cards_[size_t(cur_)].st);
    sync(cx);
  }

  size_t si_, gi_;
  std::vector<Card> cards_;
  int cur_ = 0, vis_ = 1;
  unsigned gen_ = 0;
  std::string game_name_, cnt_text_;
  // layout (pixels)
  bool laid_ = false;
  float lay_w_ = 0, lay_h_ = 0;
  Metrics m_;
  Style st_;
  float margin_ = 0, card_w_ = 0, card_h_ = 0, gap_ = 0, pad_ = 0, thumb_w_ = 0, thumb_h_ = 0;
  float cards_y_ = 0, title_cy_ = 0, game_cy_ = 0, sheet_top_ = 0;
  int title_px_ = 1;
  float scroll_ = 0;
  Shapes shapes_;
  HelpBar help_;
  Txt t_title_, t_count_, t_game_;
};

std::unique_ptr<Screen> make_screen(Ctx& cx, const std::string& arg) {
  size_t sys = 0;
  size_t game = 0;
  if (!savestates::parse_arg(cx, arg, sys, game)) return nullptr;
  if (!savestates::enabled(cx.systems[sys], cx.systems[sys].games[game], cx.conf.map())) return nullptr;
  return std::make_unique<SaveStateScreen>(cx, sys, game);
}

}  // namespace

ZF_REGISTER_SCREEN("menu.savestates", make_screen);

}  // namespace zf
