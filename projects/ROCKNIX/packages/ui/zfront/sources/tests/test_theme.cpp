#include <string>

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
  CHECK(!Theme::from_xml("<theme><variables>", theme, "1/1", t, err));

  CHECK(std::string(ratio_name(720, 720)) == "1/1");
  CHECK(std::string(ratio_name(640, 480)) == "4/3");
  CHECK(std::string(ratio_name(480, 320)) == "3/2");
  CHECK(std::string(ratio_name(720, 405)) == "16/9");
  CHECK(parse_color("zz") == (Color{0, 0, 0, 255}));
  return zt::done();
}
