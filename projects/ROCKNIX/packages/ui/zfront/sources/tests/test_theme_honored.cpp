// The choices of an ES theme that zfront redraws itself (THEME CONFIGURATION, subset.<name>):
//  - Iconic honors COLOR SCHEME (dark = the colours its own XML defines for "dark"), PLAY VIDEO PREVIEWS and SHOW DETAILED GAME INFO; the
//    generated theme and the pages drawn from it change with them (needs the merged Zenith folder; that part is skipped when there is none);
//  - THEME CONFIGURATION lists only the subsets that are honored for xmb / artbook / techdweeb / iconic, and everything for any other theme.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "es_theme.hpp"
#include "gfx.hpp"
#include "i18n.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "menu_theme_cfg.hpp"
#include "registry.hpp"
#include "remake.hpp"
#include "scene.hpp"
#include "settings.hpp"
#include "status.hpp"
#include "theme.hpp"
#include "util.hpp"
#include "video.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

std::string find_base(const char* name) {
  if (const char* e = std::getenv("ZF_THEME_DIR")) {
    const std::string p = std::string(e) + "/" + name;
    if (fs::exists(p + "/theme.xml")) return p;
  }
  for (const char* d : {"themes/", "../themes/", "./"}) {
    const std::string p = std::string(d) + name;
    if (fs::exists(p + "/theme.xml")) return p;
  }
  return {};
}

void spit(const std::string& p, const std::string& s) {
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream(p) << s;
}

// The subsets of Iconic (names and options as its theme-config.xml), plus a few variables of its dark scheme and a view.
const char* kIconicXml =
    "<theme><formatVersion>7</formatVersion>"
    "<subset name=\"aspect-ratio\" displayName=\"Aspect Ratio\"><include name=\"1-1\" displayName=\"1:1\"/><include name=\"4-3\" displayName=\"4:3\"/></subset>"
    "<subset name=\"color-scheme\" displayName=\"Color Scheme\"><include name=\"light\" displayName=\"Light\"/><include name=\"dark\" displayName=\"Dark\"/></subset>"
    "<subset name=\"media-type\" displayName=\"Game Media Type\"><include name=\"image\" displayName=\"Image\"/><include name=\"boxart\" displayName=\"Boxart\"/></subset>"
    "<subset name=\"play-video-previews-of-games\" displayName=\"Play Video Previews\"><include name=\"yes\" displayName=\"Yes\"/><include name=\"no\" displayName=\"No\"/></subset>"
    "<subset name=\"show-detailed-game-info\" displayName=\"Show Detailed Game Info\"><include name=\"yes\" displayName=\"Yes\"/><include name=\"no\" displayName=\"No\"/></subset>"
    "<subset name=\"smooth-resize\" displayName=\"Smooth Resize\"><include name=\"yes\" displayName=\"Yes\"/><include name=\"no\" displayName=\"No\"/></subset>"
    "<variables ifSubset=\"color-scheme:dark\"><menuBackgroundColor>151515</menuBackgroundColor><menuSelectedColor>f0f0f0</menuSelectedColor>"
    "<gameMetadataColor>cccccc</gameMetadataColor><systemInfoSecondaryColor>a0a0a0</systemInfoSecondaryColor><gamelistSelectorColor>444444</gamelistSelectorColor></variables>"
    "<view name=\"system\"><carousel name=\"systemcarousel\"><type>horizontal</type></carousel></view></theme>";
const char* kXmbXml =
    "<theme><formatVersion>7</formatVersion>"
    "<subset name=\"color-scheme\" displayName=\"Color Scheme\"><include name=\"dark\" displayName=\"Dark\"/><include name=\"light\" displayName=\"Light\"/></subset>"
    "<subset name=\"wallpaper\" displayName=\"Wallpaper\"><include name=\"1\" displayName=\"1\"/><include name=\"2\" displayName=\"2\"/></subset></theme>";
const char* kOtherXml =
    "<theme><formatVersion>7</formatVersion>"
    "<subset name=\"color-scheme\" displayName=\"Color Scheme\"><include name=\"dark\" displayName=\"Dark\"/><include name=\"light\" displayName=\"Light\"/></subset>"
    "<subset name=\"aspect-ratio\" displayName=\"Aspect Ratio\"><include name=\"1-1\" displayName=\"1:1\"/><include name=\"4-3\" displayName=\"4:3\"/></subset>"
    "<view name=\"system\"><carousel name=\"systemcarousel\"><type>horizontal</type></carousel></view></theme>";  // a real theme has views

std::vector<std::string> labels(Menu& m) {
  std::vector<std::string> v;
  for (size_t i = 0; i < m.size(); ++i) v.push_back(m.entry(i).label);
  return v;
}
bool has(const std::vector<std::string>& v, const std::string& s) {
  for (const std::string& x : v)
    if (x == s) return true;
  return false;
}

float lum(const Color& c) { return (0.2126f * float(c[0]) + 0.7152f * float(c[1]) + 0.0722f * float(c[2])) / 255.0f; }

}  // namespace

int main() {
  // ---- which subsets are honored
  {
    CHECK(themecfg::honored("iconic", "color-scheme"));
    CHECK(themecfg::honored("iconic", "play-video-previews-of-games"));
    CHECK(themecfg::honored("iconic", "show-detailed-game-info"));
    CHECK(!themecfg::honored("iconic", "aspect-ratio"));
    CHECK(!themecfg::honored("iconic", "smooth-resize"));
    for (const char* style : {"xmb", "artbook", "techdweeb"}) {
      CHECK(!themecfg::honored(style, "color-scheme"));
      CHECK(!themecfg::honored(style, "aspect-ratio"));
    }
    CHECK(themecfg::honored("", "anything"));  // any other theme: everything
  }

  // ---- THEME CONFIGURATION for the themes on disk
  {
    char tmpl[] = "/tmp/zf-test-honored-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string themes = dir + "/storage/.config/emulationstation/themes";
    spit(themes + "/es-theme-iconic/theme.xml", kIconicXml);
    spit(themes + "/es-theme-xmb-fcamod/theme.xml", kXmbXml);
    spit(themes + "/es-theme-other/theme.xml", kOtherXml);
    spit(themes + "/es-theme-zenith/theme.xml", "<theme/>");
    mg::env().root = dir;
    mg::reset_caches();
    Ctx cx;
    std::string err;
    CHECK(Theme::from_xml("<theme/>", "<theme/>", "4/3", cx.th, err));
    cx.w = 640;
    cx.h = 480;
    auto use = [&](const std::string& set) {
      cx.es = EsSettings::from_text("<?xml version=\"1.0\"?>\n<string name=\"ThemeSet\" value=\"" + set + "\"/>\n", dir + "/es_settings.cfg");
    };
    use("es-theme-iconic");
    CHECK(themecfg::available(cx, nullptr));
    {
      auto s = Registry::instance().make("menu.ui.theme", cx, "");
      CHECK(s != nullptr);
      if (s) {
        Menu& m = *static_cast<Menu*>(s.get());
        m.rebuild(cx);
        const std::vector<std::string> l = labels(m);
        CHECK(has(l, "Color Scheme"));
        CHECK(has(l, "Play Video Previews"));
        CHECK(has(l, "Show Detailed Game Info"));
        CHECK(!has(l, "Aspect Ratio"));
        CHECK(!has(l, "Game Media Type"));
        CHECK(!has(l, "Smooth Resize"));
        CHECK(has(l, tr("RESET CUSTOMIZATIONS")));
      }
    }
    use("es-theme-xmb-fcamod");  // nothing honored: no row at all, no screen
    CHECK(!themecfg::available(cx, nullptr));
    CHECK(Registry::instance().make("menu.ui.theme", cx, "") == nullptr);
    use("es-theme-other");  // a theme zfront does not redraw keeps every row
    CHECK(themecfg::available(cx, nullptr));
    {
      auto s = Registry::instance().make("menu.ui.theme", cx, "");
      CHECK(s != nullptr);
      if (s) {
        Menu& m = *static_cast<Menu*>(s.get());
        m.rebuild(cx);
        const std::vector<std::string> l = labels(m);
        CHECK(has(l, "Color Scheme"));
        CHECK(has(l, "Aspect Ratio"));
      }
    }
    use("es-theme-zenith");
    CHECK(!themecfg::available(cx, nullptr));
    mg::env().root.clear();
    mg::reset_caches();
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  // ---- Iconic drawn with its choices
  const std::string light = find_base("theme-light"), dark = find_base("theme-dark");
  if (light.empty() || dark.empty()) {
    std::printf("sin carpetas del tema: se omite la parte de dibujo\n");
    return zt::done();
  }
  char tmpl[] = "/tmp/zf-test-honored-draw-XXXXXX";
  const std::string root = mkdtemp(tmpl);
  const std::string es = root + "/es-theme-iconic";
  spit(es + "/theme.xml", kIconicXml);

  struct Drawn {
    Color bg{};
    Color first_rect{};
    bool dark_flag = false;
    size_t ops = 0;
    bool video_box = false;
    std::string ink, card;
  };
  const std::vector<std::string> names = {"A game", "Another game", "Short", "Game four", "Game five"};
  auto draw = [&](const std::string& scheme, const std::string& video, const std::string& detail) {
    Drawn r;
    estheme::Options opt;
    opt.setting = [&](const std::string& k) -> const std::string* {
      if (k == "subset.color-scheme" && !scheme.empty()) return &scheme;
      if (k == "subset.play-video-previews-of-games" && !video.empty()) return &video;
      if (k == "subset.show-detailed-game-info" && !detail.empty()) return &detail;
      return nullptr;
    };
    estheme::Generated gen;
    std::string err;
    CHECK(estheme::generate(es, estheme::Bases{light, dark}, root + "/gen", opt, gen, err));
    Theme th;
    CHECK(Theme::load(gen.dir, "1/1", th, err));
    CHECK(remake::kind_of(th) == remake::Kind::Iconic);
    Assets a;
    CHECK(a.load_font(th.path(th.str("font")), err));
    r.bg = th.color("bg");
    r.dark_flag = th.str("icDark", "?") == "1";
    r.ink = std::string(th.str("icInk", "?"));
    r.card = std::string(th.str("icCard", "?"));
    const Status st = Status::demo("12:04");
    DetailedData d;
    d.name = "A game";
    d.year = 1994;
    d.genre = "RPG";
    d.players = "1-2";
    d.rating = 0.7f;
    d.desc = "A description with some words in it, long enough to need a few lines of the card of the game.";
    d.video = "/nonexistent/video.mp4";
    d.desc_time = -1;
    std::vector<Op> ops;
    remake::state().system_theme = "snes";
    video::box_reset();
    remake::list_view(th, a, 720, 720, "Super Nintendo", names, 1, st, &d, ops);
    r.ops = ops.size();
    r.video_box = video::box_get().valid();
    for (const Op& op : ops)
      if (op.kind == Op::Rect) {
        r.first_rect = op.color;
        break;
      }
    // and the system view, which must still draw
    std::vector<SystemItem> items(3);
    for (size_t i = 0; i < items.size(); ++i) {
      items[i].theme_name = "snes";
      items[i].full_name = "Super Nintendo";
      items[i].games = 3;
    }
    std::vector<Op> sys;
    InfoFade fade;
    remake::system_view(th, a, 720, 720, items, 0, 0.0f, st, nullptr, fade, sys);
    CHECK(!sys.empty());
    return r;
  };

  const Drawn l = draw("", "", ""), dk = draw("dark", "", "");
  CHECK(!l.dark_flag);
  CHECK(dk.dark_flag);
  CHECK(lum(l.bg) > 0.8f);    // light: the page is almost white
  CHECK(lum(dk.bg) < 0.15f);  // dark: the 151515 of the theme's own menuBackgroundColor
  CHECK(l.first_rect == l.bg);  // the first thing the list draws is the page, in the theme's background
  CHECK(dk.first_rect == dk.bg);
  CHECK(l.first_rect != dk.first_rect);
  CHECK(dk.bg == (Color{0x15, 0x15, 0x15, 255}));
  CHECK_EQ(dk.ink, std::string("F0F0F0"));  // text from the XML's menuSelectedColor
  CHECK_EQ(l.ink, std::string("1C1E26"));
  CHECK(dk.card != l.card);

  const Drawn v_off = draw("", "no", "");
  CHECK(l.video_box);  // previews on (the default): the list offers the box of the picture
  CHECK(!v_off.video_box);
  const Drawn d_off = draw("", "", "no");
  CHECK(d_off.ops < l.ops);  // no genre / year / players chips, no description

  std::error_code ec;
  fs::remove_all(root, ec);
  return zt::done();
}
