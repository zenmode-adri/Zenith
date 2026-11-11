// `ssmedia`: the slideshow / random-video screensavers. ES defaults of the settings, the source of the media (the games' metadata or a
// custom directory with ES's extension filter), ES's pick rules (random without repeats until the list is used up), the swap timing, the
// fade and the game-info schedule, the rows of SCREENSAVER SETTINGS, and that nothing runs when no media screensaver was asked for.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "oscontract.hpp"
#include "screensaver.hpp"
#include "ssmedia.hpp"
#include "video.hpp"

using namespace zf;
using std::string;
using std::vector;
using std::chrono::milliseconds;

namespace {

string g_dir;

void sh(const string& c) {
  if (std::system(c.c_str()) != 0) std::fprintf(stderr, "command failed: %s\n", c.c_str());
}
void put(const string& path, const string& data = "x") {
  std::ofstream(path, std::ios::binary) << data;
}
string slurp(const string& path) {
  std::ifstream f(path, std::ios::binary);
  return string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void test_config() {
  EsSettings es;
  ssmedia::Config c = ssmedia::load_config(es, false);  // Settings.cpp defaults
  CHECK(!c.video && c.swap_ms == 10000 && !c.stretch && !c.custom && c.dir == "/storage/roms/screenshots" && c.filter == ".png,.jpg" && !c.recurse);
  CHECK(c.game_name && c.game_info == "never" && c.marquee && !c.datetime && c.date_format == "%Y-%m-%d" && c.time_format == "%H:%M:%S" && !c.mute);
  c = ssmedia::load_config(es, true);
  CHECK(c.video && c.swap_ms == 30000 && c.filter == ".mp4,.avi" && c.dir == "/storage/roms/screenshots");  // empty video dir = the screenshots folder
  es.set_int("ScreenSaverSwapImageTimeout", 5000);
  es.set_int("ScreenSaverSwapVideoTimeout", 45000);
  es.set_bool("SlideshowScreenSaverStretch", true);
  es.set_bool("StretchVideoOnScreenSaver", false);
  es.set_bool("SlideshowScreenSaverCustomVideoSource", true);
  es.set_string("SlideshowScreenSaverVideoDir", "/media/clips");
  es.set_bool("SlideshowScreenSaverVideoRecurse", true);
  es.set_bool("ScreenSaverVideoMute", true);
  es.set_string("ScreenSaverGameInfo", "start & end");
  c = ssmedia::load_config(es, false);
  CHECK(c.swap_ms == 5000 && c.stretch && !c.custom);  // each type reads its own keys
  c = ssmedia::load_config(es, true);
  CHECK(c.swap_ms == 45000 && !c.stretch && c.custom && c.dir == "/media/clips" && c.recurse && c.mute && c.game_info == "start & end");
  es.set_int("ScreenSaverSwapImageTimeout", 0);  // a stray 0 would swap on every loop
  CHECK(ssmedia::load_config(es, false).swap_ms >= 1000);
}

void test_filter_and_dirs() {
  CHECK(ssmedia::ext_matches("/a/b.png", ".png,.jpg"));
  CHECK(ssmedia::ext_matches("/a/b.jpg", ".png,.jpg"));
  CHECK(!ssmedia::ext_matches("/a/b.gif", ".png,.jpg"));
  CHECK(!ssmedia::ext_matches("/a/b.PNG", ".png,.jpg"));  // ES: case sensitive
  CHECK(ssmedia::ext_matches("/a/b.pn", ".png,.jpg"));    // ES: a substring test (find), kept as it is
  CHECK(ssmedia::ext_matches("/a/b.anything", ""));        // empty filter: every file
  CHECK(!ssmedia::ext_matches("/a/noext", ".png"));

  const string d = g_dir + "/custom";
  sh("rm -rf " + d + " && mkdir -p " + d + "/sub/deeper");
  put(d + "/a.png");
  put(d + "/b.jpg");
  put(d + "/c.txt");
  put(d + "/.hidden.png");
  put(d + "/sub/d.png");
  put(d + "/sub/deeper/e.jpg");
  vector<string> flat = ssmedia::list_custom(d, ".png,.jpg", false);
  CHECK(flat.size() == 2 && flat[0] == d + "/a.png" && flat[1] == d + "/b.jpg");  // sorted, no hidden, no sub-folders
  vector<string> deep = ssmedia::list_custom(d, ".png,.jpg", true);
  CHECK(deep.size() == 4);
  CHECK(ssmedia::list_custom(d, "", false).size() == 3);  // every file (c.txt too), still no hidden
  CHECK(ssmedia::list_custom(d + "/nope", "", true).empty());
  CHECK(ssmedia::list_custom(d + "/a.png", "", true).empty());  // a file is no directory

  ssmedia::Config c;
  c.dir = d;
  c.filter = ".png,.jpg";
  CHECK(ssmedia::pick_custom(c, [](size_t) { return size_t(1); }) == d + "/b.jpg");
  CHECK(ssmedia::pick_custom(c, [](size_t n) { return n + 5; }) == d + "/b.jpg");  // a wild rng stays inside the list
  c.filter = ".webp";
  CHECK(ssmedia::pick_custom(c, [](size_t) { return size_t(0); }).empty());
}

// ---- the games -----------------------------------------------------------------------------------------------------------

class FakeGames : public ssmedia::GameSource {
 public:
  explicit FakeGames(string dir, int n = 3) : dir_(std::move(dir)), n_(n) {}
  size_t systems() const override { return 1; }
  Info info(size_t) const override { return {"snes", "Super Nintendo"}; }
  void entries(size_t, vector<Cand>& out) override {
    for (int i = 0; i < n_; ++i) {
      Cand c;
      c.rom = "r" + std::to_string(i);
      c.name = "g" + std::to_string(i);
      out.push_back(c);
    }
  }
  bool details(size_t, const Cand& c, GameDetails& out) override {
    const string n = c.rom.substr(1);
    out = GameDetails();
    out.image = dir_ + "/img" + n + ".png";
    out.video = dir_ + "/vid" + n + ".mp4";  // vid2 is never created: ES skips a game whose file is missing
    out.marquee = dir_ + "/mq" + n + ".png";
    return true;
  }

 private:
  string dir_;
  int n_;
};

void test_picker() {
  const string d = g_dir + "/games";
  sh("rm -rf " + d + " && mkdir -p " + d);
  for (int i = 0; i < 3; ++i) put(d + "/img" + std::to_string(i) + ".png");
  put(d + "/vid0.mp4");
  put(d + "/vid1.mp4");

  {  // images: every game once, then the list starts again
    ssmedia::Picker p(std::make_unique<FakeGames>(d), [](size_t) { return size_t(0); });
    CHECK(p.remaining(false) == 0);
    vector<string> got;
    ssmedia::Pick pk;
    for (int i = 0; i < 3; ++i) {
      CHECK(p.next(false, pk));
      got.push_back(pk.game);
      CHECK(pk.has_game && pk.system == "snes" && pk.system_full == "Super Nintendo" && pk.path == d + "/img" + pk.game.substr(1) + ".png");
    }
    CHECK(p.remaining(false) == 0);
    std::sort(got.begin(), got.end());
    CHECK(got == vector<string>({"g0", "g1", "g2"}));  // no repeat
    CHECK(p.next(false, pk));                          // used up: the list is rebuilt
    CHECK(p.remaining(false) == 2);
  }
  {  // videos: only games whose file exists; the image list is separate
    ssmedia::Picker p(std::make_unique<FakeGames>(d), [](size_t) { return size_t(0); });
    ssmedia::Pick pk;
    CHECK(p.next(true, pk) && pk.game == "g0" && pk.path == d + "/vid0.mp4");
    CHECK(p.next(true, pk) && pk.game == "g1");  // g2 has no video file: skipped
    CHECK(p.next(true, pk) && pk.game == "g0");  // used up: starts again
    CHECK(p.remaining(false) == 0);              // the picture list was never touched
  }
  {  // a random pick is one of the games, whatever the rng says
    ssmedia::Picker p(std::make_unique<FakeGames>(d), [](size_t n) { return n + 100; });
    ssmedia::Pick pk;
    CHECK(p.next(false, pk));
  }
  {  // nothing to show: false, not a loop
    ssmedia::Picker p(std::make_unique<FakeGames>(d + "/missing"), [](size_t) { return size_t(0); });
    ssmedia::Pick pk;
    CHECK(!p.next(false, pk) && !p.next(true, pk));
    ssmedia::Picker none(std::make_unique<FakeGames>(d, 0), [](size_t) { return size_t(0); });
    CHECK(!none.next(false, pk));
  }

  // which systems count: game systems with a gamelist, not image viewers nor ignored platforms
  vector<System> sys(5);
  sys[0].name = "snes";
  sys[0].fullname = "Super Nintendo";
  sys[0].rom_dir = "/r/snes";
  sys[0].gamelist = "/r/snes/gamelist.xml";
  sys[1].name = "empty";  // no gamelist
  sys[1].rom_dir = "/r/e";
  sys[2].name = "pics";
  sys[2].rom_dir = "/r/p";
  sys[2].gamelist = "/r/p/gamelist.xml";
  sys[2].theme = "imageviewer";
  sys[3].name = "pc";
  sys[3].rom_dir = "/r/pc";
  sys[3].gamelist = "/r/pc/gamelist.xml";
  sys[3].platform = "ignore";
  sys[4].name = "gba";
  sys[4].rom_dir = "/r/gba";
  sys[4].gamelist = "/r/gba/gamelist.xml";
  const vector<ssmedia::SysRef> refs = ssmedia::eligible_systems(sys);
  CHECK(refs.size() == 2 && refs[0].name == "snes" && refs[1].name == "gba");
}

void test_timing() {
  CHECK(ssmedia::fade_alpha(0) == 0.0f && ssmedia::fade_alpha(250) == 0.5f && ssmedia::fade_alpha(500) == 1.0f && ssmedia::fade_alpha(9000) == 1.0f);
  CHECK(ssmedia::fade_alpha(-5) == 0.0f);
  const TimePoint t0 = Clock::now();
  CHECK(!ssmedia::swap_due(t0 + milliseconds(9999), t0, 10000));
  CHECK(ssmedia::swap_due(t0 + milliseconds(10000), t0, 10000));
  CHECK(ssmedia::swap_due(t0 + milliseconds(30001), t0, 30000));

  CHECK(ssmedia::info_alpha("never", 100, 30000) == 0.0f && ssmedia::info_alpha("", 100, 30000) == 0.0f);
  CHECK(ssmedia::info_alpha("always", 15000, 30000) == 1.0f);
  // start & end: 4 s at each end, 150 ms fades
  CHECK(ssmedia::info_alpha("start & end", 0, 30000) == 1.0f);
  CHECK(ssmedia::info_alpha("start & end", 3800, 30000) == 1.0f);
  const float mid = ssmedia::info_alpha("start & end", 3925, 30000);  // halfway through the fade out
  CHECK(mid > 0.45f && mid < 0.55f);
  CHECK(ssmedia::info_alpha("start & end", 4000, 30000) == 0.0f);
  CHECK(ssmedia::info_alpha("start & end", 15000, 30000) == 0.0f);
  CHECK(ssmedia::info_alpha("start & end", 25925, 30000) > 0.0f);  // fading back in
  CHECK(ssmedia::info_alpha("start & end", 26000, 30000) == 1.0f);
  CHECK(ssmedia::info_alpha("start & end", 29999, 30000) == 1.0f);
  CHECK(ssmedia::info_alpha("start & end", 3500, 7000) == 1.0f);  // too short for a gap: it stays
}

// ---- menu rows -----------------------------------------------------------------------------------------------------------

bool has(const vector<Entry>& v, const char* label) {
  for (const Entry& e : v)
    if (e.label == string(tr(label))) return true;
  return false;
}

vector<Entry> rows(Ctx& cx) {
  vector<Entry> v;
  MenuBuilder b(cx, v, nullptr);
  ssmedia::build_rows(b, cx);
  return v;
}

void test_rows() {
  Ctx cx;
  CHECK(rows(cx).empty());  // no type chosen
  cx.es.set_string("ScreenSaverBehavior", "dim");
  CHECK(rows(cx).empty());
  cx.es.set_string("ScreenSaverBehavior", "black");
  CHECK(rows(cx).empty());
  cx.es.set_string("ScreenSaverBehavior", "suspend");
  CHECK(rows(cx).empty());

  cx.es.set_string("ScreenSaverBehavior", "slideshow");
  vector<Entry> v = rows(cx);
  for (const char* l : {"SLIDESHOW SCREENSAVER SETTINGS", "IMAGE DURATION (SECS)", "SHOW GAME INFO", "SHOW DATE TIME", "USE MARQUEE AS GAME INFO", "STRETCH IMAGES", "USE CUSTOM IMAGES"})
    CHECK(has(v, l));
  CHECK(!has(v, "VIDEO DURATION (SECS)") && !has(v, "STRETCH VIDEOS") && !has(v, "MUTE VIDEO AUDIO"));
  CHECK(!has(v, "CUSTOM IMAGE DIRECTORY") && !has(v, "DATE FORMAT"));  // they follow their switches
  cx.es.set_bool("SlideshowScreenSaverCustomImageSource", true);
  cx.es.set_bool("ScreenSaverDateTime", true);
  v = rows(cx);
  for (const char* l : {"CUSTOM IMAGE DIRECTORY", "USE IMAGES IN SUBFOLDERS OF CUSTOM DIRECTORY", "CUSTOM IMAGE FILE EXTENSIONS", "DATE FORMAT", "TIME FORMAT", "USE MARQUEE AS GAME INFO"})
    CHECK(has(v, l));

  Ctx cv;
  cv.es.set_string("ScreenSaverBehavior", "random video");
  v = rows(cv);
  for (const char* l : {"RANDOM VIDEO SCREENSAVER SETTINGS", "VIDEO DURATION (SECS)", "SHOW GAME INFO", "SHOW DATE TIME", "USE MARQUEE AS GAME INFO", "STRETCH VIDEOS", "MUTE VIDEO AUDIO", "USE CUSTOM VIDEOS"})
    CHECK(has(v, l));
  CHECK(!has(v, "IMAGE DURATION (SECS)") && !has(v, "STRETCH IMAGES"));
  cv.es.set_bool("SlideshowScreenSaverCustomVideoSource", true);
  v = rows(cv);
  for (const char* l : {"CUSTOM VIDEO DIRECTORY", "USE VIDEOS IN SUBFOLDERS OF CUSTOM DIRECTORY", "CUSTOM VIDEO FILE EXTENSIONS"})
    CHECK(has(v, l));
  CHECK(!has(v, "USE MARQUEE AS GAME INFO"));  // ES: the marquee only with the games' own videos
}

// ---- off means off -------------------------------------------------------------------------------------------------------

void test_idle_and_fallback() {
  Ctx cx;
  const TimePoint now = Clock::now();
  ssmedia::reset_for_test();
  CHECK(!ssmedia::active() && !ssmedia::poll(cx, now) && !ssmedia::deadline(now).has_value());  // nothing asked: nothing happens
  CHECK(!video::wanted());

  // slideshow asked, but no game system has anything to show: ES shows the plain black screensaver, and the panel goes off
  sh("rm -rf " + g_dir + "/bl && mkdir -p " + g_dir + "/bl/a");
  put(g_dir + "/bl/a/bl_power", "0\n");
  oscontract::paths().backlight_dir = g_dir + "/bl";
  screensaver::set_mode(screensaver::Mode::Media);
  ssmedia::request_start(false);
  CHECK(ssmedia::active());
  CHECK(ssmedia::poll(cx, now));  // begin(): no systems
  CHECK(screensaver::mode() == screensaver::Mode::Black && slurp(g_dir + "/bl/a/bl_power") == "4\n");
  CHECK(!ssmedia::active() && !ssmedia::poll(cx, now) && !video::wanted());
  screensaver::set_mode(screensaver::Mode::Off);
  ssmedia::on_stop();  // idempotent
  ssmedia::reset_for_test();
}

}  // namespace

int main() {
  char tpl[] = "/tmp/zf-ssmedia-XXXXXX";
  if (!::mkdtemp(tpl)) return 2;
  g_dir = tpl;
  test_config();
  test_filter_and_dirs();
  test_picker();
  test_timing();
  test_rows();
  test_idle_and_fallback();
  sh("rm -rf " + g_dir);
  return zt::done();
}
