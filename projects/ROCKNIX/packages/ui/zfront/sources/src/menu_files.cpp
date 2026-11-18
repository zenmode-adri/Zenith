// `menu.files` (TOOLS > "File Manager"): zfront's own file manager. One list, drawn by hand on the grid of the remade themes with the plain colours of the
// theme (light or dark, high contrast): no skin and no backdrop, so it looks the same in every theme.
//
//   A open (folder) / properties (file)   B back (clears the selection first)   X actions   Y mark   Select go to   Start close   L / R page
//   X: copy, move, paste, rename, delete, new folder, properties, select all, sort, show hidden files
//   Select: games, game card, USB drives, screenshots, home (what is not there is not offered)
//
// What it does lives in fileops.cpp (tests on a temp folder). Copy, move and delete run on the slow job lane with a progress card and B to cancel.
// Where it may go: the roots (`/storage`, `/media`; `ZF_FILES_ROOT` replaces them in tests or in the harness, or the argument of the screen).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <sys/stat.h>

#include "app.hpp"
#include "collections.hpp"
#include "fileops.hpp"
#include "i18n.hpp"
#include "menu_files.hpp"
#include "menu_system.hpp"
#include "registry.hpp"
#include "remake.hpp"
#include "scene.hpp"
#include "services.hpp"
#include "ui_dialogs.hpp"
#include "ui_prefs.hpp"
#include "ui_style.hpp"
#include "util.hpp"

namespace zf {
namespace {

namespace fo = fileops;
using remake::Grid;

ui::Shapes g_sh;
std::atomic<bool> g_busy{false};  // a copy / move / delete is running (one at a time)
std::atomic<int> g_gen{0};        // bumped when a job ends: the list is read again
std::string g_last_confirm;       // the last question about deleting or renaming (what the tests read)

struct Clip {
  std::vector<std::string> paths;
  bool move = false;
} g_clip;

struct Job {
  fo::Progress pr;
  std::atomic<bool> finished{false};
  fo::Result res;
  std::atomic<bool> measuring{true};  // still walking the tree to count it: the card says "Calculating..."
  int kind = 0;                       // 0 copy, 1 move, 2 delete
};

// ---- drawing helpers (same as the battery screen's) ---------------------------------------------------------------------------------------------

void label(Assets& a, std::vector<Op>& ops, std::string_view s, float x, float cy, int px, Color c, float max_w = 0, bool right = false) {
  if (s.empty()) return;
  ImgId id = max_w > 0 ? a.text_ellipsis(s, float(px), c, max_w) : a.text(s, float(px), c);
  const float tw = float(a.img(id).w), th = float(a.img(id).h);
  ops.push_back(Op::image(id, std::round(right ? x - tw : x), std::round(cy - th / 2.0f), tw, th));
  if (remake::audit_sink()) remake::audit(right ? x - tw : x, cy - th / 2.0f, tw, th, "\"" + std::string(s.substr(0, 18)) + "\"", right ? 'R' : 'L');
}

void box(float x, float y, float w, float h, const char* tag, bool cols = false) {
  if (remake::audit_sink()) remake::audit(x, y, w, h, tag, 'B', cols, true);
}

void head(Ctx& cx, const ui::Style& st, const Grid& g, std::string_view title, std::vector<Op>& ops) {
  ops.push_back(Op::rect(0, 0, g.w, g.h, st.bg));
  const size_t stack_from = ops.size();
  status_bar(cx.th, cx.assets, g.w, g.h, cx.status, ops);
  const ui::Metrics mt = ui::Metrics::from(cx.th, g.w, g.h);
  const float x = mt.title_left;
  const float tmax = ui::title_max_w(st, mt, ops, stack_from, g.w * 0.55f);
  int tpx = mt.title_px;
  ImgId id = cx.assets.text_ellipsis(title, float(tpx), st.txt, tmax);
  while (tpx > 8 && float(cx.assets.img(id).h) > float(g.bar_h())) id = cx.assets.text_ellipsis(title, float(--tpx), st.txt, tmax);
  const float tw = float(cx.assets.img(id).w), th = float(cx.assets.img(id).h);
  ops.push_back(Op::image(id, x, std::round(mt.header_cy - th / 2.0f), tw, th));
  ui::header_box() = {true, x, x + tw, mt.header_cy - th / 2.0f, mt.header_cy + th / 2.0f, float(g.content_top())};
}

// One look in every theme: a fixed grey-blue palette that only follows light / dark. With high contrast the theme's own colours stay (they are the
// high-contrast ones).
ui::Style look(const Theme& th) {
  ui::Style st = ui::Style::from(th);
  if (ui::access().contrast) return st;
  if (st.dark) {
    st.bg = {0x14, 0x17, 0x1c, 255}, st.panel = {0x1f, 0x23, 0x2a, 255}, st.txt = {0xe8, 0xea, 0xee, 255}, st.dim = {0x8b, 0x93, 0xa1, 255};
    st.cyan = {0x38, 0xbd, 0xf8, 255}, st.cyan_dark = {0x7d, 0xd3, 0xfc, 255}, st.cyan_soft = {0x38, 0xbd, 0xf8, 41};
    st.scrim = {0, 0, 0, 130};
  } else {
    st.bg = {0xf3, 0xf4, 0xf6, 255}, st.panel = {255, 255, 255, 255}, st.txt = {0x1b, 0x1f, 0x27, 255}, st.dim = {0x6b, 0x72, 0x80, 255};
    st.cyan = {0x02, 0x84, 0xc7, 255}, st.cyan_dark = {0x03, 0x69, 0xa1, 255}, st.cyan_soft = {0x02, 0x84, 0xc7, 31};
    st.scrim = {0, 0, 0, 90};
  }
  return st;
}

// ---- names ---------------------------------------------------------------------------------------------------------------------------------

std::string join(const std::string& d, const std::string& n) { return d == "/" ? "/" + n : d + "/" + n; }

std::string ext_of(const std::string& n) {
  const size_t k = n.rfind('.');
  if (k == std::string::npos || k == 0) return {};
  std::string e = n.substr(k + 1);
  for (char& c : e)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return e;
}

ui::Icon icon_of(const fo::Entry& e, bool up) {
  if (up) return ui::Icon::Back;
  if (e.dir) return ui::Icon::Folder;
  static const std::set<std::string> kImg = {"png", "jpg", "jpeg", "gif", "webp", "bmp", "svg"};
  static const std::set<std::string> kArc = {"zip", "7z", "rar", "tar", "gz", "xz", "bz2", "zst"};
  static const std::set<std::string> kRom = {"sfc", "smc", "nes", "gb", "gbc", "gba", "n64", "z64", "v64", "iso", "bin", "cue", "chd", "gen", "md", "smd", "pce", "sms", "gg",
                                              "a26", "nds", "3ds", "psp", "cso", "pbp", "wad", "ngp", "ws", "wsc", "32x", "lnx", "col", "fds", "vb"};
  const std::string x = ext_of(e.name);
  if (kImg.count(x)) return ui::Icon::Image;
  if (kArc.count(x)) return ui::Icon::Archive;
  if (kRom.count(x)) return ui::Icon::Game;
  return ui::Icon::File;
}

std::string err_text(fo::Err e) {
  switch (e) {
    case fo::Err::Protected: return tr("That folder is protected");
    case fo::Err::Outside: return tr("Outside the allowed folders");
    case fo::Err::NoSpace: return tr("Not enough free space");
    case fo::Err::Exists: return tr("That name already exists");
    case fo::Err::Cancelled: return tr("Cancelled");
    case fo::Err::Inside: return tr("A folder cannot go into itself");
    case fo::Err::BadName: return tr("That name is not valid");
    case fo::Err::NotFound: return tr("Not found");
    case fo::Err::TooBig: return tr("File larger than 4 GB: this card cannot hold it");
    case fo::Err::FsName: return tr("This card does not accept that name");
    case fo::Err::ReadOnly: return tr("This location is read-only");
    default: return tr("An error occurred");
  }
}

std::string base_name(const std::string& p) {
  const size_t k = p.rfind('/');
  return k == std::string::npos ? p : p.substr(k + 1);
}

// What went wrong, which item it was and, when several were asked for, how many were done: "\"a.bin\": Not enough free space (3 of 7 done)".
std::string fail_text(const fo::Result& r) {
  std::string t = err_text(r.err);
  const std::string nm = base_name(r.first);
  if (!nm.empty() && r.err != fo::Err::Cancelled) t = "\"" + nm + "\": " + t;
  if (r.total > 1) t += " (" + collections::fmt(tr("%s of %s done"), std::to_string(r.done), std::to_string(r.total)) + ")";
  return t;
}

std::string items_text(int n) { return std::to_string(n) + " " + (n == 1 ? tr("item") : tr("items")); }

std::string skipped_text(int n) { return std::to_string(n) + " " + (n == 1 ? tr("link skipped") : tr("links skipped")); }

// What the question about deleting names: the one item that will go (not the row under the cursor, which may be another), or how many.
std::string delete_text(const std::vector<std::string>& t) {
  return (t.size() == 1 ? "\"" + base_name(t[0]) + "\"" : items_text(int(t.size()))) + "\n" + tr("This can't be undone.");
}

// ---- the screen ----------------------------------------------------------------------------------------------------------------------------

class FilesScreen : public Screen {
 public:
  FilesScreen(Ctx& cx, const std::string& arg) {
    const std::string so = cx.conf.get_or("zfront.files.sort", "name");  // what was chosen last time
    sort_ = so == "size" ? fo::Sort::Size : so == "date" ? fo::Sort::Date : fo::Sort::Name;
    hidden_ = cx.conf.get_bool("zfront.files.hidden", false);
    const char* env = std::getenv("ZF_FILES_ROOT");
    if (env && *env) {
      pol_.roots = {env};  // the same shape as the console's, so the tests meet the same rules
      pol_.keep = {std::string(env) + "/roms"};
      pol_.keep_sub = {std::string(env) + "/roms"};
      pol_.warn = {std::string(env) + "/.config"};
    } else {
      pol_.roots = {"/storage", "/media"};
      pol_.keep = {"/storage/roms", "/storage/games-external", "/storage/games-internal", "/storage/games-external/roms", "/storage/games-internal/roms",
                   "/storage/.config", "/storage/.update"};
      // the folder of each system stays; what is inside it may go. /storage/roms is the card (or the overlay of both), reached by any of these names
      pol_.keep_sub = {"/storage/roms", "/storage/games-external/roms", "/storage/games-internal/roms"};
      pol_.warn = {"/storage/.config", "/storage/.update"};              // system files: a second question
    }
    std::string start = arg;
    if (start.empty() || !fo::inside_real(pol_, start)) {
      start = pol_.roots[0];
      for (const char* c : {"/storage/roms"})
        if (!env && fo::inside_real(pol_, c)) start = c;
    }
    cwd_ = fo::normalize(start);
    seen_gen_ = g_gen;
    refill("");
  }
  /// Test hook (test_layout): shows the progress card with a job that is a third of the way, so its layout can be measured. Starts nothing.
  void fake_job_for_test(bool calculating) {
    job_ = std::make_shared<Job>();
    job_->measuring = calculating;
    job_->pr.files_total = 12, job_->pr.files_done = 4, job_->pr.bytes_total = 3000, job_->pr.bytes_done = 1000;
    if (!calculating) job_->pr.set_current("A file whose name is far too long to fit in any row of any screen at all.sfc");
  }
  /// Test seam (`files_debug_for_test`).
  std::string debug(Ctx& cx, const std::string& what) {
    if (what == "confirm") return g_last_confirm;
    if (what == "state") return unreadable_ ? "unreadable" : (es_.empty() || (es_.size() == 1 && is_up(0))) ? "empty" : "ok";
    if (what == "sort") return sort_ == fo::Sort::Name ? "name" : sort_ == fo::Sort::Size ? "size" : "date";
    if (what == "hidden") return hidden_ ? "1" : "0";
    if (what == "clip") return std::to_string(g_clip.paths.size());
    if (what == "cursor") return cursor_path();
    if (what == "props") return properties_text();
    if (what == "refill") refill("");
    if (what == "busy:1") g_busy = true;
    if (what == "busy:0") g_busy = false;
    (void)cx;
    return {};
  }

  bool is_menu() const override { return true; }
  std::optional<float> wake_in() const override { return running() ? std::optional<float>(0.25f) : std::nullopt; }
  bool take_redraw() override {
    const bool r = redraw_;
    redraw_ = false;
    return r;
  }

  bool tick(float dt, Ctx&) override {
    if (job_ && job_->finished) job_.reset();
    if (seen_gen_ != g_gen) {
      seen_gen_ = g_gen;
      refill("");
      redraw_ = true;
    }
    if (running()) {
      acc_ += dt;
      if (acc_ >= 0.2f) acc_ = 0, redraw_ = true;
    }
    return false;
  }

  Transition take_transition(Ctx& cx) override {
    if (pending_.empty()) return Transition::none();
    const std::string p = std::move(pending_);
    pending_.clear();
    return act(cx, p);
  }

  Transition handle(Action a, Ctx& cx) override {
    if (running()) {  // a job: only B (cancel) is heard
      if (a == Action::B) job_->pr.cancel = true;
      return Transition::none();
    }
    const int n = int(es_.size());
    switch (a) {
      case Action::Up:
        if (n) cur_ = (cur_ + n - 1) % n;
        break;
      case Action::Down:
        if (n) cur_ = (cur_ + 1) % n;
        break;
      case Action::L: cur_ = std::max(0, cur_ - std::max(1, rows_)); break;
      case Action::R: cur_ = std::min(std::max(0, n - 1), cur_ + std::max(1, rows_)); break;
      case Action::Left:
        if (!at_top()) go_up();
        break;
      case Action::Right:
      case Action::A: return open_cursor(cx);
      case Action::B:
        if (!marks_.empty()) marks_.clear();
        else if (!at_top()) go_up();
        else return Transition::pop();
        break;
      case Action::Y:
        if (n && !is_up(cur_)) {
          const std::string& nm = es_[size_t(cur_)].name;
          if (!marks_.erase(nm)) marks_.insert(nm);
          if (cur_ + 1 < n) ++cur_;
        }
        break;
      case Action::X: return actions_menu(cx);
      case Action::Select: return jump_menu();
      case Action::Start: return Transition::close_menus();
      default: break;
    }
    return Transition::none();
  }

  void draw(Ctx& cx, std::vector<Op>& ops) override {
    const Grid g(cx.w, cx.h);
    const ui::Style st = look(cx.th);
    Assets& a = cx.assets;
    std::vector<remake::Box>*& sink = remake::audit_sink();
    std::vector<remake::Box>* const saved_sink = sink;
    if (running()) sink = nullptr;  // what the card covers is not measured (the card is)
    const float ts = std::max(1.0f, ui::access().scale());
    const int fb = int(std::lround(float(g.F(16, 13)) * ts)), fs = int(std::lround(float(g.F(13, 11)) * ts));
    head(cx, st, g, marks_.empty() ? std::string(tr("Files")) : std::to_string(marks_.size()) + " " + tr("selected"), ops);
    const float m = g.col(0), R = m + g.span(12), W12 = g.span(12);
    const int u = g.u;
    const float ct = float(g.content_top());
    // ---- the path
    const int ph = u * std::max(4, int(std::ceil(float(fs) * 1.7f / float(u))));
    g_sh.pill(a, ops, m, ct, W12, float(ph), st.dark ? Color{255, 255, 255, 14} : Color{255, 255, 255, 255});
    box(m, ct, W12, float(ph), "tarjeta", true);
    label(a, ops, path_text(a, fs, W12 - float(g.U(4))), m + float(g.U(2)), ct + float(ph) / 2.0f, fs, st.dim, W12 - float(g.U(4)));
    // ---- where the list ends: the help row (taller with a big text size), then the free space strip above it
    float help_top = float(g.help_top());
    if (ui::access().scale() > 1.0f) help_top = std::min(help_top, ui::help_pill_top(a, cx.th, ui::Metrics::from(cx.th, g.w, g.h)) - float(g.U(2)));
    const float strip = help_top - float(g.U(3)), y0 = ct + float(ph) + float(g.U(2));
    const int rc = std::max(g.snap(std::max(36.0f * g.s, float(fb) + 12.0f * g.s), 5) / u, 5);
    const float pitch = float(g.U(rc)), rh = pitch - float(g.U(1));
    rows_ = std::max(1, int((strip - float(u) - y0) / pitch));
    const int n = int(es_.size());
    if (cur_ < top_) top_ = cur_;
    if (cur_ >= top_ + rows_) top_ = cur_ - rows_ + 1;
    top_ = std::clamp(top_, 0, std::max(0, n - rows_));
    int icc = std::max(3, int(std::lround(18.0f * g.s / float(u))));
    if ((rc - 1 - icc) % 2 != 0) icc = icc + 1 <= rc - 2 ? icc + 1 : icc - 1;
    const int ic = g.U(icc);
    for (int i = top_; i < std::min(n, top_ + rows_); ++i) {
      const fo::Entry& e = es_[size_t(i)];
      const bool up = is_up(i), on = i == cur_, marked = !up && marks_.count(e.name) > 0;
      const float y = y0 + float(i - top_) * pitch, cy = y + rh / 2.0f;
      if (on) {
        g_sh.rrect(a, ops, m, y, W12, rh, g.U(2), st.cyan_soft);
        box(m, y, W12, rh, "tarjeta", true);
      }
      const Color col = on ? st.cyan_dark : st.txt, dcol = on ? st.cyan_dark : st.dim;
      const float ix = m + float(g.U(2)), iy = y + float(g.U((rc - 1 - icc) / 2));
      if (!marks_.empty() && !up) {  // selecting: a box instead of the icon
        const Color ac = st.dark ? st.cyan : st.cyan_dark;
        if (marked) {
          g_sh.rrect(a, ops, ix, iy, float(ic), float(ic), std::max(2, ic / 4), ac);
          g_sh.icon(a, ops, ui::Icon::Check, ix, iy, ic, st.dark ? st.bg : Color{255, 255, 255, 255});
        } else {
          g_sh.rrect(a, ops, ix, iy, float(ic), float(ic), std::max(2, ic / 4), st.dim);
          g_sh.rrect(a, ops, ix + 2, iy + 2, float(ic - 4), float(ic - 4), std::max(1, ic / 4 - 1), st.bg);
        }
        box(ix, iy, float(ic), float(ic), "pastilla");
      } else {
        g_sh.icon(a, ops, icon_of(e, up), ix, iy, ic, e.link ? ui::with_alpha(dcol, 0.7f) : dcol);
        if (remake::audit_sink()) remake::audit(ix, iy, float(ic), float(ic), "icono", 'B', false, false);
      }
      const float tx = ix + float(ic) + float(g.U(2)), right = R - float(g.U(2));
      std::string r;
      if (e.dir && !up) {
        const int c = count_of(i);
        if (c >= 0) r = items_text(c);
      } else if (!up) {
        r = fo::size_text(e.size);
      }
      float rw = 0;
      if (!r.empty()) {
        rw = a.measure(r, float(fs));
        label(a, ops, r, right, cy, fs, dcol, 0, true);
      }
      label(a, ops, up ? std::string("..") : e.name, tx, cy, fb, col, right - rw - float(g.U(2)) - tx);
    }
    if (es_.empty() || (n == 1 && is_up(0)))
      label(a, ops, unreadable_ ? tr("Cannot read this folder") : tr("This folder is empty"), m + float(g.U(2)), y0 + (n ? pitch * 1.5f : pitch / 2.0f), fb, st.dim, W12 - float(g.U(4)));
    // ---- free space
    const float scy = strip + float(g.U(3)) / 2.0f;
    label(a, ops, fo::size_text(free_) + " " + tr("free"), m, scy, fs, st.dim, g.span(3));
    const float bx = g.col(3), bw = g.span(6), bh = float(u), by = strip + float(u);
    if (total_ > 0) {
      g_sh.pill(a, ops, bx, by, bw, bh, ui::with_alpha(st.dim, 0.25f));
      const float used = 1.0f - float(free_) / float(total_);
      g_sh.pill(a, ops, bx, by, std::max(bh, bw * used), bh, st.dark ? st.cyan : st.cyan_dark);
      box(bx, by, bw, bh, "pastilla", true);
    }
    label(a, ops, fo::size_text(total_), R, scy, fs, st.dim, g.span(3), true);
    // ---- the job
    sink = saved_sink;
    if (running()) progress_card(cx, st, g, fb, fs, ops);
    help_bar(cx.th, a, g.w, g.h, help_prompts(running()), ops);
  }

 private:
  bool running() const { return job_ && !job_->finished; }
  bool at_top() const {
    for (const std::string& r : pol_.roots)
      if (fo::normalize(r) == cwd_) return true;
    return false;
  }
  bool is_up(int i) const { return i == 0 && !at_top() && !es_.empty() && es_[0].name == ".."; }

  std::string path_text(Assets& a, int px, float max_w) const {
    std::string p = cwd_;
    std::string pre;
    while (a.measure(pre + p, float(px)) > max_w) {
      const size_t k = p.find('/', 1);
      if (k == std::string::npos) break;
      p = p.substr(k);
      pre = "...";
    }
    return pre + p;
  }

  int count_of(int i) {
    auto it = counts_.find(es_[size_t(i)].name);
    if (it != counts_.end()) return it->second;
    const int c = fo::count_entries(join(cwd_, es_[size_t(i)].name), hidden_);
    counts_[es_[size_t(i)].name] = c;
    return c;
  }

  void refill(const std::string& select) {
    unreadable_ = !fo::list(cwd_, sort_, hidden_, es_);  // a folder that cannot be read (drive pulled out, no permission) is not an empty one
    if (!at_top()) {
      fo::Entry up;
      up.name = "..";
      up.dir = true;
      es_.insert(es_.begin(), up);
    }
    counts_.clear();
    std::set<std::string> have;
    for (const fo::Entry& e : es_) have.insert(e.name);
    for (auto it = marks_.begin(); it != marks_.end();) it = have.count(*it) ? std::next(it) : marks_.erase(it);
    uint64_t f = 0, t = 0;
    free_ = total_ = 0;
    if (fo::space(cwd_, f, t)) free_ = f, total_ = t;
    cur_ = std::min(cur_, std::max(0, int(es_.size()) - 1));
    if (!select.empty())
      for (size_t i = 0; i < es_.size(); ++i)
        if (es_[i].name == select) cur_ = int(i);
  }

  void go_to(Ctx& cx, const std::string& path, const std::string& select = "") {
    if (!fo::inside_real(pol_, path)) {
      cx.sv->toasts.push(err_text(fo::Err::Outside), 3000);
      return;
    }
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return;
    cwd_ = fo::normalize(path);
    marks_.clear();
    cur_ = top_ = 0;
    refill(select);
  }
  void go_up() {
    const size_t k = cwd_.rfind('/');
    const std::string came = cwd_.substr(k + 1), parent = k == 0 ? "/" : cwd_.substr(0, k);
    cwd_ = parent;
    marks_.clear();
    cur_ = top_ = 0;
    refill(came);
  }

  std::string cursor_path() const { return es_.empty() || is_up(cur_) ? std::string() : join(cwd_, es_[size_t(cur_)].name); }

  std::vector<std::string> targets() const {
    std::vector<std::string> t;
    if (!marks_.empty()) {
      for (const fo::Entry& e : es_)
        if (marks_.count(e.name)) t.push_back(join(cwd_, e.name));
    } else if (!cursor_path().empty()) {
      t.push_back(cursor_path());
    }
    return t;
  }

  Transition open_cursor(Ctx& cx) {
    if (es_.empty()) return Transition::none();
    if (is_up(cur_)) {
      go_up();
      return Transition::none();
    }
    const fo::Entry& e = es_[size_t(cur_)];
    const std::string p = join(cwd_, e.name);
    struct stat st;
    if (::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      go_to(cx, p);
      return Transition::none();
    }
    return properties();
  }

  // The text of "Properties" for the row under the cursor. The listing leaves the date of folders and links out (it only asks for it when sorting by
  // date), so it is read here, once, when the user asks.
  std::string properties_text() const {
    const std::string p = cursor_path();
    if (p.empty()) return {};
    const fo::Entry& e = es_[size_t(cur_)];
    char when[40] = "";
    std::time_t t = e.mtime;
    if (t == 0) {
      struct stat ls;
      if (::lstat(p.c_str(), &ls) == 0) t = ls.st_mtime;
    }
    std::tm tmv{};
    ::localtime_r(&t, &tmv);
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tmv);
    std::string text = e.name + "\n\n";
    text += std::string(e.dir ? tr("Folder") : e.link ? tr("Link") : tr("File")) + "\n";
    if (!e.dir) text += fo::size_text(e.size) + "\n";
    else {
      const int c = fo::count_entries(p, hidden_);
      if (c >= 0) text += items_text(c) + "\n";
    }
    text += std::string(when) + "\n" + p;
    return text;
  }
  Transition properties() const {
    const std::string text = properties_text();
    if (text.empty()) return Transition::none();
    return ui::message(text, nullptr, tr("Properties"));
  }

  Transition actions_menu(Ctx&) {
    std::vector<Option> o;
    const bool have = !targets().empty();
    if (have) {
      o.push_back({"copy", tr("Copy")});
      o.push_back({"move", tr("Move")});
    }
    if (!g_clip.paths.empty()) o.push_back({"paste", tr("Paste here")});
    if (marks_.empty() && !cursor_path().empty()) o.push_back({"rename", tr("Rename")});
    o.push_back({"newdir", tr("New folder")});
    if (have) o.push_back({"delete", tr("Delete")});
    if (marks_.empty() && !cursor_path().empty()) o.push_back({"props", tr("Properties")});
    if (!es_.empty()) o.push_back({"selall", tr("Select all")});
    if (!marks_.empty()) o.push_back({"selnone", tr("Clear selection")});
    o.push_back({"sort", std::string(tr("Sort by")) + ": " + (sort_ == fo::Sort::Name ? tr("Name") : sort_ == fo::Sort::Size ? tr("Size") : tr("Date"))});
    o.push_back({"hidden", hidden_ ? tr("Hide hidden files") : tr("Show hidden files")});
    return ui::choose(tr("Actions"), std::move(o), "", [this](Ctx&, const std::string& v) { pending_ = v; });
  }

  Transition jump_menu() {
    std::vector<Option> o;
    auto add = [&](const std::string& label_text, const std::string& path) {
      struct stat st;
      if (fo::inside_real(pol_, path) && ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) o.push_back({"go:" + path, label_text});
    };
    if (std::getenv("ZF_FILES_ROOT")) {
      add(tr("Home"), pol_.roots[0]);
    } else {
      add(tr("Games"), "/storage/roms");
      add(tr("Game card"), "/storage/games-external");
      std::vector<fo::Entry> usb;
      if (fo::list("/media", fo::Sort::Name, false, usb))
        for (const fo::Entry& e : usb)
          if (e.dir) add(std::string(tr("USB drive")) + " " + e.name, "/media/" + e.name);
      add(tr("Screenshots"), "/storage/roms/screenshots");
      add(tr("Home"), "/storage");
    }
    if (o.empty()) return Transition::none();
    return ui::choose(tr("Go to"), std::move(o), "", [this](Ctx&, const std::string& v) { pending_ = v; });
  }

  Transition act(Ctx& cx, const std::string& v) {
    if (v.compare(0, 3, "go:") == 0) {
      go_to(cx, v.substr(3));
    } else if (v == "copy" || v == "move") {
      g_clip.paths = targets();
      g_clip.move = v == "move";
      marks_.clear();
      cx.sv->toasts.push(std::string(g_clip.move ? tr("Ready to move") : tr("Ready to copy")) + ": " + items_text(int(g_clip.paths.size())), 2500);
    } else if (v == "paste") {
      if (!g_clip.paths.empty()) {
        if (start_job(cx, g_clip.move ? 1 : 0, g_clip.paths, cwd_) && g_clip.move) g_clip.paths.clear();  // another job running: what was cut stays on the clipboard
      }
    } else if (v == "rename") {
      const std::string p = cursor_path();
      if (p.empty()) return Transition::none();
      const std::string cur_name = es_[size_t(cur_)].name;
      auto ask = [this, p, cur_name]() {
        return ui::keyboard({tr("Rename"), false, 255}, cur_name, [this, p](Ctx& c, const std::string& nm) {
          const fo::Result r = fo::rename_item(pol_, p, nm);
          if (r.err != fo::Err::None) c.sv->toasts.push(fail_text(r), 3500);
          refill(r.err == fo::Err::None ? nm : "");
        });
      };
      if (!fo::needs_warning(pol_, p)) return ask();
      ui::ConfirmOpts wo;  // a system file: asked before the name is
      wo.title = tr("Warning");
      wo.yes = tr("Rename");
      wo.no = tr("Cancel");
      wo.danger = true;
      g_last_confirm = "\"" + cur_name + "\"\n" + tr("This is a system file. Renaming it can break the console.");
      return ui::confirm(g_last_confirm, [ask](Ctx&) { return ask(); }, wo);
    } else if (v == "newdir") {
      return ui::keyboard({tr("New folder"), false, 255}, "", [this](Ctx& c, const std::string& nm) {
        const fo::Result r = fo::make_dir(pol_, cwd_, nm);
        if (r.err != fo::Err::None) c.sv->toasts.push(fail_text(r), 3500);
        refill(r.err == fo::Err::None ? nm : "");
      });
    } else if (v == "delete") {
      const std::vector<std::string> t = targets();
      if (t.empty()) return Transition::none();
      ui::ConfirmOpts o;
      o.title = tr("Delete");
      o.yes = tr("Delete");
      o.no = tr("Cancel");
      o.danger = true;
      const std::string msg = delete_text(t);
      g_last_confirm = msg;
      auto go = [this, t](Ctx& c) {
        marks_.clear();
        start_job(c, 2, t, "");
        return Transition::none();
      };
      bool warn = false;
      for (const std::string& p : t) warn = warn || fo::needs_warning(pol_, p);
      if (!warn) return ui::confirm(msg, go, o);
      ui::ConfirmOpts o2 = o;  // inside /storage/.config or /storage/.update: a second question, with a plain warning
      o2.title = tr("Warning");
      return ui::confirm(msg, [go, o2](Ctx&) {
        g_last_confirm = tr("This is a system file. Deleting it can break the console.");
        return ui::confirm(g_last_confirm, go, o2);
      }, o);
    } else if (v == "props") {
      return properties();
    } else if (v == "selall") {
      for (const fo::Entry& e : es_)
        if (e.name != "..") marks_.insert(e.name);
    } else if (v == "selnone") {
      marks_.clear();
    } else if (v == "sort") {
      sort_ = sort_ == fo::Sort::Name ? fo::Sort::Size : sort_ == fo::Sort::Size ? fo::Sort::Date : fo::Sort::Name;
      cx.conf.persist("zfront.files.sort", sort_ == fo::Sort::Size ? "size" : sort_ == fo::Sort::Date ? "date" : "name");
      refill(cursor_path().empty() ? "" : es_[size_t(cur_)].name);
    } else if (v == "hidden") {
      hidden_ = !hidden_;
      cx.conf.persist("zfront.files.hidden", hidden_ ? "1" : "0");
      refill(cursor_path().empty() ? "" : es_[size_t(cur_)].name);
    }
    return Transition::none();
  }

  bool start_job(Ctx& cx, int kind, std::vector<std::string> srcs, std::string dst) {
    if (g_busy.exchange(true)) {
      cx.sv->toasts.push(tr("Wait for the other job"), 2500);
      return false;
    }
    auto job = std::make_shared<Job>();
    job->kind = kind;
    job_ = job;
    const fo::Policy pol = pol_;
    msys::post(cx,
         [job, pol, srcs, dst, kind] {
           fo::measure(srcs, &job->pr);
           job->measuring = false;
           job->res = kind == 2 ? fo::remove(pol, srcs, job->pr) : fo::transfer(pol, srcs, dst, kind == 1, job->pr);
         },
         [&cx, job] {
           g_busy = false;
           job->finished = true;
           ++g_gen;
           const fo::Result& r = job->res;
           if (r.err == fo::Err::None) {
             const char* verb = job->kind == 0 ? tr("Copied") : job->kind == 1 ? tr("Moved") : tr("Deleted");
             std::string t = std::string(verb) + ": " + items_text(r.done);
             if (r.skipped > 0) t += ", " + skipped_text(r.skipped) + (r.kept ? " (" + std::string(tr("originals kept")) + ")" : std::string());
             cx.sv->toasts.push(t, r.skipped > 0 ? 5500 : 3000);
           } else {
             cx.sv->toasts.push(fail_text(r), 4500);
           }
         },
         true);
    redraw_ = true;
    return true;
  }

  void progress_card(Ctx& cx, const ui::Style& st, const Grid& g, int fb, int fs, std::vector<Op>& ops) {
    Assets& a = cx.assets;
    const int u = g.u;
    ops.push_back(Op::rect(0, 0, g.w, g.h, st.scrim));
    const int lh = std::max(u, int(std::ceil(float(std::max(fb, fs)) * 1.45f / float(u))) * u);  // a line of text, in whole cells
    const int hc = int(std::lround(float(5 * u + 3 * lh) / float(u)));                           // pad, three lines (title, count, file), gap, bar, pad
    const float x = g.col(2), w = g.span(8), h = float(g.U(hc)), y = float(g.row(std::max(0, int((g.h / 2.0f - h / 2.0f - float(g.top())) / float(u)))));
    g_sh.rrect(a, ops, x, y, w, h, g.U(2), st.panel);
    box(x, y, w, h, "tarjeta", true);
    const float tx = x + float(g.U(2)), tw = w - float(g.U(4));
    const Job& j = *job_;
    const char* title = j.kind == 0 ? tr("Copying") : j.kind == 1 ? tr("Moving") : tr("Deleting");
    label(a, ops, title, tx, y + float(g.U(2)) + float(lh) * 0.5f, fb, st.txt, tw);
    const int fd = j.pr.files_done, ft = std::max(j.pr.files_total.load(), fd);
    const float sub_cy = y + float(g.U(2)) + float(lh) * 1.5f;
    float frac = 0;
    if (j.measuring) {  // the tree is still being counted: no "0 / 0, 0 %"
      label(a, ops, tr("Calculating..."), tx, sub_cy, fs, st.dim, tw);
    } else {
      label(a, ops, std::to_string(fd) + " / " + std::to_string(ft), tx, sub_cy, fs, st.dim, tw * 0.5f);
      const uint64_t bt = j.pr.bytes_total, bd = j.pr.bytes_done;
      frac = j.kind == 2 || bt == 0 ? (ft ? float(fd) / float(ft) : 0.0f) : std::min(1.0f, float(bd) / float(bt));
      char pc[16];
      std::snprintf(pc, sizeof pc, "%d %%", int(std::lround(frac * 100.0f)));
      label(a, ops, pc, x + w - float(g.U(2)), sub_cy, fs, st.dim, 0, true);
      const std::string now = j.pr.current();  // the file in hand, cut with "..." to the width of the card
      if (!now.empty()) label(a, ops, now, tx, y + float(g.U(2)) + float(lh) * 2.5f, fs, st.dim, tw);
    }
    const float bh = float(g.U(1)), by = y + float(g.U(2)) + float(3 * lh) + float(u);
    g_sh.pill(a, ops, tx, by, tw, bh, ui::with_alpha(st.dim, 0.25f));
    g_sh.pill(a, ops, tx, by, std::max(bh, tw * frac), bh, st.dark ? st.cyan : st.cyan_dark);
    box(tx, by, tw, bh, "pastilla");
  }

  // The most important first: the help bar drops the last ones that do not fit (at 480x320 the page and close prompts go).
  static const HelpPrompts& help_prompts(bool busy) {
    static HelpPrompts p[2];
    static std::deque<std::string> text;  // pointers into a deque stay valid while it grows
    static unsigned gen = ~0u;
    if (gen != i18n::generation()) {
      gen = i18n::generation();
      text.clear();
      auto t = [](const char* k) {
        text.push_back(tr(k));
        return text.back().c_str();
      };
      p[1] = {{"B", t(N_("Cancel"))}};
      p[0] = {{"A", t(N_("Open"))}, {"B", t(N_("Back"))}, {"X", t(N_("Actions"))}, {"Y", t(N_("Mark"))}, {"select", t(N_("Go to"))}, {"R", t(N_("Page"))}, {"start", t(N_("Close"))}};
    }
    return p[busy ? 1 : 0];
  }

  fo::Policy pol_;
  std::string cwd_, pending_;
  std::vector<fo::Entry> es_;
  std::set<std::string> marks_;
  std::map<std::string, int> counts_;
  fo::Sort sort_ = fo::Sort::Name;
  bool hidden_ = false, redraw_ = false, unreadable_ = false;
  int cur_ = 0, top_ = 0, rows_ = 8, seen_gen_ = 0;
  float acc_ = 0;
  uint64_t free_ = 0, total_ = 0;
  std::shared_ptr<Job> job_;
};

}  // namespace

bool is_file_manager_script(std::string_view path) {
  const size_t k = path.rfind('/');
  const std::string_view name = k == std::string_view::npos ? path : path.substr(k + 1);
  return name == "fileman.sh" || name == "commander.sh";
}

std::string files_debug_for_test(Screen& s, Ctx& cx, const std::string& what) { return static_cast<FilesScreen&>(s).debug(cx, what); }

ZF_REGISTER_SCREEN("menu.files", [](Ctx& cx, const std::string& arg) -> std::unique_ptr<Screen> {
  if (arg == "@progress-test" || arg == "@progress-calc") {
    auto s = std::make_unique<FilesScreen>(cx, "");
    s->fake_job_for_test(arg == "@progress-calc");
    return s;
  }
  return std::make_unique<FilesScreen>(cx, arg);
});

}  // namespace zf
