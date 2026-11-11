// Background music (music.cpp): the pure rules of ES's AudioManager (volume curve, folder search and order, per-system folders, the
// recently-played history, song titles), the player against a FAKE backend (no audio device, no SDL_mixer: start, next track on
// "ended", pause for a game, screensaver, duck, per-system change, settings) and the menu rows.
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "music.hpp"
#include "menu_sound.hpp"
#include "registry.hpp"
#include "util.hpp"

using namespace zf;

namespace {

std::string g_dir;

bool file_has(const std::string& path, const std::string& needle) {
  std::string all;
  return read_file(path, all) && all.find(needle) != std::string::npos;
}

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

void sh(const std::string& cmd) { CHECK(std::system(cmd.c_str()) == 0); }

void put(const std::string& path, const std::string& bytes = std::string()) {
  sh("mkdir -p '" + path_dir(path) + "'");
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << bytes;
}

// ---- the fake backend -------------------------------------------------------------------------------------------------------
struct Log {
  int opens = 0, closes = 0, stops = 0;
  bool is_open = false;
  std::vector<std::string> plays;
  std::vector<int> fades, volumes;
  std::string bad;  // paths containing this text cannot be played
  std::function<void()> cb;
  void clear() { *this = Log(); }
};
Log g_log;

class Fake : public music::Backend {
 public:
  bool open() override {
    ++g_log.opens;
    g_log.is_open = true;
    return true;
  }
  void close() override {
    if (!g_log.is_open) return;
    ++g_log.closes;
    g_log.is_open = false;
    g_log.cb = nullptr;
  }
  bool play(const std::string& path, int fade_ms) override {
    g_log.plays.push_back(path);
    g_log.fades.push_back(fade_ms);
    return g_log.bad.empty() || path.find(g_log.bad) == std::string::npos;
  }
  void stop() override { ++g_log.stops; }
  void set_volume(int v) override { g_log.volumes.push_back(v); }
  void set_on_finished(std::function<void()> cb) override { g_log.cb = std::move(cb); }
};

void settle(Ctx& cx) {
  for (int i = 0; i < 3000; ++i) {
    bool ran = cx.sv->jobs.drain();
    if (cx.sv->jobs.idle() && !ran && !cx.sv->jobs.drain()) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(!"jobs did not settle");
}

// the audio thread says "track ended"
void track_ends(Ctx& cx) {
  CHECK(g_log.cb != nullptr);
  if (g_log.cb) g_log.cb();
  settle(cx);
}

std::vector<size_t> g_seq;
size_t g_seq_at = 0;
size_t scripted(size_t n) {
  size_t v = g_seq_at < g_seq.size() ? g_seq[g_seq_at++] : 0;
  return v % n;
}
void script(std::vector<size_t> v) {
  g_seq = std::move(v);
  g_seq_at = 0;
  music::set_random_for_tests(scripted);
}

std::string root_for(const char* tag) { return g_dir + "/" + tag; }
const char* kMusic = "/storage/roms/music/";

struct Rig {
  Ctx cx;
  std::string root;
  explicit Rig(const char* tag, const std::string& es_xml = "<?xml version=\"1.0\"?>\n<config>\n</config>\n") {
    root = root_for(tag);
    sh("rm -rf '" + root + "'; mkdir -p '" + root + "'");
    put(root + "/es_settings.cfg", es_xml);
    cx.es = EsSettings::load(root + "/es_settings.cfg");
    cx.sv->cmd.dry_run = true;
    g_log.clear();
    music::reset_for_tests();
    music::set_backend(std::make_unique<Fake>());
    music::set_root_for_tests(root);
  }
  ~Rig() { music::reset_for_tests(); }
  void tracks(const std::vector<std::string>& rel) {
    for (const std::string& r : rel) put(root + kMusic + r);
  }
  std::string path(const std::string& rel) const { return root + kMusic + rel; }
  void systems(const std::vector<std::string>& names) {
    for (const std::string& n : names) {
      System s;
      s.name = n;
      s.theme = n;
      cx.systems.push_back(s);
      cx.items.push_back(SystemItem{n, n, 1});
    }
  }
  size_t toasts() const { return cx.sv->toasts.items().size(); }
};

// ---- bytes of tagged files --------------------------------------------------------------------------------------------------------
std::string be32(uint32_t v) {
  std::string s(4, '\0');
  s[0] = char(v >> 24), s[1] = char(v >> 16), s[2] = char(v >> 8), s[3] = char(v);
  return s;
}
std::string synch(uint32_t v) {
  std::string s(4, '\0');
  s[0] = char((v >> 21) & 0x7f), s[1] = char((v >> 14) & 0x7f), s[2] = char((v >> 7) & 0x7f), s[3] = char(v & 0x7f);
  return s;
}
std::string frame(const std::string& id, const std::string& body, bool v24) {
  return id + (v24 ? synch(uint32_t(body.size())) : be32(uint32_t(body.size()))) + std::string(2, '\0') + body;
}
std::string tag(const std::string& frames, char ver) {
  return std::string("ID3") + ver + std::string(1, '\0') + std::string(1, '\0') + synch(uint32_t(frames.size())) + frames;
}
std::string padded(const std::string& s, size_t n) { return s + std::string(n - s.size(), '\0'); }

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-music-XXXXXX";
  CHECK(mkdtemp(tmpl) != nullptr);
  g_dir = tmpl;

  // ---- ES defaults of every key and the volume curve
  {
    music::Config c = music::read_config(EsSettings::from_text("<config/>"));
    CHECK(c.enabled && !c.per_system && c.theme_music && c.titles && c.lowers && c.video_audio && !c.stop_on_saver);
    CHECK_EQ(c.title_secs, 10);
    CHECK_EQ(c.volume, 128L);
    music::Config d = music::read_config(EsSettings::from_text(
        "<config><bool name=\"audio.bgmusic\" value=\"false\" /><bool name=\"audio.persystem\" value=\"true\" />"
        "<bool name=\"StopMusicOnScreenSaver\" value=\"true\" /><int name=\"audio.display_titles_time\" value=\"30\" />"
        "<int name=\"MusicVolume\" value=\"60\" /><bool name=\"VideoLowersMusic\" value=\"false\" /></config>"));
    CHECK(!d.enabled && d.per_system && d.stop_on_saver && !d.lowers);
    CHECK_EQ(d.title_secs, 30);
    CHECK_EQ(d.volume, 60L);

    CHECK_EQ(music::max_volume(0), 0);
    CHECK_EQ(music::max_volume(100), 128);
    CHECK_EQ(music::max_volume(128), 128);  // ES's stored default is above the slider's range
    CHECK_EQ(music::max_volume(50), 7);     // 10^(-50/40) * 128
    CHECK_EQ(music::max_volume(80), 40);
    CHECK_EQ(music::ducked_volume(128), 6);
    CHECK_EQ(music::ducked_volume(7), 1);   // never silent while there is music
    CHECK_EQ(music::ducked_volume(0), 0);
    CHECK_EQ(music::title_seconds(2), 10);
    CHECK_EQ(music::title_seconds(0), 10);
    CHECK_EQ(music::title_seconds(121), 10);
    CHECK_EQ(music::title_seconds(3), 3);
    CHECK_EQ(music::title_seconds(120), 120);
  }

  // ---- audio extensions
  {
    for (const char* f : {"a.mp3", "A.MP3", "b.ogg", "c.wav", "d.flac", "e.mod", "f.xm", "g.stm", "h.s3m", "i.far", "j.it", "k.669", "l.mtm", "m.opus"})
      CHECK(music::is_audio_file(f));
    for (const char* f : {"a.txt", "mp3", "a.mp3.bak", "cover.jpg", ""})
      CHECK(!music::is_audio_file(f));
  }

  // ---- folder walk: all folders, or only those named like the system (getMusicIn)
  {
    const std::string r = g_dir + "/walk";
    put(r + "/b.ogg");
    put(r + "/a.mp3");
    put(r + "/notes.txt");
    put(r + "/.hidden.mp3");
    put(r + "/snes/s1.mp3");
    put(r + "/snes/deep/s2.flac");
    put(r + "/nes/n1.wav");
    std::vector<std::string> all;
    music::files_in(r, false, "", all);
    CHECK_EQ(all.size(), size_t(5));
    CHECK_EQ(all[0], r + "/a.mp3");  // files and folders in name order: a.mp3, b.ogg, nes/, snes/
    CHECK_EQ(all[1], r + "/b.ogg");
    CHECK_EQ(all[2], r + "/nes/n1.wav");
    std::vector<std::string> sn;
    music::files_in(r, true, "snes", sn);  // top-level files always count (ES), the "nes" folder does not, "snes/deep" is not named snes
    CHECK_EQ(sn.size(), size_t(3));
    CHECK_EQ(sn[0], r + "/a.mp3");
    CHECK_EQ(sn[1], r + "/b.ogg");
    CHECK_EQ(sn[2], r + "/snes/s1.mp3");
    std::vector<std::string> none;
    music::files_in(g_dir + "/no-such-folder", false, "", none);
    CHECK(none.empty());
  }

  // ---- search order: theme folder, /storage/roms/music, the ES music folder; the first with files wins
  {
    music::Config c;
    std::vector<std::string> d = music::search_dirs(c, "/theme/music");
    CHECK_EQ(d.size(), size_t(3));
    CHECK_EQ(d[0], std::string("/theme/music"));
    CHECK_EQ(d[1], std::string("/storage/roms/music"));
    CHECK_EQ(d[2], std::string("/storage/.config/emulationstation/music"));
    CHECK_EQ(music::search_dirs(c, "").size(), size_t(2));
    c.per_system = true;  // changePlaylist: the theme folder only counts without per-system
    CHECK_EQ(music::search_dirs(c, "/theme/music").size(), size_t(2));
    c.per_system = false;
    c.theme_music = false;
    CHECK_EQ(music::search_dirs(c, "/theme/music").size(), size_t(2));

    const std::string r = g_dir + "/order";
    put(r + "/one/x.mp3");
    put(r + "/two/y.mp3");
    CHECK_EQ(music::find_files({r + "/empty", r + "/one", r + "/two"}, false, "").size(), size_t(1));
    CHECK_EQ(music::find_files({r + "/empty", r + "/one", r + "/two"}, false, "")[0], r + "/one/x.mp3");
    CHECK_EQ(music::find_files({r + "/two", r + "/one"}, false, "")[0], r + "/two/y.mp3");
    CHECK(music::find_files({r + "/empty"}, false, "").empty());
  }

  // ---- the recently-played history and the pick
  {
    music::Recent r;
    r.add("a", 2);  // floor(2 * 0.4) = 0: nothing is remembered
    CHECK_EQ(r.size(), size_t(0));
    for (const char* s : {"a", "b", "c", "d"}) r.add(s, 5);  // keeps floor(5 * 0.4) = 2 and the new one, as ES does
    CHECK_EQ(r.size(), size_t(3));
    CHECK(r.has("d") && r.has("c") && r.has("b"));
    CHECK(!r.has("a"));

    const std::vector<std::string> f = {"a", "b", "c", "d", "e"};
    music::Recent h;
    h.add("a", 5);
    h.add("b", 5);
    size_t calls = 0;
    std::vector<size_t> seq = {0, 1, 3};  // a (recent), b (recent), d
    auto rnd = [&](size_t n) {
      CHECK_EQ(n, size_t(5));
      return seq[calls++];
    };
    CHECK_EQ(music::pick_index(f, h, rnd), size_t(3));
    CHECK_EQ(calls, size_t(3));
    calls = 0;
    seq = {2};  // not recent: one draw
    CHECK_EQ(music::pick_index(f, h, rnd), size_t(2));
    CHECK_EQ(calls, size_t(1));
    // one track: no retries even though it is "recent"
    music::Recent h2;
    for (int i = 0; i < 6; ++i) h2.add(std::string(1, char('a' + i)), 15);
    const std::vector<std::string> one = {"a"};
    calls = 0;
    seq = {0};
    auto rnd1 = [&](size_t) { return seq[calls++]; };
    CHECK_EQ(music::pick_index(one, h2, rnd1), size_t(0));
    CHECK_EQ(calls, size_t(1));
    // two tracks, both recent: size-1 = 1 retry, then the last draw is played
    music::Recent h3;
    h3.add("a", 5);
    h3.add("b", 5);
    const std::vector<std::string> two = {"a", "b"};
    calls = 0;
    seq = {0, 1, 0};
    auto rnd2 = [&](size_t) { return seq[calls++]; };
    CHECK_EQ(music::pick_index(two, h3, rnd2), size_t(1));
    CHECK_EQ(calls, size_t(2));
  }

  // ---- song titles
  {
    const std::string d = g_dir + "/titles/";
    put(d + "plain.ogg");
    CHECK_EQ(music::song_title(d + "plain.ogg"), std::string("plain"));
    put(d + "My.Song.Name.flac");
    CHECK_EQ(music::song_title(d + "My.Song.Name.flac"), std::string("My.Song.Name"));
    put(d + "none.mp3");
    CHECK_EQ(music::song_title(d + "none.mp3"), std::string("none"));  // no tag at all: the file name

    // ID3v2.3: ISO-8859-1 title + UTF-16 (BOM) artist
    put(d + "v23.mp3", tag(frame("TIT2", std::string(1, '\0') + "Caf\xE9", false) +
                               frame("TPE1", std::string("\x01\xFF\xFE", 3) + std::string("A\0b\0\0\0", 6), false),
                           3) + "audio");
    CHECK_EQ(music::song_title(d + "v23.mp3"), std::string("Caf\xC3\xA9 - Ab"));
    // ID3v2.4: UTF-8 title (synchsafe sizes), no artist
    put(d + "v24.mp3", tag(frame("TIT2", std::string("\x03", 1) + "Tema \xC3\xB1" + "u", true), 4) + "audio");
    CHECK_EQ(music::song_title(d + "v24.mp3"), std::string("Tema \xC3\xB1u"));
    // a tag without a title frame falls back to the name
    put(d + "notitle.mp3", tag(frame("TALB", std::string(1, '\0') + "Album", false), 3) + "audio");
    CHECK_EQ(music::song_title(d + "notitle.mp3"), std::string("notitle"));
    // ID3v1 at the end
    put(d + "v1.mp3", std::string(300, 'x') + "TAG" + padded("Old Title", 30) + padded("Old Band", 30) + std::string(65, '\0'));
    CHECK_EQ(music::song_title(d + "v1.mp3"), std::string("Old Title - Old Band"));
    // tracker modules: the title field of the header
    put(d + "chip.mod", padded("Chip Tune", 20) + std::string(100, '\0'));
    CHECK_EQ(music::song_title(d + "chip.mod"), std::string("Chip Tune"));
    put(d + "x.xm", std::string(17, 'E') + padded("XM Song", 20) + std::string(40, '\0'));
    CHECK_EQ(music::song_title(d + "x.xm"), std::string("XM Song"));
    put(d + "empty.mod", std::string(120, '\0'));
    CHECK_EQ(music::song_title(d + "empty.mod"), std::string("empty"));  // blank title: the file name
  }

  // ---- the player, fake backend
  {  // starts a random track, fading in 1 s, at full volume, with the title toast (10 s)
    Rig r("start");
    r.tracks({"a.mp3", "b.mp3", "c.mp3"});
    script({1});
    music::start(r.cx);
    CHECK_EQ(g_log.opens, 1);
    CHECK_EQ(g_log.plays.size(), size_t(1));
    CHECK_EQ(g_log.plays[0], r.path("b.mp3"));
    CHECK_EQ(g_log.fades[0], 1000);
    CHECK_EQ(music::current_track(), r.path("b.mp3"));
    CHECK(!g_log.volumes.empty() && g_log.volumes.back() == 128);
    CHECK_EQ(r.toasts(), size_t(1));
    CHECK_EQ(r.cx.sv->toasts.items().back().text, std::string("b"));

    // the track ends: another one, never the one just played (a is recent only after it played; here b is)
    script({1, 1, 2});
    track_ends(r.cx);
    CHECK_EQ(g_log.plays.size(), size_t(2));
    CHECK_EQ(g_log.plays[1], r.path("c.mp3"));
    CHECK_EQ(g_log.opens, 1);  // the device stays open between tracks
    CHECK_EQ(r.toasts(), size_t(2));

    // a game: the device is released, nothing plays; afterwards a new track
    // the audio thread reports "ended" and the main thread stops before the job runs: that job must do nothing
    CHECK(g_log.cb != nullptr);
    g_log.cb();
    music::pause_for_game();
    CHECK(!g_log.is_open);
    CHECK(music::current_track().empty());
    music::on_system(r.cx, 0);  // nothing happens while a game runs
    music::next();
    CHECK(!g_log.is_open);
    music::resume_after_game();
    CHECK(g_log.is_open);
    CHECK_EQ(g_log.opens, 2);
    CHECK(!music::current_track().empty());

    // the late "ended" of the track that was stopped is ignored (the new track is the one playing)
    const size_t n = g_log.plays.size();
    settle(r.cx);
    CHECK_EQ(g_log.plays.size(), n);
    music::shutdown();
    CHECK(!g_log.is_open);
    CHECK_EQ(g_log.plays.size(), n);
  }

  {  // off: nothing opens; switching it on starts; off stops
    Rig r("off", "<config><bool name=\"audio.bgmusic\" value=\"false\" /></config>");
    r.tracks({"a.mp3"});
    music::start(r.cx);
    CHECK_EQ(g_log.opens, 0);
    r.cx.es.set_bool("audio.bgmusic", true);
    music::apply_settings(true);
    CHECK_EQ(g_log.opens, 1);
    CHECK_EQ(g_log.plays.size(), size_t(1));
    r.cx.es.set_bool("audio.bgmusic", false);
    music::apply_settings(true);
    CHECK(!g_log.is_open);
    music::next();  // disabled: stays silent
    CHECK(!g_log.is_open);
  }

  {  // no music files at all: the device is never opened (idle cost zero)
    Rig r("empty");
    sh("mkdir -p '" + r.root + kMusic + "'");
    music::start(r.cx);
    CHECK_EQ(g_log.opens, 0);
    CHECK(music::current_track().empty());
  }

  {  // a track that cannot be played is skipped (ES asks again among the rest)
    Rig r("skip");  // the fake fails every path with "bad" in it: not in the folder name
    r.tracks({"bad.mp3", "good.mp3"});
    g_log.bad = "bad";
    script({0});
    music::start(r.cx);
    CHECK_EQ(g_log.plays.size(), size_t(2));
    CHECK_EQ(g_log.plays[0], r.path("bad.mp3"));
    CHECK_EQ(music::current_track(), r.path("good.mp3"));
    Rig r2("skip2");
    r2.tracks({"bad1.mp3", "bad2.mp3"});
    g_log.bad = "bad";
    music::start(r2.cx);  // nothing plays: the device is released again
    CHECK(!g_log.is_open);
    CHECK(music::current_track().empty());
  }

  {  // the folders: ES's search order picks /storage/roms/music before the ES folder
    Rig r("order2");
    put(r.root + "/storage/.config/emulationstation/music/es.mp3");
    music::start(r.cx);
    CHECK_EQ(music::current_track(), r.root + "/storage/.config/emulationstation/music/es.mp3");
    music::reset_for_tests();
    Rig r2("order3");
    put(r2.root + "/storage/.config/emulationstation/music/es.mp3");
    r2.tracks({"u.mp3"});
    music::start(r2.cx);
    CHECK_EQ(music::current_track(), r2.path("u.mp3"));
    music::reset_for_tests();
    Rig r3("order4");  // the theme folder comes first
    put(r3.root + "/theme/music/t.mp3");
    r3.tracks({"u.mp3"});
    music::set_theme_dir(r3.root + "/theme/music");
    music::start(r3.cx);
    CHECK_EQ(music::current_track(), r3.root + "/theme/music/t.mp3");
  }

  {  // per-system folders: a system change brings a track of that system's folder
    Rig r("persys", "<config><bool name=\"audio.persystem\" value=\"true\" /></config>");
    r.tracks({"snes/s.mp3", "nes/n.mp3"});
    r.systems({"nes", "snes"});
    music::start(r.cx);
    CHECK_EQ(g_log.opens, 0);  // no system known yet: no folder matches, nothing opens
    music::on_system(r.cx, 1);
    CHECK_EQ(music::current_track(), r.path("snes/s.mp3"));
    music::on_system(r.cx, 1);  // same system: the track goes on
    CHECK_EQ(g_log.plays.size(), size_t(1));
    music::on_system(r.cx, 0);
    CHECK_EQ(music::current_track(), r.path("nes/n.mp3"));
    CHECK_EQ(g_log.plays.size(), size_t(2));
    r.cx.es.set_bool("audio.persystem", false);  // without it a system change keeps the music
    music::on_system(r.cx, 1);
    CHECK_EQ(g_log.plays.size(), size_t(2));
    CHECK_EQ(music::current_track(), r.path("nes/n.mp3"));
    music::on_system(r.cx, -1);
    music::on_system(r.cx, 7);  // out of range: ignored
    CHECK_EQ(g_log.plays.size(), size_t(2));
  }

  {  // screensaver: by default the music goes on; with StopMusicOnScreenSaver it stops and comes back
    Rig r("saver");
    r.tracks({"a.mp3"});
    music::start(r.cx);
    music::on_screensaver(true);
    CHECK(g_log.is_open);
    music::on_screensaver(false);
    CHECK_EQ(g_log.plays.size(), size_t(1));
    r.cx.es.set_bool("StopMusicOnScreenSaver", true);
    music::on_screensaver(true);
    CHECK(!g_log.is_open);
    music::next();  // a menu action under the screensaver does not start it
    CHECK(!g_log.is_open);
    music::on_screensaver(false);
    CHECK(g_log.is_open);
    CHECK_EQ(g_log.plays.size(), size_t(2));
    music::on_screensaver(false);  // a second stop does nothing
    CHECK_EQ(g_log.plays.size(), size_t(2));
  }

  {  // volume: MusicVolume curve, live changes, duck for videos
    Rig r("vol", "<config><int name=\"MusicVolume\" value=\"50\" /></config>");
    r.tracks({"a.mp3"});
    music::start(r.cx);
    CHECK_EQ(g_log.volumes.back(), 7);
    r.cx.es.set_int("MusicVolume", 100);
    music::apply_settings(false);
    CHECK_EQ(g_log.volumes.back(), 128);
    music::duck(true);
    CHECK_EQ(g_log.volumes.back(), 6);
    music::duck(false);
    CHECK_EQ(g_log.volumes.back(), 128);
    r.cx.es.set_bool("VideoLowersMusic", false);
    music::duck(true);
    CHECK_EQ(g_log.volumes.back(), 128);  // VideoLowersMusic off: no change
    music::duck(false);
    r.cx.es.set_bool("VideoLowersMusic", true);
    r.cx.es.set_bool("VideoAudio", false);  // a silent video does not lower it either
    music::duck(true);
    CHECK_EQ(g_log.volumes.back(), 128);
    music::duck(false);
    music::duck(true);  // no music playing: harmless
    music::pause_for_game();
    music::duck(false);
    CHECK(!g_log.is_open);
  }

  {  // titles can be turned off
    Rig r("notitle", "<config><bool name=\"audio.display_titles\" value=\"false\" /></config>");
    r.tracks({"a.mp3"});
    music::start(r.cx);
    CHECK_EQ(g_log.plays.size(), size_t(1));
    CHECK_EQ(r.toasts(), size_t(0));
  }

  // ---- menu rows
  {
    Rig r("menu");
    sh("mkdir -p '" + r.root + "/bin'");
    sound::env().bin_dir = r.root + "/bin/";
    sound::env().fake = false;
    std::string terr;
    CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", r.cx.th, terr));
    r.cx.w = 640;
    r.cx.h = 480;
    CHECK(music::available());
    std::unique_ptr<Screen> hold = Registry::instance().make("menu.sound", r.cx, "");
    CHECK(hold != nullptr);
    if (hold) {
      Menu* m = static_cast<Menu*>(hold.get());
      m->rebuild(r.cx);
      auto find = [&](const char* label) {
        for (size_t i = 0; i < m->size(); ++i)
          if (m->entry(i).label == label) return int(i);
        return -1;
      };
      const int h = find("MUSIC");
      CHECK(h >= 0);
      if (h >= 0) {  // ES order, ES labels, ES defaults
        const char* order[] = {"FRONTEND MUSIC", "MUSIC VOLUME", "DISPLAY SONG TITLES", "SONG TITLE DISPLAY DURATION",
                               "ONLY PLAY SYSTEM-SPECIFIC MUSIC FOLDER", "PLAY SYSTEM-SPECIFIC MUSIC", "LOWER MUSIC WHEN PLAYING VIDEO"};
        CHECK(m->entry(size_t(h)).kind == EntryKind::Header);
        for (int i = 0; i < 7; ++i) CHECK_EQ(m->entry(size_t(h + 1 + i)).label, std::string(order[i]));
        const Entry& fm = m->entry(size_t(h + 1));
        CHECK(fm.kind == EntryKind::Toggle && fm.get_b());
        const Entry& mv = m->entry(size_t(h + 2));
        CHECK(mv.kind == EntryKind::Slider);
        CHECK_EQ(mv.get_i(), 100);  // stored 128 (default) shown at the end of the slider
        CHECK_EQ(mv.min, 0);
        CHECK_EQ(mv.max, 100);
        CHECK(m->entry(size_t(h + 3)).get_b());
        const Entry& dur = m->entry(size_t(h + 4));
        CHECK(dur.kind == EntryKind::Slider);
        CHECK_EQ(dur.get_i(), 10);
        CHECK_EQ(dur.min, 2);
        CHECK_EQ(dur.max, 120);
        CHECK_EQ(dur.step, 2);
        CHECK(!m->entry(size_t(h + 5)).get_b());
        CHECK(m->entry(size_t(h + 6)).get_b());
        CHECK(m->entry(size_t(h + 7)).get_b());

        // the keys are ES's and the change reaches the file
        m->entry(size_t(h + 5)).set_b(true);
        CHECK(r.cx.es.get_bool("audio.persystem", false));
        CHECK(file_has(r.root + "/es_settings.cfg", "audio.persystem"));
        m->entry(size_t(h + 2)).set_i(50);
        CHECK_EQ(r.cx.es.get_int("MusicVolume", 0), 50L);
        m->entry(size_t(h + 1)).set_b(false);
        CHECK(!r.cx.es.get_bool("audio.bgmusic", true));
      }
    }
    // screensaver row: only with frontend music on
    r.cx.es.set_bool("audio.bgmusic", true);
    std::unique_ptr<Screen> ui = Registry::instance().make("menu.ui", r.cx, "");
    CHECK(ui != nullptr);
    if (ui) {
      Menu* m = static_cast<Menu*>(ui.get());
      m->rebuild(r.cx);
      int at = -1;
      for (size_t i = 0; i < m->size(); ++i)
        if (m->entry(i).label == "STOP MUSIC ON SCREENSAVER") at = int(i);
      CHECK(at >= 0);
      if (at >= 0) {
        CHECK(!m->entry(size_t(at)).get_b());  // default off
        m->entry(size_t(at)).set_b(true);
        CHECK(r.cx.es.get_bool("StopMusicOnScreenSaver", false));
      }
      r.cx.es.set_bool("audio.bgmusic", false);
      m->rebuild(r.cx);
      for (size_t i = 0; i < m->size(); ++i) CHECK(m->entry(i).label != "STOP MUSIC ON SCREENSAVER");
    }
    // without the library (no backend) the rows are not offered
    music::set_backend(nullptr);
    CHECK(!music::available());
    std::unique_ptr<Screen> s2 = Registry::instance().make("menu.sound", r.cx, "");
    if (s2) {
      Menu* m = static_cast<Menu*>(s2.get());
      m->rebuild(r.cx);
      for (size_t i = 0; i < m->size(); ++i) CHECK(m->entry(i).label != "FRONTEND MUSIC" && m->entry(i).label != "MUSIC");
    }
  }

  sh("rm -rf '" + g_dir + "'");
  return zt::done();
}
