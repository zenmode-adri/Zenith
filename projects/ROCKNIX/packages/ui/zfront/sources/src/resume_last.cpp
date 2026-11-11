#include "resume_last.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "bootargs.hpp"
#include "covers.hpp"
#include "i18n.hpp"
#include "recent.hpp"
#include "remake.hpp"
#include "savestates.hpp"
#include "ui_style.hpp"
#include "util.hpp"

namespace zf::resume {

Paths default_paths() {
  const char* e = std::getenv("ZF_LAST_SESSION");
  return Paths{boot::startup_marker(), e && *e ? e : "/storage/.config/zfront/last-session"};
}

bool enabled(const Ctx& cx) { return cx.conf.get_or(kKey, "") == "1"; }

std::optional<Target> newest(Ctx& cx, time_t now) {
  std::optional<Target> best;
  time_t best_t = 0;
  for (size_t si = 0; si < cx.systems.size(); ++si) {
    if (cx.item_of(si) < 0) continue;  // hidden: the carousel does not show its row either
    const std::vector<RecentItem> v = recent_states(cx.systems[si], now, 1);
    if (v.empty() || (best && v[0].mtime <= best_t)) continue;
    Target t;
    t.sys = si, t.slot = v[0].slot, t.name = v[0].name, t.shot = v[0].shot;
    best = t;
    best_t = v[0].mtime;
  }
  if (!best) return std::nullopt;
  size_t game = 0;
  if (!savestates::find_game(cx, best->sys, best->name, game) || !file_exists(cx.systems[best->sys].games[game].path)) return std::nullopt;
  best->game = game;
  const std::string& g = cx.systems[best->sys].games[game].name;
  if (!g.empty()) best->name = g;
  return best;
}

void note_session(const std::string& path, unsigned long secs, const std::string& rom) {
  if (path.empty()) return;
  for (size_t p = 1; p < path.size(); ++p)
    if (path[p] == '/') ::mkdir(path.substr(0, p).c_str(), 0755);
  if (FILE* f = std::fopen(path.c_str(), "w")) {
    std::fprintf(f, "%lu %s\n", secs, rom.find_first_of("\r\n") == std::string::npos ? rom.c_str() : "");
    std::fclose(f);
  }
}

bool session_allows(const std::string& path, const std::string& rom, unsigned long& secs) {
  secs = 0;
  std::string s;
  if (!read_file(path, s)) return true;
  const std::string_view line = trim(s);
  const size_t sp = line.find(' ');
  long v = 0;
  if (!parse_int(line.substr(0, sp), v) || v < 0) return true;
  const std::string_view who = sp == std::string_view::npos ? std::string_view() : trim(line.substr(sp + 1));
  if (!who.empty() && who != rom) return true;  // the short run was of something else
  secs = static_cast<unsigned long>(v);
  return secs >= kMinSession;
}

// ---- the screen ------------------------------------------------------------------------------------------------------------------

namespace {
const ui::Prompt kHelp[] = {{"A", N_("START NOW")}, {"B", N_("CANCEL")}};
}

struct ResumeScreen::View {
  float W = 0, H = 0;
  bool laid = false;
  ui::Metrics m;
  ui::Style st;
  ui::HelpBar help;
  ui::Txt title, count;
  int count_for = -1;
};

ResumeScreen::ResumeScreen(Target t) : t_(std::move(t)), v_(std::make_shared<View>()) {}

int ResumeScreen::seconds_left() const { return std::max(0, int(std::ceil(kCountdown - elapsed_ - 1e-4f))); }

Transition ResumeScreen::start(Ctx& cx) {
  done_ = true;
  Transition t = savestates::launch_state(cx, t_.sys, t_.game, t_.slot);  // exactly the launch of the "Continue playing" row
  if (t.kind == Transition::None) return Transition::pop();
  return t;
}

Transition ResumeScreen::handle(Action a, Ctx& cx) {
  if (done_) return Transition::none();
  if (a == Action::A) return start(cx);
  if (a == Action::B || a == Action::Start) {
    done_ = true;
    return Transition::pop();
  }
  return Transition::none();
}

bool ResumeScreen::tick(float dt, Ctx& cx) {
  (void)cx;
  // before the first frame nothing counts, and a stall of the loop counts at most a second: the person always gets to see the screen
  if (!armed_ || done_) return false;
  elapsed_ += std::min(std::max(dt, 0.0f), 1.0f);
  if (seconds_left() != shown_) redraw_ = true;
  return false;
}

std::optional<float> ResumeScreen::wake_in() const {
  if (done_) return 0.02f;  // the loop must come back to let `take_transition` remove the screen
  if (!armed_) return std::nullopt;
  const float left = kCountdown - elapsed_;
  const float next = left - float(std::max(seconds_left() - 1, 0));  // until the number on screen changes (or the end)
  return std::max(next, 0.02f);
}

bool ResumeScreen::take_redraw() {
  const bool r = redraw_;
  redraw_ = false;
  return r;
}

Transition ResumeScreen::take_transition(Ctx& cx) {
  // done_ and still on top: the launch was asked and only the question before it (the missing-BIOS check) was answered NO. Nothing is
  // left to wait for, so the screen goes instead of staying as a frozen "starting in 0".
  if (done_) return Transition::pop();
  if (!armed_) return Transition::none();
  if (elapsed_ >= kCountdown - 1e-4f) return start(cx);
  return Transition::none();
}

void ResumeScreen::draw(Ctx& cx, std::vector<Op>& out) {
  using namespace ui;
  armed_ = true;
  View& v = *v_;
  Assets& a = cx.assets;
  if (!v.laid || v.W != cx.w || v.H != cx.h) {
    v.laid = true;
    v.W = cx.w, v.H = cx.h;
    v.m = Metrics::from(cx.th, cx.w, cx.h);
    v.st = Style::from(cx.th);
    v.title.reset(), v.count.reset();
    v.count_for = -1;
  }
  const Metrics& m = v.m;
  const Style& st = v.st;
  out.push_back(Op::rect(0, 0, v.W, v.H, st.bg));

  if (!v.title.alive(a)) v.title.set(a, std::string(tr("RESUMING")) + " " + short_title(t_.name), m.title_px, v.W - 2.0f * m.margin);
  const float tx = title_x(st, m, m.title_left, v.title.w);
  put_text(out, v.title, tx, m.header_cy, st.txt);
  const float title_bottom = m.header_cy + v.title.h / 2.0f;
  HeaderBox& hb = header_box();
  hb = {true, tx, tx + v.title.w, m.header_cy - v.title.h / 2.0f, title_bottom, title_bottom};

  shown_ = seconds_left();
  if (!v.count.alive(a) || v.count_for != shown_) {
    std::string s = tr("STARTING IN %i");
    const size_t p = s.find("%i");
    if (p != std::string::npos) s.replace(p, 2, std::to_string(shown_));
    v.count.set(a, s, m.em, v.W - 2.0f * m.margin);
    v.count_for = shown_;
  }
  const float gap = std::round(0.5f * float(m.em));
  const float help_top = help_pill_top(a, cx.th, m);
  const float count_cy = std::round(help_top - gap - v.count.h / 2.0f);
  put_text(out, v.count, std::round((v.W - v.count.w) / 2.0f), count_cy, st.dim);
  if (remake::audit_sink()) remake::audit(std::round((v.W - v.count.w) / 2.0f), count_cy - v.count.h / 2.0f, v.count.w, v.count.h, "cuenta", 'C');

  // the game's picture, as large as fits between the title and the number, never cut
  const float top = std::round(title_bottom + gap), bottom = std::round(count_cy - v.count.h / 2.0f - gap);
  const float aw = v.W - 2.0f * m.margin, ah = bottom - top;
  ImgId id = 0;
  bool have = false;
  if (!t_.shot.empty() && ah > 8 && aw > 8) {
    const uint32_t bw = uint32_t(aw), bh = uint32_t(ah);
    if (cx.async_covers) {
      have = covers::request(a, cx.sv->jobs, t_.shot, bw, bh) && covers::ready(a, t_.shot, bw, bh, id);
    } else if (!covers::failed(t_.shot)) {
      have = a.file_fit(t_.shot, bw, bh, id);
      if (!have) covers::mark_failed(t_.shot);
    }
  }
  if (have) {
    const Img& im = a.img(id);
    const float fit = std::min(aw / float(im.w), ah / float(im.h));
    const float dw = std::round(float(im.w) * fit), dh = std::round(float(im.h) * fit);
    out.push_back(Op::image(id, std::round((v.W - dw) / 2.0f), std::round(top + (ah - dh) / 2.0f), dw, dh));
    if (remake::audit_sink()) remake::audit(std::round((v.W - dw) / 2.0f), std::round(top + (ah - dh) / 2.0f), dw, dh, "imagen", 'C');
  }
  v.help.draw(a, cx.th, m, st, kHelp, 2, out);
}

std::unique_ptr<Screen> begin(Ctx& cx, const Paths& p, time_t now, std::string& why) {
  why.clear();
  if (!enabled(cx)) return nullptr;
  if (!cx.conf.get_or(boot::kBootGamePath, "").empty()) return nullptr;  // the boot game wins
  // Once per boot, decided now: claimed even when nothing resumes below, so that a later restart of the frontend (after the person
  // played something and zfront crashed) never starts a game by itself.
  const std::string claimed = boot::startup_game("resume", p.marker, [](const std::string&) { return true; }, why);
  if (claimed.empty()) return nullptr;
  std::optional<Target> t = newest(cx, now);
  if (!t) return nullptr;
  unsigned long secs = 0;
  if (!session_allows(p.session, cx.systems[t->sys].games[t->game].path, secs)) {
    why = "not resuming " + t->name + ": its last session lasted " + std::to_string(secs) + " s (under " + std::to_string(kMinSession) + " s, it probably crashed)";
    return nullptr;
  }
  return std::make_unique<ResumeScreen>(std::move(*t));
}

}  // namespace zf::resume
