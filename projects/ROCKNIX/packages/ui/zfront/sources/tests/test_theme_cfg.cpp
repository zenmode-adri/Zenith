// THEME CONFIGURATION (menu_theme_cfg.cpp): the subsets a theme declares (es_theme.cpp `list_subsets`: filters, variables read later, appliesTo),
// the rows and the ES keys they store (Theme* for the six special subsets, `subset.<name>` for the rest, "" for the default), the screen
// `menu.ui.theme`, its row in USER INTERFACE SETTINGS (hidden for a theme without subsets, as in ES), and ES's themesettings/<ThemeSet>.cfg
// (written and read back when the theme set changes). Temp dirs only.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "es_theme.hpp"
#include "i18n.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "menu_theme_cfg.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;

namespace {

std::string tmpdir(const char* tag) {
  std::string d = "/tmp/zf_test_theme_cfg_" + std::string(tag) + "_" + std::to_string(getpid());
  std::string cmd = "rm -rf " + d + " && mkdir -p " + d;
  CHECK(std::system(cmd.c_str()) == 0);
  return d;
}
void spit(const std::string& p, const std::string& s) {
  FILE* f = std::fopen(p.c_str(), "wb");
  CHECK(f != nullptr);
  if (f) {
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
  }
}
std::string slurp(const std::string& p) {
  std::string s;
  read_file(p, s);
  return s;
}
bool has(const std::string& hay, const std::string& n) { return hay.find(n) != std::string::npos; }
std::string get(const EsSettings& es, const std::string& k) {
  const std::string* v = es.get(k);
  return v ? *v : std::string("<none>");
}

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

// An ES theme with subsets: a filtered option, a name that is a variable defined by a file read later, an option with no displayName, subsets
// with `appliesTo` (listed last), a "-" subset (no row), and one declared in an included file.
const char* kThemeXml =
    "<theme>\n<formatVersion>7</formatVersion>\n<variables><colorLabel>Colour scheme</colorLabel><subset.gl-style>List style</subset.gl-style></variables>\n"
    "<include>./_inc/more.xml</include>\n"
    "<subset name=\"menu-style\" appliesTo=\"all\"><include name=\"a\" displayName=\"A\"/><include name=\"b\" displayName=\"B\"/></subset>\n"
    "<subset name=\"gl-style\" appliesTo=\"detailed, grid\"><include name=\"one\" displayName=\"One\"/><include name=\"two\" displayName=\"Two\"/></subset>\n"
    "<subset name=\"colorset\" displayName=\"${colorLabel}\"><include name=\"dark\" displayName=\"Dark\"/><include name=\"light\" displayName=\"Light\"/>"
    "<include name=\"sepia\" displayName=\"Sepia\" if=\"{screen.ratio} == '16/9'\"/></subset>\n"
    "<subset name=\"aspect-ratio\" displayName=\"${aspectLabel}\"><include if=\"{screen.ratio} == '4/3'\" name=\"4-3-auto\" displayName=\"Automatic\"/>"
    "<include name=\"4-3\" displayName=\"4:3\"/><include name=\"16-9\"/></subset>\n"
    "<subset name=\"region\" displayName=\"-\"><include name=\"eu\"/></subset>\n"
    "<include>./_inc/vars.xml</include>\n"
    "<view name=\"system\"><text name=\"t\"><color>FFFFFF</color></text></view>\n</theme>\n";
const char* kMoreXml =
    "<theme><variables><laterLabel>System view</laterLabel></variables>\n"
    "<subset name=\"systemview\" displayName=\"${laterLabel}\"><include name=\"carousel\" displayName=\"Carousel\"/><include name=\"grid\" displayName=\"Grid\"/></subset></theme>\n";
const char* kVarsXml = "<theme><variables><aspectLabel>Aspect ratio</aspectLabel></variables></theme>\n";
const char* kPlainXml = "<theme><variables><a>1</a><b>2</b></variables><view name=\"system\"><text name=\"t\"><color>FFFFFF</color></text></view></theme>\n";

struct Fixture {
  std::string dir, themes, user;
  Ctx cx;
  explicit Fixture(const char* tag) {
    dir = tmpdir(tag);
    user = dir + "/storage/.config/emulationstation";
    themes = user + "/themes";
    CHECK(std::system(("mkdir -p " + themes + "/es-theme-test/_inc " + themes + "/es-theme-plain").c_str()) == 0);
    spit(themes + "/es-theme-test/theme.xml", kThemeXml);
    spit(themes + "/es-theme-test/_inc/more.xml", kMoreXml);
    spit(themes + "/es-theme-test/_inc/vars.xml", kVarsXml);
    spit(themes + "/es-theme-plain/theme.xml", kPlainXml);
    std::string err;
    CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
    cx.w = 640;
    cx.h = 480;
    mg::env().root = dir;
    mg::reset_caches();
    use("es-theme-test");
  }
  ~Fixture() {
    mg::env().root.clear();
    mg::reset_caches();
    std::string cmd = "rm -rf " + dir;
    if (std::system(cmd.c_str())) {
    }
  }
  void use(const std::string& theme_set, const std::string& more = {}) {
    cx.es = EsSettings::from_text("<?xml version=\"1.0\"?>\n<string name=\"ThemeSet\" value=\"" + theme_set + "\"/>\n" + more, dir + "/es_settings.cfg");
  }
};

Menu& as_menu(std::unique_ptr<Screen>& s) { return *static_cast<Menu*>(s.get()); }
int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}
const themecfg::Row* row_of(const std::vector<themecfg::Row>& rows, const std::string& subset) {
  for (const themecfg::Row& r : rows)
    if (r.subset == subset) return &r;
  return nullptr;
}
std::vector<std::string> split_cr(const std::string& t) {
  std::vector<std::string> v;
  size_t i = 0;
  while (i < t.size()) {
    size_t e = t.find('\r', i);
    if (e == std::string::npos) e = t.size();
    v.push_back(t.substr(i, e - i));
    i = e + 1;
  }
  return v;
}
bool lists(const std::vector<std::string>& v, const std::string& l) {
  for (const std::string& s : v)
    if (s == l) return true;
  return false;
}

}  // namespace

int main() {
  // ---- the subsets of a theme
  std::vector<estheme::SubsetInfo> infos;
  {
    Fixture f("subsets");
    estheme::Options opt;
    opt.ratio = "4/3";
    opt.width = 640;
    opt.height = 480;
    infos = estheme::list_subsets(f.themes + "/es-theme-test", opt);
    std::vector<std::string> names;
    for (const auto& i : infos) names.push_back(i.name);
    // order of appearance; the include read first brings its subset first
    CHECK_EQ(names.size(), size_t(6));
    if (names.size() == 6) {
      CHECK_EQ(names[0], std::string("systemview"));
      CHECK_EQ(names[1], std::string("menu-style"));
      CHECK_EQ(names[2], std::string("gl-style"));
      CHECK_EQ(names[3], std::string("colorset"));
      CHECK_EQ(names[4], std::string("aspect-ratio"));
      CHECK_EQ(names[5], std::string("region"));
    }
    auto sub = [&](const std::string& n) -> const estheme::SubsetInfo* {
      for (const auto& i : infos)
        if (i.name == n) return &i;
      return nullptr;
    };
    const estheme::SubsetInfo* cs = sub("colorset");
    CHECK(cs != nullptr);
    if (cs) {
      CHECK_EQ(cs->display, std::string("Colour scheme"));  // ${colorLabel}
      CHECK_EQ(cs->options.size(), size_t(2));              // sepia is for 16/9
      CHECK_EQ(cs->options[0].name, std::string("dark"));
      CHECK_EQ(cs->options[0].display, std::string("Dark"));
      CHECK(cs->applies_to.empty());
    }
    const estheme::SubsetInfo* sv = sub("systemview");
    CHECK(sv != nullptr && sv->display == "System view");  // its variable is in the same file, but read after the subset
    const estheme::SubsetInfo* ar = sub("aspect-ratio");
    CHECK(ar != nullptr);
    if (ar) {
      CHECK_EQ(ar->display, std::string("Aspect ratio"));  // ${aspectLabel}: defined in a file read AFTER the subset
      CHECK_EQ(ar->options.size(), size_t(3));
      CHECK_EQ(ar->options[0].name, std::string("4-3-auto"));  // the one whose `if` passes comes first: the default
      CHECK_EQ(ar->options[0].display, std::string("Automatic"));
      CHECK_EQ(ar->options[2].display, std::string("16-9"));  // no displayName: the name
    }
    const estheme::SubsetInfo* gl = sub("gl-style");
    CHECK(gl != nullptr);
    if (gl) {
      CHECK_EQ(gl->display, std::string("List style"));  // the theme variable `subset.<name>` when the subset has no displayName
      CHECK_EQ(gl->applies_to.size(), size_t(2));
    }
    const estheme::SubsetInfo* ms = sub("menu-style");
    CHECK(ms != nullptr && ms->display.empty() && ms->applies_to.size() == 1);
    // another screen ratio: the filtered option appears and the "auto" one does not
    opt.ratio = "16/9";
    opt.width = 1280;
    opt.height = 720;
    auto wide = estheme::list_subsets(f.themes + "/es-theme-test", opt);
    for (const auto& i : wide)
      if (i.name == "colorset") CHECK_EQ(i.options.size(), size_t(3));
    // the theme default of a subset: the setting is not needed to list it
    CHECK(estheme::list_subsets(f.themes + "/es-theme-plain", opt).empty());
    CHECK(estheme::list_subsets(f.themes + "/nope", opt).empty());
  }

  // ---- rows and keys
  {
    CHECK_EQ(themecfg::setting_key("colorset"), std::string("ThemeColorSet"));
    CHECK_EQ(themecfg::setting_key("iconset"), std::string("ThemeIconSet"));
    CHECK_EQ(themecfg::setting_key("menu"), std::string("ThemeMenu"));
    CHECK_EQ(themecfg::setting_key("systemview"), std::string("ThemeSystemView"));
    CHECK_EQ(themecfg::setting_key("gamelistview"), std::string("ThemeGamelistView"));
    CHECK_EQ(themecfg::setting_key("region"), std::string("ThemeRegionName"));
    CHECK_EQ(themecfg::setting_key("aspect-ratio"), std::string("subset.aspect-ratio"));
    CHECK_EQ(themecfg::setting_key("menu-style"), std::string("subset.menu-style"));  // only `menu` itself is special

    std::vector<themecfg::Row> rows = themecfg::rows_for(infos);
    std::vector<std::string> order;
    for (const auto& r : rows) order.push_back(r.subset);
    // the "-" subset with one option has no row; the subsets with appliesTo go last (ES's stable sort)
    CHECK_EQ(order.size(), size_t(5));
    if (order.size() == 5) {
      CHECK_EQ(order[0], std::string("systemview"));
      CHECK_EQ(order[1], std::string("colorset"));
      CHECK_EQ(order[2], std::string("aspect-ratio"));
      CHECK_EQ(order[3], std::string("menu-style"));
      CHECK_EQ(order[4], std::string("gl-style"));
    }
    const themecfg::Row* cs = row_of(rows, "colorset");
    CHECK(cs != nullptr);
    if (cs) {
      CHECK_EQ(cs->key, std::string("ThemeColorSet"));
      CHECK_EQ(cs->label, std::string("Colour scheme"));
      CHECK_EQ(cs->def, std::string("dark"));
      CHECK(!cs->applies);
      // the choice: what is stored when it is an option, else the default; the default is stored as ""
      EsSettings es = EsSettings::from_text("<string name=\"ThemeColorSet\" value=\"light\"/>");
      CHECK_EQ(themecfg::current_value(es, *cs), std::string("light"));
      EsSettings gone = EsSettings::from_text("<string name=\"ThemeColorSet\" value=\"purple\"/>");  // not an option of this theme
      CHECK_EQ(themecfg::current_value(gone, *cs), std::string("dark"));
      EsSettings none = EsSettings::from_text("<bool name=\"x\" value=\"true\"/>");
      CHECK_EQ(themecfg::current_value(none, *cs), std::string("dark"));
      CHECK_EQ(themecfg::stored_value(*cs, "dark"), std::string());
      CHECK_EQ(themecfg::stored_value(*cs, "light"), std::string("light"));
    }
    const themecfg::Row* ms = row_of(rows, "menu-style");
    CHECK(ms != nullptr);
    if (ms) {
      CHECK_EQ(ms->label, std::string("THEME MENU-STYLE"));  // ES: "THEME <SUBSET>" when the theme names none
      CHECK(!ms->applies);                                   // ...and it is listed under THEME OPTIONS
    }
    const themecfg::Row* gl = row_of(rows, "gl-style");
    CHECK(gl != nullptr && gl->applies);  // a title and appliesTo: GAMELIST THEME OPTIONS
  }

  // ---- the screen and its row in USER INTERFACE SETTINGS
  {
    Fixture f("screen");
    CHECK(themecfg::available(f.cx, nullptr));
    {
      auto s = Registry::instance().make("menu.ui.theme", f.cx, "");
      CHECK(s != nullptr);
      Menu& m = as_menu(s);
      m.rebuild(f.cx);
      CHECK_EQ(m.title(), std::string(tr("THEME CONFIGURATION")));
      int h1 = find(m, tr("THEME OPTIONS")), cs = find(m, "Colour scheme"), ar = find(m, "Aspect ratio"), ms = find(m, "THEME MENU-STYLE");
      int h2 = find(m, tr("GAMELIST THEME OPTIONS")), gl = find(m, "List style"), tools = find(m, tr("TOOLS")), reset = find(m, tr("RESET CUSTOMIZATIONS"));
      CHECK(h1 >= 0 && cs > h1 && ar > cs && ms > ar);
      CHECK(h2 > ms && gl > h2);
      CHECK(tools > gl && reset > tools);
      if (cs >= 0) {
        CHECK_EQ(m.entry(size_t(cs)).get_s(), std::string("dark"));  // nothing stored: the default
        CHECK_EQ(m.entry(size_t(cs)).options.size(), size_t(2));
        CHECK_EQ(m.entry(size_t(cs)).options[1].label, std::string("Light"));
        // the default picked again stores nothing, so leaving does not reload
        m.entry(size_t(cs)).set_s("dark");
        CHECK(m.handle(Action::B, f.cx).kind == Transition::Pop);
        CHECK(!oscontract::quit_requested());
        CHECK_EQ(get(f.cx.es, "ThemeColorSet"), std::string("<none>"));
      }
    }
    {
      auto s = Registry::instance().make("menu.ui.theme", f.cx, "");
      Menu& m = as_menu(s);
      m.rebuild(f.cx);
      int cs = find(m, "Colour scheme"), ar = find(m, "Aspect ratio");
      CHECK(cs >= 0 && ar >= 0);
      if (cs >= 0 && ar >= 0) {
        m.entry(size_t(cs)).set_s("light");
        m.entry(size_t(ar)).set_s("16-9");
        CHECK_EQ(get(f.cx.es, "ThemeColorSet"), std::string("<none>"));  // applied when the screen is left, like ES's save function
        CHECK_EQ(m.entry(size_t(cs)).get_s(), std::string("light"));    // the screen shows the pending choice
        CHECK(!oscontract::quit_requested());
        CHECK(m.handle(Action::B, f.cx).kind == Transition::Pop);
        CHECK_EQ(get(f.cx.es, "ThemeColorSet"), std::string("light"));
        CHECK_EQ(get(f.cx.es, "subset.aspect-ratio"), std::string("16-9"));
        CHECK(oscontract::quit_requested());  // the frontend restarts to read the theme again
        EsSettings back = EsSettings::load(f.dir + "/es_settings.cfg");
        CHECK_EQ(get(back, "ThemeColorSet"), std::string("light"));
        CHECK_EQ(get(back, "subset.aspect-ratio"), std::string("16-9"));
      }
    }
    // the row of USER INTERFACE SETTINGS: there for a theme with subsets, not for one without, not for Zenith
    {
      auto u = Registry::instance().make("menu.ui", f.cx, "");
      CHECK(u != nullptr);
      Menu& m = as_menu(u);
      m.rebuild(f.cx);
      int row = find(m, tr("THEME CONFIGURATION"));
      CHECK(row >= 0);
      if (row >= 0) CHECK(m.entry(size_t(row)).show_arrow);
    }
    f.use("es-theme-plain");
    CHECK(!themecfg::available(f.cx, nullptr));
    CHECK(Registry::instance().make("menu.ui.theme", f.cx, "") == nullptr);
    {
      auto u = Registry::instance().make("menu.ui", f.cx, "");
      Menu& m = as_menu(u);
      m.rebuild(f.cx);
      CHECK(find(m, tr("THEME CONFIGURATION")) < 0);
    }
    f.use("es-theme-zenith");
    CHECK(!themecfg::available(f.cx, nullptr));
    CHECK(Registry::instance().make("menu.ui.theme", f.cx, "") == nullptr);
    f.use("");
    CHECK(!themecfg::available(f.cx, nullptr));
  }

  // ---- themesettings/<ThemeSet>.cfg
  {
    Fixture f("file");
    f.use("es-theme-a",
          "<string name=\"ThemeColorSet\" value=\"dark\"/>\n<string name=\"ThemeMenu\" value=\"\"/>\n<string name=\"subset.aspect-ratio\" value=\"16-9\"/>\n"
          "<string name=\"subset.gb.colorset\" value=\"red\"/>\n<string name=\"GamelistViewStyle\" value=\"grid\"/>\n<string name=\"DefaultGridSize\" value=\"3x3\"/>\n"
          "<string name=\"gb.defaultView\" value=\"detailed\"/>\n<bool name=\"DrawClock\" value=\"true\"/>\n");
    const std::string dirc = themecfg::settings_dir();
    CHECK_EQ(dirc, f.user + "/themesettings");
    themecfg::save_for(f.cx, "es-theme-a");
    std::string t = slurp(dirc + "/es-theme-a.cfg");
    CHECK(!t.empty() && t.back() == '\r');
    CHECK(t.find('\n') == std::string::npos);  // ES ends each line with \r
    std::vector<std::string> lines = split_cr(t);
    CHECK(lists(lines, "colorset=dark"));  // the six special ones are written by their bare name
    CHECK(lists(lines, "subset.aspect-ratio=16-9"));
    CHECK(lists(lines, "subset.gb.colorset=red"));
    CHECK(lists(lines, "GamelistViewStyle=grid"));
    CHECK(lists(lines, "DefaultGridSize=3x3"));
    CHECK(lists(lines, "gb.defaultView=detailed"));
    CHECK(!has(t, "ThemeColorSet"));
    CHECK(!has(t, "menu="));  // empty values are not written
    CHECK(!has(t, "DrawClock"));
    CHECK_EQ(lines.size(), size_t(6));

    // picking another theme set: the old one is remembered, the choices are cleared, nothing is loaded for a set that has no file
    themecfg::switch_theme(f.cx, "es-theme-b");
    CHECK_EQ(get(f.cx.es, "ThemeSet"), std::string("es-theme-b"));
    for (const char* k : {"ThemeColorSet", "subset.aspect-ratio", "subset.gb.colorset", "GamelistViewStyle", "DefaultGridSize", "gb.defaultView"}) CHECK_EQ(get(f.cx.es, k), std::string());
    CHECK_EQ(get(f.cx.es, "DrawClock"), std::string("true"));  // the rest of the settings are not touched
    CHECK(file_exists(dirc + "/es-theme-a.cfg"));
    CHECK(!file_exists(dirc + "/es-theme-b.cfg"));
    f.cx.es.set_string("ThemeColorSet", "x");
    f.cx.es.set_string("subset.q", "1");
    // ...and back: b is saved, a comes back
    themecfg::switch_theme(f.cx, "es-theme-a");
    CHECK_EQ(get(f.cx.es, "ThemeSet"), std::string("es-theme-a"));
    CHECK_EQ(get(f.cx.es, "ThemeColorSet"), std::string("dark"));
    CHECK_EQ(get(f.cx.es, "subset.aspect-ratio"), std::string("16-9"));
    CHECK_EQ(get(f.cx.es, "subset.gb.colorset"), std::string("red"));
    CHECK_EQ(get(f.cx.es, "GamelistViewStyle"), std::string("grid"));
    CHECK_EQ(get(f.cx.es, "DefaultGridSize"), std::string("3x3"));
    CHECK_EQ(get(f.cx.es, "gb.defaultView"), std::string("detailed"));
    CHECK_EQ(get(f.cx.es, "subset.q"), std::string());
    std::vector<std::string> bl = split_cr(slurp(dirc + "/es-theme-b.cfg"));
    CHECK(lists(bl, "colorset=x"));
    CHECK(lists(bl, "subset.q=1"));

    // nothing to remember: the file goes away; "automatic" is not a choice
    f.use("es-theme-c", "<string name=\"GamelistViewStyle\" value=\"automatic\"/>\n");
    spit(dirc + "/es-theme-c.cfg", "colorset=old\r");
    themecfg::save_for(f.cx, "es-theme-c");
    CHECK(!file_exists(dirc + "/es-theme-c.cfg"));
    // a name that leaves the folder writes nothing
    f.use("../evil", "<string name=\"ThemeColorSet\" value=\"dark\"/>\n");
    themecfg::save_for(f.cx, "../evil");
    CHECK(!file_exists(f.user + "/evil.cfg"));
    CHECK(!file_exists(dirc + "/../evil.cfg"));

    // reading: comments, a name without a value, junk lines and \n separators (a file edited by hand)
    f.use("es-theme-d");
    spit(dirc + "/es-theme-d.cfg", "#colorset=no\nfoo=bar\ncolorset=Z\n;x=1\nsubset.q=7\n=novalue\nmenu=\nregion=fr\niconset=I\r\n");
    themecfg::load_for(f.cx, "es-theme-d");
    CHECK_EQ(get(f.cx.es, "ThemeColorSet"), std::string("Z"));
    CHECK_EQ(get(f.cx.es, "subset.q"), std::string("7"));
    CHECK_EQ(get(f.cx.es, "ThemeRegionName"), std::string("fr"));
    CHECK_EQ(get(f.cx.es, "ThemeIconSet"), std::string("I"));
    CHECK_EQ(get(f.cx.es, "foo"), std::string("<none>"));
    CHECK_EQ(get(f.cx.es, "ThemeMenu"), std::string("<none>"));
    CHECK_EQ(get(f.cx.es, "x"), std::string("<none>"));

    // RESET CUSTOMIZATIONS: the Theme* choices go, the remembered file of the set goes, the other settings stay
    f.use("es-theme-test", "<string name=\"ThemeColorSet\" value=\"light\"/>\n<string name=\"subset.aspect-ratio\" value=\"4-3\"/>\n<bool name=\"DrawClock\" value=\"true\"/>\n");
    spit(dirc + "/es-theme-test.cfg", "colorset=light\r");
    auto s = Registry::instance().make("menu.ui.theme", f.cx, "");
    CHECK(s != nullptr);
    if (s) {
      Menu& m = as_menu(s);
      m.rebuild(f.cx);
      int reset = find(m, tr("RESET CUSTOMIZATIONS"));
      CHECK(reset >= 0);
      for (size_t i = 0; i < m.size() + 1 && m.cursor() != reset; ++i) m.handle(Action::Down, f.cx);
      Transition t2 = m.handle(Action::A, f.cx);
      CHECK(t2.kind == Transition::Pop);
      CHECK_EQ(get(f.cx.es, "ThemeColorSet"), std::string());
      CHECK_EQ(get(f.cx.es, "subset.aspect-ratio"), std::string());
      CHECK_EQ(get(f.cx.es, "DrawClock"), std::string("true"));
      CHECK(!file_exists(dirc + "/es-theme-test.cfg"));
    }
  }
  return zt::done();
}
