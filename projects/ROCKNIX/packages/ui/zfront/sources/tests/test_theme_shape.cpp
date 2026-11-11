// Theme properties of shape: selStyle (how the selected row is marked) and headerAlign (title alignment). Without the variables
// nothing changes (Zenith); a theme that writes nonsense gets Zenith's value.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "check.hpp"
#include "cpu.hpp"
#include "text.hpp"
#include "ui_style.hpp"
#include "util.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

std::string g_tmp;

/// A theme folder with just the variables given (Style::from needs nothing else).
std::string make_theme(const char* name, const std::string& vars) {
  std::string dir = g_tmp + "/" + name;
  fs::create_directories(dir);
  std::ofstream(dir + "/layout.xml") << "<theme><variables><bg>101010</bg></variables></theme>";
  std::ofstream(dir + "/theme.xml") << "<theme><variables>" << vars << "</variables></theme>";
  return dir;
}

ui::Style style_of(const std::string& dir) {
  Theme th;
  std::string err;
  CHECK(Theme::load(dir, "1/1", th, err));
  return ui::Style::from(th);
}

const Color kBg = {16, 16, 16, 255}, kFill = {10, 20, 200, 255}, kAcc = {250, 200, 0, 255};

/// Anti-aliased arcs reach 253 of 255 coverage at best: compare with a small tolerance.
bool near(Color a, Color b, int tol = 6) {
  for (size_t i = 0; i < 3; ++i)
    if (std::abs(int(a[i]) - int(b[i])) > tol) return false;
  return true;
}

struct Px {
  Canvas cv;
  Color at(uint32_t x, uint32_t y) const {
    const uint8_t* p = &cv.px[(size_t(y) * cv.w + x) * 4];
    return Color{p[0], p[1], p[2], p[3]};
  }
};

/// Draws a 40x100 selector at (10, 10) of a 60x60 canvas... (w=100, h=40 at (10, 10) of a 120x60 one) in style `s`.
Px draw(ui::SelStyle s, float corner) {
  ui::Style st = style_of(make_theme("plain", ""));
  st.sel_style = s;
  st.corner = corner;
  Assets a;
  ui::Shapes sh;
  std::vector<Op> ops;
  ops.push_back(Op::rect(0, 0, 120, 60, kBg));
  sh.selector(a, st, ops, 10, 10, 100, 40, kFill, kAcc);
  return Px{cpu_render(ops, a, 120, 60)};
}

}  // namespace

int main() {
  g_tmp = "/tmp/zf-test-theme-shape-" + std::to_string(getpid());
  fs::create_directories(g_tmp);

  // parsing: anything unknown is Zenith's value
  CHECK(ui::parse_sel_style("fill") == ui::SelStyle::Fill);
  CHECK(ui::parse_sel_style("outline") == ui::SelStyle::Outline);
  CHECK(ui::parse_sel_style("underline") == ui::SelStyle::Underline);
  CHECK(ui::parse_sel_style("bar") == ui::SelStyle::Bar);
  CHECK(ui::parse_sel_style("") == ui::SelStyle::Fill);
  CHECK(ui::parse_sel_style("Outline") == ui::SelStyle::Fill);  // case matters, like every other text variable
  CHECK(ui::parse_sel_style("wavy") == ui::SelStyle::Fill);

  // no variable: exactly the Zenith values
  ui::Style z = style_of(make_theme("none", ""));
  CHECK(z.sel_style == ui::SelStyle::Fill);
  CHECK(!z.header_center);
  CHECK(style_of(make_theme("junk", "<selStyle>zigzag</selStyle><headerAlign>diagonal</headerAlign>")).sel_style == ui::SelStyle::Fill);
  CHECK(!style_of(make_theme("junk2", "<headerAlign>diagonal</headerAlign>")).header_center);
  ui::Style o = style_of(make_theme("set", "<selStyle>bar</selStyle><headerAlign>center</headerAlign>"));
  CHECK(o.sel_style == ui::SelStyle::Bar);
  CHECK(o.header_center);

  // Fill is the rounded rectangle Zenith always drew (same operations as rrect)
  {
    ui::Style st = z;
    Assets a;
    ui::Shapes s1, s2;
    std::vector<Op> v1, v2;
    s1.selector(a, st, v1, 10, 10, 100, 40, kFill, kAcc);
    s2.rrect(a, v2, 10, 10, 100, 40, st.radius(40), kFill);
    CHECK_EQ(v1.size(), v2.size());
    for (size_t i = 0; i < v1.size() && i < v2.size(); ++i)
      CHECK(v1[i].kind == v2[i].kind && v1[i].x == v2[i].x && v1[i].y == v2[i].y && v1[i].w == v2[i].w && v1[i].h == v2[i].h && v1[i].color == v2[i].color);
  }

  // what each style paints (square corners to keep the probe points simple)
  {
    Px f = draw(ui::SelStyle::Fill, 0.0f);
    CHECK(f.at(60, 30) == kFill && f.at(11, 11) == kFill && f.at(5, 30) == kBg);
    Px r = draw(ui::SelStyle::Outline, 0.0f);
    CHECK(r.at(60, 30) == kBg);    // hollow
    CHECK(r.at(10, 30) == kAcc && r.at(109, 30) == kAcc && r.at(60, 10) == kAcc && r.at(60, 49) == kAcc);
    CHECK(r.at(60, 13) == kBg);    // the ring is 3 px at 40 px tall, not more
    CHECK(r.at(5, 30) == kBg);
    Px u = draw(ui::SelStyle::Underline, 0.0f);
    CHECK(u.at(60, 30) == kBg && u.at(60, 49) == kAcc && u.at(10, 49) == kAcc && u.at(109, 49) == kAcc);
    CHECK(u.at(60, 10) == kBg && u.at(60, 50) == kBg);
    Px b = draw(ui::SelStyle::Bar, 0.0f);
    CHECK(b.at(10, 10) == kAcc && b.at(10, 49) == kAcc && b.at(11, 30) == kAcc);
    CHECK(b.at(60, 30) == kBg && b.at(30, 30) == kBg);
    // rounded (corner 1): the outline stays hollow and its corner pixel is not painted; the sides still are
    Px rr = draw(ui::SelStyle::Outline, 1.0f);
    CHECK(rr.at(60, 30) == kBg && rr.at(10, 10) == kBg && rr.at(60, 10) == kAcc && near(rr.at(10, 30), kAcc));
    CHECK(near(rr.at(109, 30), kAcc) && rr.at(60, 49) == kAcc);
  }

  // titles: left-aligned (Zenith) keeps x and a limit that fits; centred centres in the column; both clear the status stack
  {
    ui::Style st = z;
    ui::Metrics m = ui::Metrics::from(Theme(), 640, 480);
    std::vector<Op> stack = {Op::image(1, 560, 10, 40, 20), Op::image(2, 520, 10, 30, 20)};
    CHECK_EQ(ui::title_max_w(st, m, stack, 0, 300.0f), 300.0f);
    CHECK_EQ(ui::title_x(st, m, 33.0f, 120.0f), 33.0f);
    // a long left-aligned title (a game's options: "The Legend of Zelda - Ocarina of Time") ran into the wifi icon on the device (9-nov)
    const float lw = ui::title_max_w(st, m, stack, 0, 600.0f);
    CHECK(lw < 600.0f);
    CHECK(m.title_left + lw < 520.0f);
    st.header_center = true;
    float mw = ui::title_max_w(st, m, stack, 0, 600.0f);
    CHECK(mw < 600.0f);
    CHECK(m.W / 2.0f + mw / 2.0f <= 520.0f);  // right end of a title of that width stays left of the leftmost status image
    float x = ui::title_x(st, m, 33.0f, 120.0f);
    CHECK(x == std::round(m.col_x + (m.col_w - 120.0f) / 2.0f));
    CHECK(ui::title_max_w(st, m, {}, 0, 100.0f) == 100.0f);  // no stack and a short title: untouched
  }

  // fontFallback: the paths a theme declares, relative to its folder
  {
    Theme th;
    std::string err;
    CHECK(Theme::from_xml("<theme/>", "<theme><variables><fontFallback>./a.ttf, assets/b.otf ,,</fontFallback></variables></theme>", "1/1", th, err));
    CHECK(th.font_fallbacks().size() == 2);  // (the folder is empty here: from_xml has none)
    CHECK(th.font_fallbacks()[0] == "a.ttf" || th.font_fallbacks()[0] == "/a.ttf");
    CHECK(th.font_fallbacks()[1] == "assets/b.otf" || th.font_fallbacks()[1] == "/assets/b.otf");
    CHECK(Theme::from_xml("<theme/>", "<theme><variables><x>1</x></variables></theme>", "1/1", th, err));
    CHECK(th.font_fallbacks().empty());  // without the variable: nothing, as always
  }
  // the extra face supplies a glyph the main one lacks (needs the fonts of test_i18n; skipped without them)
  {
    const char* base = "/usr/config/emulationstation/resources";
    std::string rubik = std::string(base) + "/Rubik-Regular.ttf", droid = std::string(base) + "/DroidSansFallbackFull.ttf";
    if (!file_exists(rubik) || !file_exists(droid)) {
      std::printf("test_theme_shape: Rubik/DroidSansFallbackFull not found, fontFallback check skipped\n");
    } else {
      std::string d = g_tmp + "/fonts";
      fs::create_directories(d);
      fs::copy_file(rubik, d + "/Main.ttf");
      fs::copy_file(droid, d + "/Extra.ttf");  // not one of the standard fallback names
      const std::string zh = "\xe8\xae\xbe\xe7\xbd\xae";
      std::string err;
      Font none;  // no declared fallback: the folder has no standard one either, so nothing opens
      CHECK(none.load(d + "/Main.ttf", err));
      none.measure(zh, 24);
      CHECK_EQ(none.fallbacks_open(), 0);
      Font with;
      CHECK(with.load(d + "/Main.ttf", err, {d + "/Extra.ttf", d + "/missing.ttf"}));
      float w = with.measure(zh, 24);
      CHECK(w > 30);
      CHECK_EQ(with.fallbacks_open(), 1);
      Img r = with.render(zh, 24, {255, 255, 255, 255});
      size_t lit = 0;
      for (size_t i = 3; i < r.rgba.size(); i += 4) lit += r.rgba[i] > 0;
      CHECK(lit > 100);
      with.measure("Hello", 24);  // Latin text opens nothing more
      CHECK_EQ(with.fallbacks_open(), 1);
    }
  }

  fs::remove_all(g_tmp);
  return zt::done();
}
