#include "menu_scraper.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "i18n.hpp"
#include "menu.hpp"
#include "menu_lang.hpp"
#include "registry.hpp"
#include "scene.hpp"
#include "ui_style.hpp"
#include "util.hpp"

namespace zf {
namespace {

using namespace scraper;

// zfront's own settings (ES ignores them): the filter of the games and the systems left out
constexpr const char* kKeyFilter = "ZfScraperFilter";  // all | missing | none
constexpr const char* kKeySkip = "ZfScraperSkip";      // comma separated system names that are NOT scraped
constexpr const char* kKeyRecent = "RecentlyScrappedFilter";  // ES: index into {0,1,7,15,31,90,365} days

std::vector<std::string> split_csv(const std::string& s) {
  std::vector<std::string> v;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find(',', i);
    if (j == std::string::npos) j = s.size();
    std::string t(trim(std::string_view(s).substr(i, j - i)));
    if (!t.empty()) v.push_back(t);
    i = j + 1;
  }
  return v;
}

std::vector<std::string> skipped(const Ctx& cx) {
  const std::string* v = cx.es.get(kKeySkip);
  return v ? split_csv(*v) : std::vector<std::string>();
}

void save_skipped(Ctx& cx, const std::vector<std::string>& v) {
  std::string s;
  for (const std::string& n : v) s += (s.empty() ? "" : ",") + n;
  cx.es.set_string(kKeySkip, s);
  cx.es.save();
}

bool is_skipped(const std::vector<std::string>& sk, const std::string& name) { return std::find(sk.begin(), sk.end(), name) != sk.end(); }

std::vector<size_t> selected_systems(const Ctx& cx) {
  std::vector<std::string> sk = skipped(cx);
  std::vector<size_t> v;
  for (size_t i : supported_systems(cx))
    if (!is_skipped(sk, cx.systems[i].name)) v.push_back(i);
  return v;
}

std::string get_str(const Ctx& cx, const char* k, const char* def) {
  const std::string* v = cx.es.get(k);
  return v ? *v : std::string(def);
}
void set_str(Ctx& cx, const char* k, const std::string& v) {
  cx.es.set_string(k, v);
  cx.es.save();
}
void set_flag(Ctx& cx, const char* k, bool v) {
  cx.es.set_bool(k, v);
  cx.es.save();
}

Options::Filter filter_of(const Ctx& cx) {
  std::string f = get_str(cx, kKeyFilter, "missing");
  if (f == "all") return Options::Filter::All;
  if (f == "none") return Options::Filter::MissingAll;
  return Options::Filter::MissingAny;
}

// ---- the main menu ----------------------------------------------------------------------------------------------------
Transition start_scrape(Ctx& cx) {
  (void)cx;
  return Transition::open("menu.scraper.run");
}

void build_main(MenuBuilder& m, Ctx& cx) {
  bool ok = available();
  std::vector<size_t> sel = selected_systems(cx);
  size_t all = supported_systems(cx).size();
  if (!ok) m.info(tr("THIS BUILD CANNOT REACH SCREENSCRAPER")).danger();
  m.choice(tr("GAMES TO SCRAPE"), {{"all", tr("ALL GAMES")}, {"missing", tr("WITH MISSING MEDIA")}, {"none", tr("WITHOUT ANY MEDIA")}},
           [&cx] { return get_str(cx, kKeyFilter, "missing"); }, [&cx](const std::string& v) { set_str(cx, kKeyFilter, v); })
      .hint(tr("Which games of the chosen systems are looked up."));
  m.choice(tr("IGNORE RECENTLY SCRAPED GAMES"),
           {{"0", tr("NO")}, {"1", tr("1 DAY")}, {"2", tr("1 WEEK")}, {"3", tr("15 DAYS")}, {"4", tr("1 MONTH")}, {"5", tr("3 MONTHS")}, {"6", tr("1 YEAR")}},
           [&cx] { return std::to_string(cx.es.get_int(kKeyRecent, 3)); }, [&cx](const std::string& v) { set_str(cx, kKeyRecent, v); })
      .hint(tr("Games scraped inside this time are skipped, saving requests."));
  m.submenu(tr("SYSTEMS"), "menu.scraper.systems").value(std::to_string(sel.size()) + " / " + std::to_string(all));
  m.submenu(tr("SCRAPER SETTINGS"), "menu.scraper.options");
  m.submenu(tr("ACCOUNT"), "menu.scraper.account");
  m.action(tr("START"), start_scrape).disabled(!ok || sel.empty()).hint(sel.empty() && ok ? tr("Choose at least one system.") : tr("Looks the games up on ScreenScraper."));
}

// ---- systems ---------------------------------------------------------------------------------------------------------
void build_systems(MenuBuilder& m, Ctx& cx) {
  std::vector<size_t> sup = supported_systems(cx);
  if (sup.empty()) {
    m.info(tr("NO SYSTEM WITH GAMES THAT SCREENSCRAPER KNOWS"));
    return;
  }
  Refresh rf = m.refresh;
  m.action(tr("SELECT ALL"), [&cx, rf] {
    save_skipped(cx, {});
    rf();
  });
  m.action(tr("SELECT NONE"), [&cx, rf] {
    std::vector<std::string> all;
    for (size_t i : supported_systems(cx)) all.push_back(cx.systems[i].name);
    save_skipped(cx, all);
    rf();
  });
  m.separator();
  for (size_t i : sup) {
    std::string name = cx.systems[i].name;
    std::string label = cx.systems[i].fullname.empty() ? name : cx.systems[i].fullname;
    m.toggle(
        label, [&cx, name] { return !is_skipped(skipped(cx), name); },
        [&cx, name](bool on) {
          std::vector<std::string> sk = skipped(cx);
          sk.erase(std::remove(sk.begin(), sk.end(), name), sk.end());
          if (!on) sk.push_back(name);
          save_skipped(cx, sk);
        })
        .with_id("sys:" + name);
  }
}

// ---- settings --------------------------------------------------------------------------------------------------------
void build_options(MenuBuilder& m, Ctx& cx) {
  auto src = [&](const char* label, const char* key, const char* def, std::vector<Option> opts, const char* hint) {
    m.choice(label, std::move(opts), [&cx, key, def] { return get_str(cx, key, def); }, [&cx, key](const std::string& v) { set_str(cx, key, v); }).hint(hint);
  };
  auto flag = [&](const char* label, const char* key, bool def, const char* hint) {
    m.toggle(label, [&cx, key, def] { return cx.es.get_bool(key, def); }, [&cx, key](bool v) { set_flag(cx, key, v); }).hint(hint);
  };
  m.header(tr("PICTURES"));
  src(tr("IMAGE"), key::kImage, "ss",
      {{"ss", tr("SCREENSHOT")}, {"sstitle", tr("TITLE SCREEN")}, {"mixrbv1", tr("MIX V1")}, {"mixrbv2", tr("MIX V2")}, {"box-2D", tr("BOX FRONT")}, {"box-3D", tr("BOX 3D")}},
      tr("The main picture of each game."));
  src(tr("THUMBNAIL"), key::kThumb, "box-2D", {{"box-2D", tr("BOX FRONT")}, {"box-3D", tr("BOX 3D")}, {"ss", tr("SCREENSHOT")}, {"", tr("NONE")}},
      tr("The small picture of each game."));
  src(tr("LOGO"), key::kLogo, "wheel", {{"wheel", tr("WHEEL")}, {"wheel-hd", tr("WHEEL HD")}, {"marquee", tr("MARQUEE")}, {"", tr("NONE")}}, tr("The logo of each game."));
  m.choice(tr("IMAGE SIZE"), {{"0", tr("FULL SIZE")}, {"320", "320"}, {"480", "480"}, {"640", "640"}, {"1024", "1024"}},
           [&cx] { return std::to_string(cx.es.get_int(key::kResizeW, 640)); }, [&cx](const std::string& v) {
             cx.es.set_int(key::kResizeW, std::atoi(v.c_str()));
             cx.es.save();
           })
      .hint(tr("ScreenScraper shrinks pictures before sending them: faster and lighter."));
  src(tr("REGION"), key::kRegion, "",
      {{"", tr("AUTOMATIC")}, {"eu", tr("EUROPE")}, {"us", tr("USA")}, {"jp", tr("JAPAN")}, {"wor", tr("WORLD")}, {"sp", tr("SPAIN")}, {"fr", tr("FRANCE")}, {"de", tr("GERMANY")}, {"it", tr("ITALY")}, {"br", tr("BRAZIL")}},
      tr("Preferred region of names, dates and pictures."));
  m.header(tr("INFORMATION"));
  flag(tr("NAMES"), key::kNames, true, tr("Replace the names of the games."));
  flag(tr("DESCRIPTIONS"), key::kDesc, true, tr("Replace the descriptions."));
  flag(tr("RATINGS"), key::kRatings, true, tr("Download the rating of each game."));
  m.header(tr("EXTRA MEDIA"));
  flag(tr("VIDEOS"), key::kVideos, true, tr("Videos are big and take longer to download."));
  flag(tr("MANUALS"), key::kManual, true, tr("PDF manuals."));
  flag(tr("MAPS"), key::kMap, true, tr("Maps of the game."));
  flag(tr("FANART"), key::kFanart, false, tr("Background pictures."));
  flag(tr("BOX BACK"), key::kBoxBack, false, tr("The back of the box."));
  m.header(tr("FILES"));
  flag(tr("REPLACE EXISTING MEDIA"), key::kOverwrite, true, tr("Only for \"All games\": pictures already there are downloaded again."));
}

// ---- account ---------------------------------------------------------------------------------------------------------
struct AcctState {
  bool testing = false, have = false;
  AccountInfo info;
};

void build_account(MenuBuilder& m, Ctx& cx, const std::shared_ptr<AcctState>& st) {
  Refresh rf = m.refresh;
  m.text(tr("USERNAME"), [&cx] { return get_str(cx, key::kUser, ""); }, [&cx](const std::string& v) { set_str(cx, key::kUser, v); })
      .hint(tr("Optional. Without an account ScreenScraper allows very few requests."));
  m.text(tr("PASSWORD"), [&cx] { return get_str(cx, key::kPass, ""); }, [&cx](const std::string& v) { set_str(cx, key::kPass, v); }).secret();
  m.action(tr("TEST ACCOUNT"), [&cx, st, rf] {
    if (st->testing || !available()) return;
    st->testing = true;
    st->have = false;
    rf();
    check_account(cx, Options::load(cx), [st, rf](const AccountInfo& ai) {
      st->testing = false;
      st->have = true;
      st->info = ai;
      rf();
    });
  }).disabled(!available() || st->testing).hint(tr("Asks ScreenScraper for your limits."));
  if (st->testing) m.info(tr("CONNECTING TO SCREENSCRAPER"));
  if (st->have) {
    const AccountInfo& a = st->info;
    if (a.fail != Abort::None) {
      m.info(describe(a.fail)).danger();
    } else {
      m.info(tr("LOGIN"), a.id.empty() ? std::string(tr("ANONYMOUS")) : a.id);
      if (a.max_per_day > 0) m.info(tr("REQUESTS TODAY"), std::to_string(a.requests_today) + " / " + std::to_string(a.max_per_day));
      if (a.max_per_min > 0) m.info(tr("REQUESTS PER MINUTE"), std::to_string(a.max_per_min));
    }
  }
}

// ---- progress and summary ---------------------------------------------------------------------------------------------
const ui::Prompt kRunHelp[] = {{"B", N_("STOP")}};
const ui::Prompt kStopHelp[] = {{"updown", N_("SCROLL")}};
const ui::Prompt kDoneHelp[] = {{"updown", N_("SCROLL")}, {"B", N_("BACK")}};

std::string megabytes(uint64_t b) {
  char buf[32];
  if (b >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.1f GB", double(b) / double(1ull << 30));
  else std::snprintf(buf, sizeof buf, "%.1f MB", double(b) / double(1u << 20));
  return buf;
}

enum class Tone : uint8_t { Normal, Dim, Accent, Danger };

struct Row {
  enum Kind : uint8_t { Pair, Text, Bar, Header, Sep } kind = Pair;
  std::string l, r;
  Tone tone = Tone::Normal;
  float frac = 0;
  ui::Txt tl, tr_;
  std::string kl, kr;  // what the cached images show
  float wl = 0, wr = 0;
};

class RunScreen : public Screen {
 public:
  RunScreen(Ctx& cx, Options o) : run_(Run::create(cx, std::move(o))) {
    run_->set_watched(true);
    run_->start();
  }
  ~RunScreen() override {
    run_->set_watched(false);
    if (run_->working()) run_->cancel();
  }

  Transition handle(Action a, Ctx& cx) override {
    (void)cx;
    switch (a) {
      case Action::B:
      case Action::A:
        if (run_->finished()) return Transition::pop();
        if (a == Action::B && run_->phase() != Phase::Stopping) run_->cancel();
        return Transition::none();
      case Action::Up:
        if (top_ > 0) --top_, redraw_ = true;
        return Transition::none();
      case Action::Down:
        ++top_;  // clamped at draw time
        redraw_ = true;
        return Transition::none();
      default:
        return Transition::none();
    }
  }

  bool tick(float dt, Ctx& cx) override {
    (void)dt, (void)cx;
    run_->tick();
    if (run_->wait_seconds() != shown_wait_ || run_->version() != built_ver_) redraw_ = true;
    return false;
  }
  std::optional<float> wake_in() const override {
    std::optional<float> w = run_->wake_in();
    if (run_->wait_seconds() > 0) w = w ? std::min(*w, 1.0f) : 1.0f;
    return w;
  }
  bool take_redraw() override {
    bool r = redraw_;
    redraw_ = false;
    return r;
  }
  bool is_menu() const override { return true; }
  void on_dismiss(Ctx& cx) override {
    (void)cx;
    if (run_->working()) run_->cancel();
  }

  void draw(Ctx& cx, std::vector<Op>& out) override {
    Assets& a = cx.assets;
    if (!have_layout_ || lay_w_ != cx.w || lay_h_ != cx.h) {
      m_ = ui::Metrics::from(cx.th, cx.w, cx.h);
      st_ = ui::Style::from(cx.th);
      lay_w_ = cx.w, lay_h_ = cx.h;
      have_layout_ = true;
      built_ver_ = ~uint64_t(0);
      t_title_.reset();
    }
    const ui::Metrics& m = m_;
    const ui::Style& st = st_;
    shown_wait_ = run_->wait_seconds();
    if (run_->version() != built_ver_ || shown_wait_ != built_wait_) rebuild();

    out.push_back(Op::rect(0, 0, m.W, m.H, st.bg));
    float inset = std::round(m.pill_h * ui::kPillInset);
    std::string title = title_text();
    if (title != title_key_ || !t_title_.alive(a)) {
      t_title_.set(a, title, m.title_px, m.col_w * 0.72f);
      title_key_ = title;
    }
    ui::put_text(out, t_title_, m.col_x + inset, m.header_cy, st.txt);
    status_bar(cx.th, a, m.W, m.H, cx.status, out);
    float title_bottom = m.header_cy + t_title_.h / 2.0f;
    float first_pill_top = m.list_top + (m.row_h - m.pill_h) / 2.0f;
    float line_y = std::round((title_bottom + first_pill_top) / 2.0f);
    out.push_back(Op::rect(m.col_x, line_y, m.col_w, 1, st.line));
    ui::header_box() = {true, m.col_x + inset, m.col_x + inset + t_title_.w, m.header_cy - t_title_.h / 2.0f, title_bottom, line_y};

    // rows
    int total = int(rows_.size());
    int max_top = std::max(total - m.lines, 0);
    top_ = std::min(top_, max_top);
    float xl = m.col_x + inset, xr = m.col_x + m.col_w - inset, inner = xr - xl;
    float gap = std::round(float(m.em));
    out.push_back(Op::clip(m.col_x, m.list_top, m.col_w, m.list_h));
    for (int i = top_; i < total && i < top_ + m.lines + 1; ++i) {
      Row& r = rows_[size_t(i)];
      float y = m.list_top + float(i - top_) * m.row_h;
      float cy = y + m.row_h / 2.0f;
      Color c = tone_color(r.tone);
      switch (r.kind) {
        case Row::Sep:
          out.push_back(Op::rect(xl, std::round(cy), inner, 1, st.line));
          break;
        case Row::Header:
          fit(a, r.tl, r.kl, r.l, m.small_px, inner, r.wl);
          ui::put_text(out, r.tl, xl, y + m.row_h * 0.66f, st.group_txt);
          break;
        case Row::Text:
          fit(a, r.tl, r.kl, r.l, m.em, inner, r.wl);
          ui::put_text(out, r.tl, xl, cy, c);
          break;
        case Row::Bar: {
          float th = std::max(std::round(float(m.em) * 0.55f), 6.0f);
          float by = std::round(cy - th / 2.0f);
          shapes_.pill(a, out, xl, by, inner, th, st.track);
          float fw = std::max(std::round(inner * std::min(std::max(r.frac, 0.0f), 1.0f)), r.frac > 0 ? th : 0.0f);
          if (fw > 0) shapes_.pill(a, out, xl, by, fw, th, st.cyan);
          break;
        }
        case Row::Pair: {
          fit(a, r.tr_, r.kr, r.r, m.em, inner * 0.6f, r.wr);
          float rw = r.tr_.blank ? 0 : r.tr_.w;
          fit(a, r.tl, r.kl, r.l, m.em, std::max(inner - rw - (rw > 0 ? gap : 0), 3.0f * float(m.em)), r.wl);
          ui::put_text(out, r.tl, xl, cy, c);
          if (rw > 0) ui::put_text_right(out, r.tr_, xr, cy, r.tone == Tone::Danger ? st.danger : st.dim);
          break;
        }
      }
    }
    out.push_back(Op::unclip());
    if (total > m.lines) {
      float content = float(total) * m.row_h;
      float maxs = content - m.list_h;
      float th = std::max(std::round(m.list_h * m.list_h / content), 2.0f * float(m.em));
      float ty = m.list_top + (maxs > 0 ? std::min(std::max(float(top_) * m.row_h / maxs, 0.0f), 1.0f) : 0.0f) * (m.list_h - th);
      float bw = std::max(std::round(0.007f * m.H), 3.0f);
      shapes_.pill(a, out, m.col_x + m.col_w + std::max(std::round(inset * 0.35f), 2.0f), ty, bw, th, ui::with_alpha(st.txt, 0.30f));
    }
    const ui::Prompt* hp = kRunHelp;
    size_t hn = 1;
    if (run_->finished()) hp = kDoneHelp, hn = 2;
    else if (run_->phase() == Phase::Stopping) hp = kStopHelp, hn = 1;
    help_.draw(a, cx.th, m, st, hp, hn, out);
  }

  const Run& run() const { return *run_; }
  Run& run() { return *run_; }

 private:
  Color tone_color(Tone t) const {
    switch (t) {
      case Tone::Dim: return st_.dim;
      case Tone::Accent: return st_.cyan_dark;
      case Tone::Danger: return st_.danger;
      default: return st_.txt;
    }
  }
  static void fit(Assets& a, ui::Txt& t, std::string& key, const std::string& s, int px, float max_w, float& made_w) {
    if (t.alive(a) && key == s && made_w == max_w) return;
    t.set(a, s, px, max_w);
    key = s;
    made_w = max_w;
  }
  std::string title_text() const {
    switch (run_->phase()) {
      case Phase::Finished: return tr("SCRAPE COMPLETE");
      case Phase::Cancelled: return tr("SCRAPE STOPPED");
      case Phase::Aborted: return tr("SCRAPE FAILED");
      default: return tr("SCRAPING");
    }
  }
  static Row pair(std::string l, std::string r, Tone t = Tone::Normal) {
    Row w;
    w.kind = Row::Pair, w.l = std::move(l), w.r = std::move(r), w.tone = t;
    return w;
  }
  static Row text(std::string l, Tone t = Tone::Normal) {
    Row w;
    w.kind = Row::Text, w.l = std::move(l), w.tone = t;
    return w;
  }
  void counters(std::vector<Row>& v, bool with_media) const {
    const Stats& s = run_->stats();
    v.push_back(pair(tr("FOUND"), std::to_string(s.found)));
    v.push_back(pair(tr("NOT FOUND"), std::to_string(s.not_found)));
    v.push_back(pair(tr("ERRORS"), std::to_string(s.errors), s.errors ? Tone::Danger : Tone::Normal));
    if (with_media) v.push_back(pair(tr("MEDIA FILES"), std::to_string(s.media) + "  " + megabytes(s.bytes)));
  }
  void rebuild() {
    std::vector<Row> v;
    const Stats& s = run_->stats();
    if (!run_->finished()) {
      std::string sysline = run_->system_name();
      std::string right = run_->systems_total() > 1 ? std::to_string(std::min(run_->systems_done(), run_->systems_total())) + " / " + std::to_string(run_->systems_total()) : std::string();
      std::string status;
      switch (run_->phase()) {
        case Phase::Preparing: status = tr("READING GAME LISTS"); break;
        case Phase::Connecting: status = tr("CONNECTING TO SCREENSCRAPER"); break;
        case Phase::Stopping: status = tr("STOPPING"); break;
        default:
          if (run_->wait_seconds() > 0) status = std::string(tr("WAITING")) + " " + std::to_string(run_->wait_seconds()) + " " + tr("SECONDS");
          break;
      }
      v.push_back(pair(sysline.empty() ? std::string(tr("SCREENSCRAPER")) : sysline, run_->phase() == Phase::Running ? std::string() : right, Tone::Dim));
      v.push_back(text(run_->phase() == Phase::Running || run_->phase() == Phase::Stopping ? run_->game_name() : std::string()));
      Row bar;
      bar.kind = Row::Bar;
      bar.frac = s.total ? float(s.done) / float(s.total) : 0.0f;
      v.push_back(std::move(bar));
      std::string count = s.total ? std::to_string(s.done) + " / " + std::to_string(s.total) + "  " + std::to_string(int(bar_pct(s))) + "%" : std::string();
      v.push_back(pair(status, count, Tone::Dim));
      counters(v, false);  // 7 rows: the shortest screens show no more
    } else {
      if (run_->phase() == Phase::Aborted) v.push_back(text(describe(run_->abort_reason()), Tone::Danger));
      else if (run_->phase() == Phase::Cancelled) v.push_back(text(tr("STOPPED. WHAT WAS DONE IS SAVED"), Tone::Dim));
      if (s.total == 0 && run_->phase() == Phase::Finished) v.push_back(text(tr("NO GAMES TO SCRAPE WITH THESE FILTERS"), Tone::Dim));
      else v.push_back(pair(tr("GAMES"), std::to_string(s.done) + " / " + std::to_string(s.total)));
      counters(v, true);
      if (!run_->errors().empty()) {
        Row h;
        h.kind = Row::Header, h.l = tr("PROBLEMS");
        v.push_back(std::move(h));
        for (const ErrLine& e : run_->errors())
          v.push_back(pair(e.game, e.warning ? std::string(tr("MEDIA MISSING")) : std::string(describe(e.code)), e.warning ? Tone::Dim : Tone::Danger));
      }
    }
    // keep the images of the rows that did not change
    for (size_t i = 0; i < v.size() && i < rows_.size(); ++i) {
      Row& o = rows_[i];
      Row& n = v[i];
      if (o.kind == n.kind && o.l == n.l && o.r == n.r) {
        n.tl = std::move(o.tl), n.tr_ = std::move(o.tr_);
        n.kl = o.kl, n.kr = o.kr, n.wl = o.wl, n.wr = o.wr;
      }
    }
    rows_ = std::move(v);
    built_ver_ = run_->version();
    built_wait_ = run_->wait_seconds();
  }
  static float bar_pct(const Stats& s) { return s.total ? std::floor(100.0f * float(s.done) / float(s.total)) : 0.0f; }

  std::shared_ptr<Run> run_;
  std::vector<Row> rows_;
  uint64_t built_ver_ = ~uint64_t(0);
  int built_wait_ = -1, shown_wait_ = 0;
  int top_ = 0;
  bool redraw_ = false;
  ui::Metrics m_;
  ui::Style st_;
  bool have_layout_ = false;
  float lay_w_ = 0, lay_h_ = 0;
  ui::Shapes shapes_;
  ui::HelpBar help_;
  ui::Txt t_title_;
  std::string title_key_;
};

}  // namespace

Options scraper_run_options(Ctx& cx) {
  Options o = Options::load(cx);
  o.filter = filter_of(cx);
  o.systems = selected_systems(cx);
  return o;
}

std::unique_ptr<Screen> make_scraper_run_screen(Ctx& cx, Options o) { return std::make_unique<RunScreen>(cx, std::move(o)); }

namespace {

ZF_REGISTER_SCREEN("menu.scraper", [](Ctx&, const std::string&) {
  MenuSpec s;
  s.title = tr("SCRAPER");
  s.build = build_main;
  return std::make_unique<LangMenu>(std::move(s));
});

ZF_REGISTER_SCREEN("menu.scraper.systems", [](Ctx&, const std::string&) {
  MenuSpec s;
  s.title = tr("SYSTEMS");
  s.build = build_systems;
  return std::make_unique<LangMenu>(std::move(s));
});

ZF_REGISTER_SCREEN("menu.scraper.options", [](Ctx&, const std::string&) {
  MenuSpec s;
  s.title = tr("SCRAPER SETTINGS");
  s.build = build_options;
  return std::make_unique<LangMenu>(std::move(s));
});

ZF_REGISTER_SCREEN("menu.scraper.account", [](Ctx&, const std::string&) {
  auto st = std::make_shared<AcctState>();
  MenuSpec s;
  s.title = tr("ACCOUNT");
  s.build = [st](MenuBuilder& m, Ctx& cx) { build_account(m, cx, st); };
  return std::make_unique<LangMenu>(std::move(s));
});

ZF_REGISTER_SCREEN("menu.scraper.run", [](Ctx& cx, const std::string&) { return make_scraper_run_screen(cx, scraper_run_options(cx)); });

}  // namespace
}  // namespace zf
