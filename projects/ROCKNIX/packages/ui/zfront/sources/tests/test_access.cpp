// Accessibility: TEXT SIZE (SMALL / NORMAL / LARGE / EXTRA LARGE) and HIGH CONTRAST. The text px grows with the setting, the rows of a menu get
// taller and fewer fit the screen (never more than the viewport), high contrast changes the text and panel colours, and an open menu follows
// at once. test_layout.cpp measures the grid at LARGE and EXTRA LARGE on the real themes.
#include <cmath>
#include <memory>
#include <string>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "ui_prefs.hpp"
#include "ui_style.hpp"

using namespace zf;

namespace {

// Geometry of the 4:3 layout of the Zenith theme (layout.xml), enough for Metrics.
const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

void make_ctx(Ctx& cx, float w, float h) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = w;
  cx.h = h;
  cx.sv->cmd.dry_run = true;
}

float lum(Color c) {
  auto ch = [](uint8_t v) {
    const float x = float(v) / 255.0f;
    return x <= 0.03928f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
  };
  return 0.2126f * ch(c[0]) + 0.7152f * ch(c[1]) + 0.0722f * ch(c[2]);
}
float ratio(Color a, Color b) {
  const float la = lum(a), lb = lum(b);
  return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

}  // namespace

int main() {
  using ui::TextSize;
  const int sizes[][2] = {{480, 320}, {640, 480}, {720, 720}, {800, 480}};

  // ---- parsing and factors
  CHECK(ui::parse_text_size("small") == TextSize::Small);
  CHECK(ui::parse_text_size("large") == TextSize::Large);
  CHECK(ui::parse_text_size("xlarge") == TextSize::XLarge);
  CHECK(ui::parse_text_size("normal") == TextSize::Normal);
  CHECK(ui::parse_text_size("") == TextSize::Normal);
  CHECK(ui::parse_text_size("huge") == TextSize::Normal);
  CHECK(ui::text_scale(TextSize::Small) < ui::text_scale(TextSize::Normal));
  CHECK(ui::text_scale(TextSize::Normal) == 1.0f);
  CHECK(ui::text_scale(TextSize::Normal) < ui::text_scale(TextSize::Large));
  CHECK(ui::text_scale(TextSize::Large) < ui::text_scale(TextSize::XLarge));

  // ---- the generation grows only when something changed
  ui::set_access(TextSize::Normal, false);
  const unsigned g0 = ui::access().gen;
  ui::set_access(TextSize::Normal, false);
  CHECK_EQ(ui::access().gen, g0);
  ui::set_access(TextSize::Large, false);
  CHECK(ui::access().gen != g0);
  ui::set_access(TextSize::Normal, false);

  // ---- metrics: text px grows with the setting, the rows get taller, fewer fit, and never more than the viewport
  for (const auto& sz : sizes) {
    Ctx cx;
    make_ctx(cx, float(sz[0]), float(sz[1]));
    ui::set_access(TextSize::Normal, false);
    const ui::Metrics n = ui::Metrics::from(cx.th, cx.w, cx.h);
    ui::set_access(TextSize::Small, false);
    const ui::Metrics s = ui::Metrics::from(cx.th, cx.w, cx.h);
    ui::set_access(TextSize::Large, false);
    const ui::Metrics l = ui::Metrics::from(cx.th, cx.w, cx.h);
    ui::set_access(TextSize::XLarge, false);
    const ui::Metrics x = ui::Metrics::from(cx.th, cx.w, cx.h);
    ui::set_access(TextSize::Normal, false);

    CHECK(s.em < n.em && n.em < l.em && l.em < x.em);
    CHECK(s.small_px <= n.small_px && n.small_px < l.small_px && l.small_px < x.small_px);
    CHECK(s.foot_px <= n.foot_px && n.foot_px <= l.foot_px && l.foot_px <= x.foot_px);
    CHECK(s.title_px <= n.title_px && n.title_px < l.title_px && l.title_px < x.title_px);
    // smaller text keeps the rows; larger text makes them taller, so fewer fit
    CHECK_EQ(s.row_h, n.row_h);
    CHECK_EQ(s.lines, n.lines);
    CHECK(l.row_h > n.row_h && x.row_h > l.row_h);
    CHECK(l.lines <= n.lines && x.lines < n.lines && x.lines <= l.lines);
    CHECK(x.lines >= 3);  // still a usable list at EXTRA LARGE, on the smallest screen too
    for (const ui::Metrics* m : {&s, &n, &l, &x}) {
      CHECK_EQ(m->list_h, m->row_h * float(m->lines));
      CHECK(m->list_h <= n.list_h + 0.5f);  // the viewport of the screen is never exceeded
      CHECK(m->pill_h <= m->row_h);
      CHECK_EQ(int(m->row_h - m->pill_h) % 2, 0);
      CHECK(float(m->em) <= m->pill_h);  // the text fits the selector
    }
  }

  // ---- high contrast: text pure white / black, opaque panels, a solid selection
  {
    Ctx cx;
    make_ctx(cx, 640, 480);
    ui::set_access(TextSize::Normal, false);
    const ui::Style n = ui::Style::from(cx.th);
    ui::set_access(TextSize::Normal, true);
    const ui::Style h = ui::Style::from(cx.th);
    CHECK(n.dark && h.dark);
    CHECK(n.txt != h.txt);
    CHECK((h.txt == Color{255, 255, 255, 255}));
    CHECK(h.panel != n.panel);
    CHECK_EQ(int(h.panel[3]), 255);
    CHECK_EQ(int(h.osd_bg[3]), 255);
    CHECK_EQ(int(h.cyan_soft[3]), 255);
    CHECK_EQ(int(h.group_bg[3]), 255);
    CHECK(h.scrim[3] > n.scrim[3]);
    CHECK(ratio(h.txt, h.bg) > ratio(n.txt, n.bg));
    CHECK(ratio(h.dim, h.bg) > ratio(n.dim, n.bg));
    CHECK(ratio(h.txt, h.bg) >= 12.0f);
    CHECK(ratio(h.cyan_dark, h.cyan_soft) >= 7.0f);  // the selected row's text on its fill
    // the colours parsed from the theme are not touched: back to normal gives the original again
    ui::set_access(TextSize::Normal, false);
    const ui::Style back = ui::Style::from(cx.th);
    CHECK(back.txt == n.txt && back.panel == n.panel && back.cyan_soft == n.cyan_soft);
  }

  // ---- the setting comes from es_settings.cfg through ui_prefs, live
  {
    Ctx cx;
    make_ctx(cx, 640, 480);
    cx.es.set_string("TextSize", "xlarge");
    cx.es.set_bool("HighContrast", true);
    ui_prefs::reapply(cx);
    CHECK(ui::access().size == TextSize::XLarge);
    CHECK(ui::access().contrast);
    cx.es.set_string("TextSize", "bogus");
    cx.es.set_bool("HighContrast", false);
    ui_prefs::reapply(cx);
    CHECK(ui::access().size == TextSize::Normal);
    CHECK(!ui::access().contrast);
  }

  // ---- an open menu follows the change at once: more text px, fewer rows
  {
    Ctx cx;
    make_ctx(cx, 640, 480);
    ui::set_access(TextSize::Normal, false);
    MenuSpec spec;
    spec.title = "TEST";
    spec.build = [](MenuBuilder& m, Ctx&) {
      for (int i = 0; i < 30; ++i) m.action("ROW " + std::to_string(i), [] {});
    };
    Menu menu(std::move(spec));
    menu.handle(Action::Left, cx);  // the first handle builds and lays out
    const int rows_n = menu.visible_rows();
    CHECK(rows_n >= 7);
    ui::set_access(TextSize::XLarge, false);
    menu.tick(0.0f, cx);
    const int rows_x = menu.visible_rows();
    CHECK(rows_x < rows_n);
    CHECK(rows_x >= 3);
    ui::set_access(TextSize::Normal, false);
    menu.tick(0.0f, cx);
    CHECK_EQ(menu.visible_rows(), rows_n);
  }
  ui::set_access(TextSize::Normal, false);
  return zt::done();
}
