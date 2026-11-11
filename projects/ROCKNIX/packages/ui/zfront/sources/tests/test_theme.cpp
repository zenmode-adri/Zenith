#include <map>
#include <string>

#include "bootargs.hpp"
#include "check.hpp"
#include "theme.hpp"

using namespace zf;

int main() {
  const std::string layout =
      "<theme><variables><cellW>0.2</cellW><label>base</label></variables>"
      "<variables if=\"{$screen.ratio} == '4/3'\"><cellW>0.3</cellW></variables></theme>";
  const std::string theme = "<theme><variables><accent>FF8000</accent><tint>11223344</tint></variables></theme>";
  Theme t;
  std::string err;
  CHECK(Theme::from_xml(layout, theme, "1/1", t, err));
  CHECK_EQ(t.f("cellW"), 0.2f);
  CHECK(Theme::from_xml(layout, theme, "4/3", t, err));
  CHECK_EQ(t.f("cellW"), 0.3f);  // matching ratio block overrides the default one
  CHECK(t.str("label") == "base");
  CHECK(t.color("accent") == (Color{0xFF, 0x80, 0x00, 0xFF}));
  CHECK(t.color("tint") == (Color{0x11, 0x22, 0x33, 0x44}));
  CHECK(t.has("accent") && !t.has("nope"));
  CHECK_EQ(t.f("nope", 1.23f), 1.23f);  // optional properties fall back to Zenith's value, without a warning
  CHECK_EQ(t.f("label", 7.0f), 7.0f);   // not a number: default too
  CHECK_EQ(t.f("cellW", 7.0f), 0.3f);   // present: the theme wins
  CHECK(t.str("nope", "dflt") == "dflt" && t.str("label", "dflt") == "base");
  CHECK(!Theme::from_xml("<theme><variables>", theme, "1/1", t, err));

  {  // the user's own choices (system.cfg zfront.*) go over the theme's; unknown values leave the theme alone
    Theme u;
    CHECK(Theme::from_xml(layout, "<theme><variables><selStyle>bar</selStyle><carCount>5</carCount></variables></theme>", "1/1", u, err));
    std::map<std::string, std::string> conf = {{"zfront.sel_style", "outline"}, {"zfront.car_count", "9"}, {"zfront.header_align", "center"}};
    boot::apply_user_theme(u, [&](const std::string& k) { return conf.count(k) ? conf[k] : std::string(); });
    CHECK(u.str("selStyle") == "outline");     // valid: replaces the theme's
    CHECK(u.str("carCount") == "5");           // 9 is not on the list: the theme's own stays
    CHECK(u.str("headerAlign") == "center");   // not in the theme: added
    CHECK(!u.has("listCase"));                 // nothing stored: nothing added
  }

  CHECK(std::string(ratio_name(720, 720)) == "1/1");
  CHECK(std::string(ratio_name(640, 480)) == "4/3");
  CHECK(std::string(ratio_name(480, 320)) == "3/2");
  CHECK(std::string(ratio_name(720, 405)) == "16/9");
  CHECK(parse_color("zz") == (Color{0, 0, 0, 255}));
  return zt::done();
}
