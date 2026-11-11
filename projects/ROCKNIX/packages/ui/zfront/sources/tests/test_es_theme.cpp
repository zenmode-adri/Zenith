// ES theme translator: parsing (includes, cycles, subsets, variables, conditions, unsafe paths, broken/huge XML), the colour mapping,
// the generated Zenith-structured folder (loads, no missing variables, Style sees the colours) and the THEME SET integration.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "bootargs.hpp"
#include "check.hpp"
#include "es_theme.hpp"
#include "imgdec.hpp"
#include "theme.hpp"
#include "ui_style.hpp"
#include "util.hpp"

using namespace zf;

namespace {

std::string g_tmp;

const unsigned char kWhite[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00,
                                0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0x0f, 0x05, 0x0c, 0x30, 0x06, 0x00, 0x8f, 0x82,
                                0x0f, 0xf1, 0x3c, 0xa5, 0x56, 0x51, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
const unsigned char kCyan[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00,
                               0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x38, 0xfc, 0xf8, 0x3f, 0x18, 0xc3, 0x18, 0x00,
                               0x59, 0x0c, 0x0a, 0x95, 0x0c, 0x4f, 0xdf, 0xc4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

void mkdirs(const std::string& d) {
  std::string c = "mkdir -p '" + d + "'";
  CHECK(std::system(c.c_str()) == 0);
}
void put(const std::string& path, const std::string& text) {
  mkdirs(path_dir(path));
  FILE* f = std::fopen(path.c_str(), "wb");
  CHECK(f != nullptr);
  if (!f) return;
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
}
void put_png(const std::string& path, bool cyan) {
  put(path, cyan ? std::string(reinterpret_cast<const char*>(kCyan), sizeof kCyan) : std::string(reinterpret_cast<const char*>(kWhite), sizeof kWhite));
}
bool is_link(const std::string& p) {
  struct stat sb;
  return ::lstat(p.c_str(), &sb) == 0 && S_ISLNK(sb.st_mode);
}

estheme::Options opts(const char* ratio = "4/3", unsigned w = 640, unsigned h = 480) {
  estheme::Options o;
  o.ratio = ratio;
  o.width = w;
  o.height = h;
  return o;
}

// ---- fixtures ------------------------------------------------------------------------------------------------------------------------

// Zenith-like base (theme.xml with the variables and the literal menu colours mkvariant.py replaces, layout.xml with the essentials)
void make_base(const std::string& dir, bool dark) {
  const char* c = dark ? "26262A,F0F0F2,BEBEC6,B8B8C0,F0F0F2,00C3E3,4FD8F0,22525F" : "EBEBED,2D2D32,4E4E56,4A4A52,2D2D32,00C3E3,00708A,C4EAF3";
  std::vector<std::string> v;
  for (std::string s = c; !s.empty();) {
    size_t k = s.find(',');
    v.push_back(s.substr(0, k));
    s = k == std::string::npos ? "" : s.substr(k + 1);
  }
  std::string x = "<?xml version=\"1.0\"?>\n<theme>\n<formatVersion>7</formatVersion>\n<variables>\n<bg>" + v[0] + "</bg><txt>" + v[1] + "</txt><dim>" + v[2] + "</dim><dimDark>" + v[3] +
                  "</dimDark><tileTxt>" + v[4] + "</tileTxt><cyan>" + v[5] + "</cyan><cyanDark>" + v[6] + "</cyanDark><cyanSoft>" + v[7] +
                  "</cyanSoft><font>./assets/fonts/Rubik-Regular.ttf</font></variables>\n<include>./layout.xml</include>\n<view name=\"menu\">\n";
  x += dark ? "<menuBackground name=\"menubg\"><color>2F2F34</color></menuBackground><menuText name=\"menutext\"><separatorColor>3E3E44</separatorColor></menuText>"
              "<menuSwitch><colorOn>4C97A8</colorOn><colorOff>6C6C76</colorOff></menuSwitch><menuGroup><color>A8A8B0</color></menuGroup>"
            : "<menuBackground name=\"menubg\"><color>F7F7F9</color></menuBackground><menuText name=\"menutext\"><separatorColor>E1E1E6</separatorColor></menuText>"
              "<menuSwitch><colorOn>3A8CA0</colorOn><colorOff>A9A9B2</colorOff></menuSwitch><menuGroup><color>5F5F66</color></menuGroup>";
  x += "</view>\n</theme>\n";
  put(dir + "/theme.xml", x);
  put(dir + "/layout.xml",
      "<theme><formatVersion>7</formatVersion><variables><carW>1.1</carW><carY>0.47</carY><cellW>0.19</cellW><fSys>0.028</fSys><listLines>9</listLines><selH>0.0625</selH>"
      "<fList>0.035</fList><helpY>0.95</helpY><fHelp>0.025</fHelp><stackH>0.044</stackH></variables></theme>");
  put(dir + "/assets/fonts/Rubik-Regular.ttf", "not a font, never loaded");
  put_png(dir + "/assets/px.png", false);
  put_png(dir + "/assets/tile.png", false);
  put_png(dir + "/assets/frame.png", true);
  put_png(dir + "/assets/pill-4x3.png", true);
  put_png(dir + "/assets/menu/textinput.png", false);
  put_png(dir + "/assets/logos/auto-favorites.webp", true);
  put_png(dir + "/assets/logos/gba.webp", true);
  put(dir + "/assets/logos/icon-labels/favs/auto-favorites.txt", "FAVORITES");
}

void make_es_theme(const std::string& dir) {
  put(dir + "/theme.xml",
      "<theme>\n<formatVersion>7</formatVersion>\n"
      "<subset name=\"scheme\" displayName=\"Scheme\"><include name=\"night\"/><include name=\"day\"/></subset>\n"
      "<variables><base>./inc</base><one>1</one><fontF>./fonts/Gilroy-Bold.ttf</fontF></variables>\n"
      "<variables><calc>${one} + 2 * 3</calc><undef>${nothere}/x</undef></variables>\n"
      "<include>${base}/colors.xml</include>\n"
      "<include>./cycle_a.xml</include>\n<include>../outside.xml</include>\n<include>/etc/passwd</include>\n<include>~/x.xml</include>\n"
      "<include>./link_out.xml</include>\n<include>./missing.xml</include>\n"
      "<variables if=\"{screen.ratio} == '4/3'\"><ratioVar>four-three</ratioVar></variables>\n"
      "<variables if=\"{screen.ratio} == '16/9' || ${screen.width} > 1000\"><ratioVar>wide</ratioVar></variables>\n"
      "<view name=\"basic,detailed\">\n"
      "  <image name=\"bgimg\"><size>1 1</size><color>101820</color><path>./px.png</path><zIndex>0</zIndex></image>\n"
      "  <image name=\"card\"><size>1 1</size><color>1c2630</color><zIndex>1</zIndex></image>\n"
      "  <textlist name=\"gamelist\"><primaryColor>${txtc}</primaryColor><selectedColor>FFFFFF</selectedColor><selectorColor>3355AA</selectorColor>"
      "<fontPath>${fontF}</fontPath></textlist>\n"
      "  <text name=\"t\"><color>AAAAAA</color><fontPath>./fonts/ok.ttf</fontPath></text>\n"
      "</view>\n"
      "<view name=\"system\"><image name=\"logo\"><path>./logos/${system.theme}.png</path></image></view>\n"
      "<view name=\"menu\"><menuBackground name=\"menubg\"><color>0C1218</color></menuBackground></view>\n"
      "</theme>\n");
  put(dir + "/inc/colors.xml",
      "<theme><variables ifSubset=\"scheme:night\"><txtc>E0E8F0</txtc></variables><variables ifSubset=\"scheme:day\"><txtc>101010</txtc></variables></theme>");
  put(dir + "/cycle_a.xml", "<theme><include>./cycle_b.xml</include><variables><fromA>1</fromA></variables></theme>");
  put(dir + "/cycle_b.xml", "<theme><include>./cycle_a.xml</include></theme>");
  put(dir + "/../outside.xml", "<theme><variables><leak>1</leak></variables></theme>");
  put(g_tmp + "/secret.xml", "<theme><variables><leak2>1</leak2></variables></theme>");
  CHECK(::symlink((g_tmp + "/secret.xml").c_str(), (dir + "/link_out.xml").c_str()) == 0);
  put(dir + "/px.png", std::string(reinterpret_cast<const char*>(kWhite), sizeof kWhite));
  put_png(dir + "/logos/gba.png", false);
  put_png(dir + "/logos/auto-favorites.png", false);
  put(g_tmp + "/secret.png", std::string(reinterpret_cast<const char*>(kWhite), sizeof kWhite));
  CHECK(::symlink((g_tmp + "/secret.png").c_str(), (dir + "/logos/leak.png").c_str()) == 0);
  put(dir + "/fonts/Gilroy-Bold.ttf", std::string(5000, 'x'));
  put(dir + "/fonts/ok.ttf", std::string(5000, 'y'));
}

// ---- tests ----------------------------------------------------------------------------------------------------------------------------

void test_conditions() {
  std::map<std::string, std::string> v = {{"screen.ratio", "4/3"}, {"screen.width", "640"}, {"name", "x"}};
  auto ev = [&](const char* e, bool want, bool want_ok = true) {
    bool ok = false;
    bool parsed = estheme::eval_condition(e, v, ok);
    if (parsed != want_ok || (parsed && ok != want)) std::fprintf(stderr, "condition '%s': parsed=%d value=%d\n", e, parsed, ok);
    CHECK(parsed == want_ok);
    if (want_ok) CHECK(ok == want);
  };
  ev("{screen.ratio} == '4/3'", true);
  ev("{screen.ratio} == '16/9'", false);
  ev("{screen.ratio} != '16/9' && {screen.width} >= 640", true);
  ev("{screen.ratio} == '16/9' || ({screen.width} > 600 && !(1 == 2))", true);
  ev("{screen.width} < 480", false);
  ev("{nothing} == ''", true);
  ev("name == 'x'", true);
  ev("1 + 2 * 3 == 7", true);
  ev("1 ? 0 : 1", false);
  ev("((((", false, false);
  ev("{screen.ratio} ==", false, false);
  ev("'unterminated", false, false);
  ev(std::string(5000, '(').c_str(), false, false);
}

void test_resolve() {
  const std::string es = g_tmp + "/es/theme-fx";
  make_es_theme(es);
  estheme::Resolved r;
  std::string err;
  CHECK(estheme::resolve(es, opts(), r, err));
  CHECK_EQ(r.vars["txtc"], std::string("E0E8F0"));  // subset default = first option
  CHECK_EQ(r.subsets["scheme"], std::string("night"));
  CHECK_EQ(r.vars["ratioVar"], std::string("four-three"));
  CHECK_EQ(r.vars["calc"], std::string("7"));  // ${one} + 2 * 3
  CHECK_EQ(r.vars["undef"], std::string("${nothere}/x"));  // unknown variables stay as written
  CHECK(r.vars.count("fromA") == 1);                      // the cycle was read once and then cut by the depth limit
  CHECK(r.vars.count("leak") == 0 && r.vars.count("leak2") == 0);  // ../, absolute, ~ and symlink escapes were not followed
  CHECK(r.warnings.size() >= 5);
  bool saw_list = false;
  for (const estheme::Element& e : r.elems)
    if (e.type == "textlist" && e.name == "gamelist" && e.view == "detailed") saw_list = e.props.at("primaryColor") == "E0E8F0" && e.props.at("fontPath") == "./fonts/Gilroy-Bold.ttf";
  CHECK(saw_list);

  // the same theme with a setting, another screen
  estheme::Options o = opts("16/9", 854, 480);
  const std::string day = "day";
  o.setting = [&](const std::string& k) -> const std::string* { return k == "subset.scheme" ? &day : nullptr; };
  CHECK(estheme::resolve(es, o, r, err));
  CHECK_EQ(r.vars["txtc"], std::string("101010"));
  CHECK_EQ(r.vars["ratioVar"], std::string("wide"));
  const std::string bogus = "nonsense";
  o.setting = [&](const std::string& k) -> const std::string* { return k == "subset.scheme" ? &bogus : nullptr; };
  CHECK(estheme::resolve(es, o, r, err));
  CHECK_EQ(r.vars["txtc"], std::string("E0E8F0"));  // a setting that names no option is ignored

  // broken, empty, missing, huge: a controlled false, never a crash
  put(g_tmp + "/es/broken/theme.xml", "<theme><variables><a>1</a>");
  CHECK(!estheme::resolve(g_tmp + "/es/broken", opts(), r, err) && !err.empty());
  put(g_tmp + "/es/empty/theme.xml", "<theme><formatVersion>7</formatVersion></theme>");
  CHECK(!estheme::resolve(g_tmp + "/es/empty", opts(), r, err));
  CHECK(!estheme::resolve(g_tmp + "/es/nothing-here", opts(), r, err));
  put(g_tmp + "/es/huge/theme.xml", "<theme><variables><a>" + std::string(3u << 20, 'a') + "</a></variables></theme>");
  CHECK(!estheme::resolve(g_tmp + "/es/huge", opts(), r, err));
  put(g_tmp + "/es/notxml/theme.xml", std::string("\x00\x01\xff garbage <<<", 20));
  CHECK(!estheme::resolve(g_tmp + "/es/notxml", opts(), r, err));
  // a chain of 14 includes: the deepest are dropped with a warning
  for (int i = 0; i < 14; ++i) put(g_tmp + "/es/deep/c" + std::to_string(i) + ".xml", "<theme><variables><d" + std::to_string(i) + ">1</d" + std::to_string(i) + "></variables><include>./c" + std::to_string(i + 1) + ".xml</include></theme>");
  put(g_tmp + "/es/deep/theme.xml", "<theme><formatVersion>7</formatVersion><include>./c0.xml</include><view name=\"basic\"><text name=\"t\"><color>FFFFFF</color></text></view></theme>");
  CHECK(estheme::resolve(g_tmp + "/es/deep", opts(), r, err));
  CHECK(r.vars.count("d0") == 1 && r.vars.count("d7") == 1 && r.vars.count("d13") == 0);
  CHECK(!r.warnings.empty());
  // fix4 z3 #10: <feature> inside <feature> for ever (a corrupt theme) is cut with a warning, not a stack overflow
  {
    std::string open, close;
    for (int i = 0; i < 3000; ++i) {
      open += "<feature>";
      close += "</feature>";
    }
    put(g_tmp + "/es/nest/theme.xml", "<theme><formatVersion>7</formatVersion>" + open + "<variables><deep>1</deep></variables>" + close + "</theme>");
    estheme::Resolved rn;
    std::string en;
    CHECK(estheme::resolve(g_tmp + "/es/nest", opts(), rn, en) || !en.empty());
    CHECK(rn.vars.count("deep") == 0);  // never reached: cut at the nesting limit
    bool cut = false;
    for (const std::string& w : rn.warnings) cut = cut || w.find("nested too deep") != std::string::npos;
    CHECK(cut);
    // control: one level of <feature> is parsed without the nesting warning (so the warning above comes from the limit)
    put(g_tmp + "/es/nest1/theme.xml", "<theme><formatVersion>7</formatVersion><feature><variables><deep>1</deep></variables></feature><view name=\"basic\"><text name=\"t\"><color>FFFFFF</color></text></view></theme>");
    rn = estheme::Resolved();
    CHECK(estheme::resolve(g_tmp + "/es/nest1", opts(), rn, en));
    for (const std::string& w : rn.warnings) CHECK(w.find("nested too deep") == std::string::npos);
  }
}

void test_theme_file() {
  const std::string es = g_tmp + "/es/theme-fx";
  CHECK(!estheme::theme_file(es, "./fonts/ok.ttf").empty());
  CHECK(estheme::theme_file(es, "../outside.xml").empty());
  CHECK(estheme::theme_file(es, "/etc/passwd").empty());
  CHECK(estheme::theme_file(es, "~/x").empty());
  CHECK(estheme::theme_file(es, "./inc/../../outside.xml").empty());
  CHECK(estheme::theme_file(es, "./link_out.xml").empty());  // symlink that leaves the theme
  CHECK(estheme::theme_file(es, "./nope.png").empty());
  CHECK(estheme::theme_file(es, "").empty());
}

void test_contrast() {
  CHECK(estheme::contrast({0, 0, 0}, {255, 255, 255}) > 20.9);
  estheme::Rgb fg = {120, 120, 130};
  CHECK(estheme::fix_contrast(fg, {128, 128, 140}, 4.5));
  CHECK(estheme::contrast(fg, {128, 128, 140}) >= 4.5 || fg == (estheme::Rgb{0, 0, 0}) || fg == (estheme::Rgb{255, 255, 255}));
  estheme::Rgb ok = {255, 255, 255};
  CHECK(!estheme::fix_contrast(ok, {0, 0, 0}, 7.0));  // already fine: unchanged
  estheme::Rgb m = {200, 200, 200};
  CHECK(estheme::fix_multi(m, {{255, 255, 255}, {20, 20, 20}}, 4.5));  // no colour is readable on white and near-black at once: the best one is returned
  estheme::Rgb m3 = {230, 100, 100};
  CHECK(estheme::fix_multi(m3, {{255, 255, 255}, {250, 250, 250}}, 4.5) && estheme::contrast(m3, {250, 250, 250}) >= 4.5);
  estheme::Rgb m2 = {10, 10, 10};
  CHECK(estheme::fix_multi(m2, {{240, 240, 240}, {200, 200, 200}}, 4.5) == false);
}

void test_generate() {
  const std::string root = g_tmp + "/root";
  make_base(root + "/usr/share/zfront/theme-light", false);
  make_base(root + "/usr/share/zfront/theme-dark", true);
  estheme::Bases bases{root + "/usr/share/zfront/theme-light", root + "/usr/share/zfront/theme-dark"};
  const std::string es = g_tmp + "/es/theme-fx";
  const std::string out_root = g_tmp + "/gen";
  estheme::Generated g;
  std::string err;
  CHECK(estheme::generate(es, bases, out_root, opts(), g, err));
  CHECK_EQ(g.dir, out_root + "/theme-fx");
  CHECK(g.dark);  // bg 101820 -> the dark base
  CHECK(!g.cached);
  CHECK(!g.report_text.empty());

  Theme th;
  CHECK(Theme::load(g.dir, "4/3", th, err));
  CHECK_EQ(th.missing(), std::string());  // every essential variable of Zenith is there
  CHECK(th.color("bg") == (Color{0x10, 0x18, 0x20, 255}));
  const ui::Style st = ui::Style::from(th);
  CHECK(st.dark);
  CHECK(st.panel == (Color{0x0C, 0x12, 0x18, 255}));  // the menu background of the ES theme reaches the menu panel
  CHECK(estheme::contrast({th.color("txt")[0], th.color("txt")[1], th.color("txt")[2]}, {0x10, 0x18, 0x20}) >= 7.0);
  CHECK(std::string(th.str("font")).find("Rubik") != std::string::npos);  // Gilroy is proprietary, ok.ttf is not a font: Zenith's font
  CHECK(g.report_text.find("proprietary") != std::string::npos);
  CHECK(g.report_text.find("cannot be loaded") != std::string::npos);

  // recoloured PNGs: the cyan frame follows the (blue) accent, the pill takes the selection colour
  Img f, pill;
  CHECK(decode_image_file(g.dir + "/assets/frame.png", f) && f.w == 2);
  CHECK(!(f.rgba[0] == 0 && f.rgba[1] == 195 && f.rgba[2] == 227));
  CHECK(decode_image_file(g.dir + "/assets/pill-4x3.png", pill));
  CHECK(pill.rgba[0] == 0x33 && pill.rgba[1] == 0x55 && pill.rgba[2] == 0xAA);
  CHECK(!is_link(g.dir + "/assets/frame.png") && is_link(g.dir + "/layout.xml") && is_link(g.dir + "/assets/px.png"));

  // logos of the ES theme replace Zenith's; collection icons keep Zenith's; a symlink that leaves the theme is not used
  CHECK(is_link(g.dir + "/assets/logos/gba.webp"));
  CHECK(!file_exists(g.dir + "/assets/logos/leak.webp"));
  CHECK(!is_link(g.dir + "/assets/logos/auto-favorites.webp"));  // retinted copy, not the ES logo
  CHECK(file_exists(g.dir + "/palette.json") && file_exists(g.dir + "/report.txt"));

  // second run: nothing changed -> reused; the theme changes -> rebuilt
  estheme::Generated g2;
  CHECK(estheme::generate(es, bases, out_root, opts(), g2, err));
  CHECK(g2.cached && g2.dir == g.dir && g2.dark == g.dark);
  CHECK(g2.report_text.find("[cached]") != std::string::npos);
  put(es + "/theme.xml", "<theme>\n<!-- edited -->\n<formatVersion>7</formatVersion><view name=\"basic\"><image name=\"b\"><size>1 1</size><color>F0F0F0</color></image>"
                         "<text name=\"t\"><color>202020</color></text></view></theme>");
  CHECK(estheme::generate(es, bases, out_root, opts(), g2, err));
  CHECK(!g2.cached && !g2.dark);  // now a light theme: the light base
  CHECK(Theme::load(g2.dir, "4/3", th, err) && th.missing().empty());
  CHECK(th.color("bg") == (Color{0xF0, 0xF0, 0xF0, 255}));
  CHECK(!file_exists(g2.dir + "/assets/logos/leak.webp"));

  // a folder of that name that is not ours is never deleted
  mkdirs(out_root + "/theme-other");
  put(out_root + "/theme-other/precious.txt", "mine");
  put(g_tmp + "/es/theme-other/theme.xml", "<theme><formatVersion>7</formatVersion><view name=\"basic\"><text name=\"t\"><color>FFFFFF</color></text></view></theme>");
  CHECK(!estheme::generate(g_tmp + "/es/theme-other", bases, out_root, opts(), g2, err));
  CHECK(file_exists(out_root + "/theme-other/precious.txt"));

  // unusable inputs
  CHECK(!estheme::generate(g_tmp + "/es/broken", bases, out_root, opts(), g2, err));
  CHECK(!estheme::generate(es, estheme::Bases{g_tmp + "/nowhere", g_tmp + "/nowhere2"}, out_root, opts(), g2, err));
  // only the light base exists: it serves a dark theme too
  estheme::Bases light_only{bases.light, ""};
  make_es_theme(g_tmp + "/es/theme-fx2");
  CHECK(estheme::generate(g_tmp + "/es/theme-fx2", light_only, out_root, opts(), g2, err));
  CHECK(Theme::load(g2.dir, "4/3", th, err) && th.missing().empty());
}

// Zenith OS fork checkout (real theme fonts, ES catalogs, the image being built): ZF_FORK_DIR, else ~/r36s-fork.
static std::string fork_dir() {
  if (const char* e = std::getenv("ZF_FORK_DIR")) return e;
  const char* h = std::getenv("HOME");
  return std::string(h ? h : "") + "/r36s-fork";
}

void test_real_font() {
  const std::string rubik_path = fork_dir() + "/projects/ROCKNIX/packages/ui/themes/es-theme-zenith/theme/assets/fonts/Rubik-Regular.ttf";
  const char* rubik = rubik_path.c_str();
  if (!file_exists(rubik)) {
    std::fprintf(stderr, "(no Rubik on this machine: the real-font case is skipped)\n");
    return;
  }
  const std::string es = g_tmp + "/es/theme-font";
  std::string ttf;
  CHECK(read_file(rubik, ttf));
  put(es + "/theme.xml", "<theme><formatVersion>7</formatVersion><view name=\"basic\"><image name=\"b\"><size>1 1</size><color>202020</color></image>"
                         "<textlist name=\"l\"><primaryColor>FFFFFF</primaryColor><fontPath>./f/Nice.ttf</fontPath></textlist></view></theme>");
  put(es + "/f/Nice.ttf", ttf);
  const std::string root = g_tmp + "/root";
  estheme::Bases bases{root + "/usr/share/zfront/theme-light", root + "/usr/share/zfront/theme-dark"};
  estheme::Generated g;
  std::string err;
  CHECK(estheme::generate(es, bases, g_tmp + "/gen", opts(), g, err));
  Theme th;
  CHECK(Theme::load(g.dir, "4/3", th, err));
  CHECK_EQ(std::string(th.str("font")), std::string("./assets/fonts/esfont.ttf"));
  CHECK(is_link(g.dir + "/assets/fonts/esfont.ttf") && file_exists(th.path(th.str("font"))));
}

void test_boot_integration() {
  const std::string r = g_tmp + "/bootroot";
  make_base(r + "/usr/share/zfront/theme-light", false);
  make_base(r + "/usr/share/zfront/theme-dark", true);
  put(r + "/usr/share/themes/es-theme-zenith/layout.xml", "<theme/>");  // a Zenith-structured one: not an ES theme
  put(r + "/usr/share/themes/es-theme-zenith/theme.xml", "<theme/>");
  put(r + "/usr/share/themes/some-zenith-clone/theme.xml", "<theme/>");   // named zenith, no layout.xml: still ours, excluded
  put(r + "/usr/share/themes/es-theme-foo/theme.xml", "<theme/>");
  put(r + "/storage/.config/emulationstation/themes/bar/theme.xml", "<theme/>");
  put(r + "/usr/share/themes/not-a-theme/readme.txt", "x");
  CHECK_EQ(boot::resolve_es_theme("es-theme-foo", r), r + "/usr/share/themes/es-theme-foo");
  CHECK_EQ(boot::resolve_es_theme("bar", r), r + "/storage/.config/emulationstation/themes/bar");
  CHECK(boot::resolve_es_theme("es-theme-zenith", r).empty());
  CHECK(boot::resolve_es_theme("some-zenith-clone", r).empty());
  CHECK(boot::resolve_es_theme("not-a-theme", r).empty());
  CHECK(boot::resolve_es_theme("../themes/es-theme-foo", r).empty());
  CHECK(boot::resolve_es_theme("", r).empty());
  CHECK(boot::resolve_es_theme(".hidden", r).empty());
  const std::vector<boot::ThemeChoice> es = boot::list_es_themes(r);
  CHECK(es.size() == 2 && es[0].es && es[1].es);
  bool foo = false, bar = false;
  for (const boot::ThemeChoice& t : es) {
    foo = foo || (t.value == "es-theme-foo" && t.label == "Foo (ES)");
    bar = bar || (t.value == "bar" && t.label == "Bar (ES)");
  }
  CHECK(foo && bar);
  const std::vector<boot::ThemeChoice> all = boot::list_theme_sets(r);
  size_t n_es = 0;
  for (const boot::ThemeChoice& t : all) n_es += t.es ? 1 : 0;
  CHECK(n_es == 2 && all.size() >= 3 && !all[0].es);  // Zenith's first, the ES ones listed too

  // the whole path as main.cpp takes it
  put(r + "/usr/share/themes/es-theme-foo/theme.xml",
      "<theme><formatVersion>7</formatVersion><view name=\"basic\"><image name=\"b\"><size>1 1</size><color>182030</color></image>"
      "<text name=\"t\"><color>F0F0F0</color></text></view></theme>");
  std::string dir, report, err;
  auto none = [](const std::string&) -> const std::string* { return nullptr; };
  CHECK(boot::translate_es_theme(r + "/usr/share/themes/es-theme-foo", 640, 480, none, r, g_tmp + "/gen2", dir, report, err));
  Theme th;
  CHECK(Theme::load(dir, ratio_name(640, 480), th, err) && th.missing().empty());
  CHECK(report.find("background") != std::string::npos);
  CHECK(!boot::translate_es_theme(r + "/usr/share/themes/es-theme-zenith", 640, 480, none, r, g_tmp + "/gen2", dir, report, err));  // <theme/> is not an ES theme
  CHECK(!err.empty());
  // with no Zenith base installed the caller gets a reason (and falls back to Zenith itself)
  CHECK(!boot::translate_es_theme(r + "/usr/share/themes/es-theme-foo", 640, 480, none, g_tmp + "/noroot", g_tmp + "/gen2", dir, report, err));
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-esth-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  test_conditions();
  test_resolve();
  test_theme_file();
  test_contrast();
  test_generate();
  test_real_font();
  test_boot_integration();
  std::string c = "rm -rf '" + g_tmp + "'";
  if (std::system(c.c_str()) != 0) std::fprintf(stderr, "cleanup failed\n");
  return zt::done();
}
