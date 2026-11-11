#include "manual.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "covers.hpp"
#include "gamelist.hpp"
#include "gamelist_game.hpp"
#include "menu_game.hpp"
#include "registry.hpp"
#include "ui_style.hpp"
#include "util.hpp"

namespace zf::manual {

// ---- plain functions (ES ApiSystem) -----------------------------------------------------------------------------------------------

int parse_page_count(const std::string& out) {
  size_t i = 0;
  while (i <= out.size()) {
    size_t e = out.find('\n', i);
    if (e == std::string::npos) e = out.size();
    std::string_view line(out.data() + i, e - i);
    size_t c = line.find(':');
    if (c != std::string_view::npos && trim(line.substr(0, c)) == "Pages") return std::atoi(std::string(trim(line.substr(c + 1))).c_str());  // ES: key "Pages", atoi of the value
    i = e + 1;
  }
  return 0;
}

std::string pdfinfo_command(const std::string& pdf) { return "pdfinfo " + shell_quote(pdf); }

bool small_screen(unsigned w, unsigned h, const std::string* force) {
  if (force && !force->empty()) return *force == "true";  // ES ScreenSettings::isSmallScreen: ForceSmallScreen wins
  return w <= 480 || h <= 480;
}

int page_dpi(bool small) { return small ? 96 : 125; }  // ES extractPdfImages: Renderer::isSmallScreen() ? "96" : "125"

std::string page_prefix(const std::string& pdf, int dpi, int page) {
  char b[16];
  std::snprintf(b, sizeof b, "%08d", page);
  return path_filename(pdf) + "-" + std::to_string(dpi) + "-" + b + "-pdf";
}

std::string extract_command(const std::string& pdf, int dpi, int page, const std::string& dir, const std::string& prefix) {
  const std::string p = std::to_string(page);
  return "pdftoppm -jpeg -r " + std::to_string(dpi) + " -cropbox -f " + p + " -l " + p + " " + shell_quote(pdf) + " " + shell_quote(dir + "/" + prefix);
}

std::string find_page_file(std::vector<std::string> names, const std::string& prefix) {
  std::sort(names.begin(), names.end());
  for (const std::string& n : names) {
    const std::string e = path_ext_lower(n);
    if (e != ".jpg" && e != ".png" && e != ".ppm") continue;
    if (starts_with(n, prefix)) return n;
  }
  return {};
}

std::string cache_dir() { return mg::env().rooted(mg::env().user_dir + "/pdftmp"); }

bool available() {
  const std::string d = mg::env().rooted(mg::env().bin_dir);
  return file_exists(d + "/pdftoppm") && file_exists(d + "/pdfinfo");
}

std::string media_file(const Ctx& cx, const std::string& system, const std::string& rom_path, Kind k) {
  for (const System& s : cx.systems) {
    if (s.name != system) continue;
    for (const Game& g : s.games) {
      if (g.path != rom_path) continue;
      const char* tag = k == Kind::Manual ? "manual" : "magazine";
      auto f = gamelist_read_fields(s.gamelist, s.rom_dir, g.path, g.meta, {tag});
      auto it = f.find(tag);
      if (it == f.end() || it->second.empty()) return {};
      std::string p = resolve_rel_path(it->second, s.rom_dir);  // ES: the metadata path, `./` relative to the ROM folder
      return !p.empty() && file_exists(p) ? p : std::string();
    }
  }
  return {};
}

void media_entries(MenuBuilder& m, Ctx& cx, const std::string& system, const std::string& rom_path) {
  if (!available()) return;
  const std::string man = media_file(cx, system, rom_path, Kind::Manual), mag = media_file(cx, system, rom_path, Kind::Magazine);
  if (man.empty() && mag.empty()) return;
  const std::string arg = mg::game_arg(system, rom_path);
  m.header(tr("GAME MEDIA"));
  if (!man.empty()) m.action(tr("VIEW GAME MANUAL"), [arg] { return Transition::open("menu.game.manual", arg); });
  if (!mag.empty()) m.action(tr("VIEW GAME MAGAZINE"), [arg] { return Transition::open("menu.game.magazine", arg); });
}

// ---- the viewer -------------------------------------------------------------------------------------------------------------------

namespace {

using namespace ui;

std::vector<std::string> list_names(const std::string& dir) {
  std::vector<std::string> v;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d)) {
      const std::string n = e->d_name;
      if (n != "." && n != "..") v.push_back(n);
    }
    ::closedir(d);
  }
  return v;
}

// ES empties the folder when the viewer closes (GuiImageViewer destructor); only the page pictures here
void remove_pages(const std::string& dir) {
  for (const std::string& n : list_names(dir)) {
    const std::string e = path_ext_lower(n);
    if (e == ".jpg" || e == ".png" || e == ".ppm") ::unlink((dir + "/" + n).c_str());
  }
}

const Prompt kHelpFit[] = {{"leftright", N_("PAGE")}, {"A", N_("ZOOM")}, {"L", N_("ZOOM OUT")}, {"R", N_("ZOOM IN")}, {"B", N_("CLOSE")}};
const Prompt kHelpZoomed[] = {{"dpad", N_("MOVE")}, {"L", N_("ZOOM OUT")}, {"R", N_("ZOOM IN")}, {"A", N_("ZOOM")}, {"B", N_("CLOSE")}};

constexpr float kMaxZoom = 5.0f, kZoomStep = 1.25f, kPanStep = 0.1f;

struct Page {
  enum St { None, Busy, Ready } st = None;  // Ready with an empty file = pdftoppm gave nothing
  std::string file;
};

class ManualScreen : public Screen {
 public:
  ManualScreen(Ctx& cx, std::string pdf) : pdf_(std::move(pdf)), dir_(cache_dir()), life_(std::make_shared<bool>(true)) {
    dpi_ = page_dpi(small_screen(unsigned(cx.w), unsigned(cx.h), cx.es.get("ForceSmallScreen")));
    count(cx);
  }
  ~ManualScreen() override { *life_ = false; }

  Transition handle(Action a, Ctx& cx) override {
    switch (a) {
      case Action::B:
      case Action::Start:  // the viewer is no menu, so Start cannot close "the menus" through it: it leaves like B
        *life_ = false;  // a job that finishes later finds nobody
        cx.sv->jobs.post([dir = dir_] { remove_pages(dir); });  // FIFO: after any extraction still running
        return Transition::pop();
      case Action::L: zoom(zoom_ / kZoomStep); break;
      case Action::R: zoom(zoom_ * kZoomStep); break;
      case Action::A: zoom(zoomed() ? 1.0f : 2.0f); break;
      case Action::Left:
        if (!zoomed()) go(cx, cur_ - 1);
        else pan(kPanStep * W_, 0);  // the page moves right, the view left (ES: left lowers the origin)
        break;
      case Action::Right:
        if (!zoomed()) go(cx, cur_ + 1);
        else pan(-kPanStep * W_, 0);
        break;
      case Action::Up:
        if (zoomed()) pan(0, kPanStep * ph_);
        break;
      case Action::Down:
        if (zoomed()) pan(0, -kPanStep * ph_);
        break;
      default: break;
    }
    return Transition::none();
  }

  Transition take_transition(Ctx& cx) override {
    (void)cx;
    if (closed_ && !popped_) {  // no pages (ES: `delete this` when pdfinfo says 0)
      popped_ = true;
      return Transition::pop();
    }
    return Transition::none();
  }

  void draw(Ctx& cx, std::vector<Op>& out) override {
    layout(cx);
    Assets& a = cx.assets;
    out.push_back(Op::rect(0, 0, W_, H_, Color{0, 0, 0, 255}));
    ImgId id = 0;
    bool have = false;
    if (pages_ > 0 && cur_ >= 1 && cur_ <= pages_) {
      const Page& pg = pg_[size_t(cur_)];
      if (pg.st == Page::Ready && !pg.file.empty()) have = image_of(cx, pg.file, id);
      if (have) {  // the neighbours get decoded once this page is on screen, one at a time (a newer decode request replaces an older one)
        for (int d : {1, -1}) {
          const int p = cur_ + d;
          ImgId other;
          if (p < 1 || p > pages_ || pg_[size_t(p)].st != Page::Ready || pg_[size_t(p)].file.empty()) continue;
          if (!image_of(cx, pg_[size_t(p)].file, other)) break;
        }
      }
    }
    if (have) {
      const Img& im = a.img(id);
      const float iw = float(im.w), ih = float(im.h);
      const float fit = std::min(W_ / iw, ph_ / ih);  // the page fits its area at zoom 1
      dw_ = iw * fit * zoom_, dh_ = ih * fit * zoom_;
      clamp_pan();
      const float x = std::round((W_ - dw_) / 2.0f + ox_), y = std::round(top_ + (ph_ - dh_) / 2.0f + oy_);
      out.push_back(Op::clip(0, top_, W_, ph_));
      out.push_back(Op::image(id, x, y, std::round(dw_), std::round(dh_)));
      out.push_back(Op::unclip());
    } else if (!closed_) {
      // a page that is done but has no picture (pdftoppm gave nothing, or the file does not decode) says so instead of staying black
      const bool bad = pages_ > 0 && cur_ >= 1 && cur_ <= pages_ && pg_[size_t(cur_)].st == Page::Ready &&
                       (pg_[size_t(cur_)].file.empty() || covers::failed(pg_[size_t(cur_)].file));
      failed_ = bad;
      Txt& t = bad ? t_err_ : t_load_;
      if (!t.alive(a)) t.set(a, bad ? tr("Could not show this page") : tr("Loading..."), m_.em);
      put_text(out, t, std::round((W_ - t.w) / 2.0f), std::round(top_ + ph_ / 2.0f), Color{230, 230, 232, 255});  // on the black page
    }
    if (have) failed_ = false;
    if (pages_ > 0) draw_counter(a, out);
    if (zoomed()) help_.draw(a, cx.th, m_, st_, kHelpZoomed, 5, out);
    else help_.draw(a, cx.th, m_, st_, kHelpFit, 5, out);
  }

  std::optional<float> wake_in() const override { return std::nullopt; }

  bool zoomed() const { return zoom_ > 1.0001f; }
  ViewerState state() const {
    ViewerState s;
    s.pages = pages_, s.page = cur_, s.closed = closed_, s.zoom = zoom_, s.failed = failed_;
    s.counter = counter_;
    if (laid_) s.area = {0, top_, W_, ph_};
    return s;
  }

 private:
  // The decoded picture of a page file: at most twice the screen (a 125 dpi page is smaller, so it is its own size). Decoded on the job
  // thread; the harness and the tests (no async covers) decode inline.
  bool image_of(Ctx& cx, const std::string& path, ImgId& id) {
    const uint32_t bw = uint32_t(std::max(cx.w, 1.0f)) * 2u, bh = uint32_t(std::max(cx.h, 1.0f)) * 2u;
    if (cx.async_covers) return covers::request(cx.assets, cx.sv->jobs, path, bw, bh) && covers::ready(cx.assets, path, bw, bh, id);
    if (covers::failed(path)) return false;
    if (cx.assets.file_fit(path, bw, bh, id)) return true;
    covers::mark_failed(path);
    return false;
  }

  // `pdfinfo` on the job thread: the page count, or a viewer that closes (ES: no pages, no viewer)
  void count(Ctx& cx) {
    busy_ = true;
    auto out = std::make_shared<std::string>();
    auto sv = cx.sv;
    Ctx* c = &cx;  // `Ctx` owns the jobs: `done` never runs after it is gone
    auto life = life_;
    const std::string cmd = pdfinfo_command(pdf_);
    cx.sv->jobs.post([sv, out, cmd] { *out = sv->cmd.run(cmd, 30000).out; },
                     [this, c, life, out] {
                       if (!*life) return;
                       busy_ = false;
                       pages_ = parse_page_count(*out);
                       if (pages_ <= 0) {
                         closed_ = true;
                         return;
                       }
                       pg_.assign(size_t(pages_) + 1, Page());
                       cur_ = 1;
                       pump(*c);
                     });
  }

  // One extraction at a time on the shared job thread: the page on screen first, then the next one, then the previous one
  void pump(Ctx& cx) {
    if (busy_ || pages_ <= 0) return;
    for (int p : {cur_, cur_ + 1, cur_ - 1}) {
      if (p < 1 || p > pages_ || pg_[size_t(p)].st != Page::None) continue;
      extract(cx, p);
      return;
    }
  }

  void extract(Ctx& cx, int p) {
    pg_[size_t(p)].st = Page::Busy;
    busy_ = true;
    const std::string prefix = page_prefix(pdf_, dpi_, p), cmd = extract_command(pdf_, dpi_, p, dir_, prefix), dir = dir_;
    auto file = std::make_shared<std::string>();
    auto sv = cx.sv;
    Ctx* c = &cx;
    auto life = life_;
    cx.sv->jobs.post(
        [sv, cmd, dir, prefix, file] {
          ::mkdir(dir.c_str(), 0755);  // its parent is ES's user folder
          sv->cmd.run(cmd, 120000);
          *file = find_page_file(list_names(dir), prefix);
        },
        [this, c, life, p, file, dir] {
          if (!*life) return;
          busy_ = false;
          Page& pg = pg_[size_t(p)];
          pg.st = Page::Ready;
          if (!file->empty()) pg.file = dir + "/" + *file;
          pump(*c);
        });
  }

  void go(Ctx& cx, int p) {
    p = std::max(1, std::min(p, pages_));
    if (pages_ <= 0 || p == cur_) return;
    cur_ = p;
    zoom_ = 1.0f, ox_ = oy_ = 0;
    t_count_.reset();
    pump(cx);
  }
  void zoom(float z) {
    zoom_ = std::max(1.0f, std::min(z, kMaxZoom));
    if (!zoomed()) zoom_ = 1.0f, ox_ = oy_ = 0;
    clamp_pan();
  }
  void pan(float dx, float dy) {
    ox_ += dx, oy_ += dy;
    clamp_pan();
  }
  // the page can leave its area only as far as its edge reaches the area's edge
  void clamp_pan() {
    const float mx = std::max(0.0f, (dw_ - W_) / 2.0f), my = std::max(0.0f, (dh_ - ph_) / 2.0f);
    ox_ = std::max(-mx, std::min(ox_, mx));
    oy_ = std::max(-my, std::min(oy_, my));
  }

  void layout(Ctx& cx) {
    if (laid_ && W_ == cx.w && H_ == cx.h) return;
    laid_ = true;
    W_ = cx.w, H_ = cx.h;
    m_ = Metrics::from(cx.th, cx.w, cx.h);
    st_ = Style::from(cx.th);
    t_load_.reset(), t_err_.reset(), t_count_.reset();
    // the page area: under the title row (where the counter is) and above the help bar, as the content of the menus; the help was
    // drawn over the white page before and could not be read
    top_ = std::round(2.0f * m_.header_cy);
    float bottom = std::round(2.0f * m_.help_y - H_);
    if (bottom - top_ < 0.5f * H_) top_ = 0, bottom = H_;  // a theme without room for both: the whole screen, as before
    ph_ = bottom - top_;
  }

  // "3 / 12" in a small pill at the top right, on the margin of the menus
  void draw_counter(Assets& a, std::vector<Op>& out) {
    char b[32];
    std::snprintf(b, sizeof b, "%d / %d", cur_, pages_);
    if (!t_count_.alive(a) || count_text_ != b) {
      count_text_ = b;
      t_count_.set(a, count_text_, m_.small_px);
    }
    const float pad = std::round(0.5f * float(m_.em)), h = std::round(t_count_.h + 0.5f * float(m_.em)), w = std::round(t_count_.w + 2.0f * pad);
    const float x = std::round(m_.W - m_.margin - w), y = std::round(m_.header_cy - h / 2.0f);
    shapes_.framed(a, out, x, y, w, h, st_.radius(h), st_.panel, st_.line, 1.0f);
    put_text(out, t_count_, std::round(x + pad), y + h / 2.0f, st_.txt);
    counter_ = {x, y, w, h};
  }

  std::string pdf_, dir_;
  int dpi_ = 125, pages_ = 0, cur_ = 0;
  bool busy_ = false, closed_ = false, popped_ = false;
  std::vector<Page> pg_;  // by page number (index 0 unused)
  std::shared_ptr<bool> life_;
  float zoom_ = 1.0f, ox_ = 0, oy_ = 0, dw_ = 0, dh_ = 0;
  bool laid_ = false;
  float W_ = 0, H_ = 0;
  float top_ = 0, ph_ = 0;  // the page area (layout)
  Metrics m_;
  Style st_;
  Shapes shapes_;
  HelpBar help_;
  Txt t_load_, t_err_, t_count_;
  bool failed_ = false;
  std::string count_text_;
  Box counter_;
};

std::unique_ptr<Screen> make_for(Ctx& cx, const std::string& arg, Kind k) {
  std::string system, rom;
  if (!mg::split_game_arg(arg, system, rom) || !available()) return nullptr;
  const std::string pdf = media_file(cx, system, rom, k);
  if (pdf.empty()) return nullptr;
  return std::make_unique<ManualScreen>(cx, pdf);
}

}  // namespace

std::unique_ptr<Screen> make_viewer(Ctx& cx, const std::string& pdf) { return std::make_unique<ManualScreen>(cx, pdf); }

ViewerState viewer_state(const Screen& s) {
  return static_cast<const ManualScreen&>(s).state();  // built with -fno-rtti: `s` must come from make_viewer / the manual screens
}

ZF_REGISTER_SCREEN("menu.game.manual", [](Ctx& cx, const std::string& arg) { return make_for(cx, arg, Kind::Manual); });
ZF_REGISTER_SCREEN("menu.game.magazine", [](Ctx& cx, const std::string& arg) { return make_for(cx, arg, Kind::Magazine); });

}  // namespace zf::manual
