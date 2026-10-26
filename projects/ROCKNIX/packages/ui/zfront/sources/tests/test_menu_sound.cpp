// menu.sound: entries, the commands and files it produces (dry-run, temporary system.cfg / es_settings.cfg, no audio),
// the volume OSD, the inotify watcher and the pure parsers.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "menu_sound.hpp"
#include "registry.hpp"
#include "ui_osd.hpp"
#include "util.hpp"

using namespace zf;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string g_dir;  // temporary tree of this run

void write_file(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << text;
}
std::string read_all(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct Rig {
  Ctx cx;
  std::string cfg, es, bin;
  explicit Rig(const char* tag, const std::string& cfg_text = "audio.volume=50\n", bool volume_tool = true, bool audio_tool = false) {
    std::string d = g_dir + "/" + tag;
    std::string cmd = "mkdir -p " + d + "/bin";
    CHECK(std::system(cmd.c_str()) == 0);
    cfg = d + "/system.cfg";
    es = d + "/es_settings.cfg";
    bin = d + "/bin/";
    write_file(cfg, cfg_text);
    write_file(es, "<?xml version=\"1.0\"?>\n<config>\n</config>\n");
    if (volume_tool) write_file(bin + "volume", "#!/bin/sh\n");
    if (audio_tool) write_file(bin + "set-audio", "#!/bin/sh\n");
    std::string err;
    CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
    cx.w = 640;
    cx.h = 480;
    cx.sv->cmd.dry_run = true;
    cx.sv->cmd.keep_log = true;
    cx.conf = SystemConf::load(cfg);
    cx.es = EsSettings::load(es);
    SystemConf::lock_path() = d + "/lock";
    sound::env().bin_dir = bin;
    sound::env().fake = false;
    sound::env().lister = nullptr;
    sound::reset_state();
  }
  // job results come back on the main thread: run them until nothing is left
  void settle() {
    for (int i = 0; i < 3000; ++i) {
      bool ran = cx.sv->jobs.drain();
      if (cx.sv->jobs.idle() && !ran && !cx.sv->jobs.drain()) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(!"jobs did not settle");
  }
};

Menu* open_menu(Rig& r, std::unique_ptr<Screen>& hold) {
  hold = Registry::instance().make("menu.sound", r.cx, "");
  CHECK(hold != nullptr);
  Menu* m = static_cast<Menu*>(hold.get());
  m->handle(Action::X, r.cx);  // the first handle builds and runs on_open (X does nothing here)
  r.settle();
  m->tick(0.0f, r.cx);  // applies the refresh the jobs asked for
  return m;
}

int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}

bool log_has(const Rig& r, const std::string& cmd) {
  for (const std::string& c : r.cx.sv->cmd.log)
    if (c == cmd) return true;
  return false;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-sound-XXXXXX";
  CHECK(mkdtemp(tmpl) != nullptr);
  g_dir = tmpl;

  // ---- parsers
  {
    CHECK_EQ(sound::parse_volume("a=1\naudio.volume=60\nb=2\n"), 60);
    CHECK_EQ(sound::parse_volume("a=1\n"), -1);          // set_setting deleted the line and has not appended it yet
    CHECK_EQ(sound::parse_volume("audio.volume=\n"), -1);
    CHECK_EQ(sound::parse_volume("audio.volume=abc\n"), -1);
    CHECK_EQ(sound::parse_volume("audio.volume=250\n"), 100);
    CHECK_EQ(sound::parse_volume("audio.volume=-5\n"), -1);
    CHECK_EQ(sound::parse_volume("# audio.volume=30\naudio.volume=0\n"), 0);

    auto d = sound::parse_audio_list("headphone\tHeadphones\r\nhdmi\tHDMI audio\tdigital\n\nauto\n", false);
    CHECK_EQ(d.size(), size_t(3));
    CHECK_EQ(d[0].value, std::string("headphone"));
    CHECK_EQ(d[0].label, std::string("Headphones"));
    CHECK_EQ(d[1].label, std::string("HDMI audio digital"));
    CHECK_EQ(d[2].value, std::string("auto"));
    CHECK_EQ(d[2].label, std::string("auto"));  // a lone token is both
    auto p = sound::parse_audio_list("a2dp\tbluez_card.AA_BB\tHigh quality\noff\tOff\n", true);
    CHECK_EQ(p.size(), size_t(2));
    CHECK_EQ(p[0].label, std::string("High quality"));  // the card id is dropped for profiles
    CHECK_EQ(p[1].label, std::string("Off"));
    auto q = sound::parse_audio_list("a2dp\tbluez_card.AA_BB\tHigh quality\n", false);
    CHECK_EQ(q[0].label, std::string("bluez_card.AA_BB High quality"));  // ...but not for outputs
    CHECK(sound::parse_audio_list("", false).empty());
  }

  // ---- tracker: only a real change counts; a missing value never does
  {
    sound::VolumeTracker t;
    CHECK_EQ(t.update(40), -1);  // first sight
    t.seed(40);
    CHECK_EQ(t.update(40), -1);
    CHECK_EQ(t.update(45), 45);
    CHECK_EQ(t.update(-1), -1);  // line deleted by set_setting
    CHECK_EQ(t.update(45), -1);  // ...and appended back with the same value
    CHECK_EQ(t.update(50), 50);
    CHECK_EQ(t.update(0), 0);
  }

  // ---- what the watcher does with a changed system.cfg
  {
    sound::VolumeTracker t;
    t.seed(40);
    int v = -1;
    CHECK(sound::decide(t, "audio.volume=40\n", false, false, v) == sound::Seen::Nothing);  // another key changed
    CHECK(sound::decide(t, "x=1\n", false, false, v) == sound::Seen::Nothing);              // set_setting between delete and append
    CHECK(sound::decide(t, "audio.volume=40\n", false, false, v) == sound::Seen::Nothing);  // ...and the same value again
    CHECK(sound::decide(t, "audio.volume=45\n", false, false, v) == sound::Seen::Osd);
    CHECK_EQ(v, 45);
    CHECK(sound::decide(t, "audio.volume=50\n", true, false, v) == sound::Seen::Nothing);   // our own `volume N`
    CHECK(sound::decide(t, "audio.volume=50\n", false, false, v) == sound::Seen::Nothing);  // ...the baseline moved with it
    CHECK(sound::decide(t, "audio.volume=55\n", false, true, v) == sound::Seen::Sync);      // a game was running
    CHECK_EQ(v, 55);
    CHECK(sound::decide(t, "audio.volume=60\n", false, false, v) == sound::Seen::Osd);
  }

  // ---- the tool probe follows the bin dir
  {
    Rig r("probe", "", true, false);
    CHECK(sound::have_tool("volume"));
    CHECK(!sound::have_tool("set-audio"));
    sound::env().fake = true;
    CHECK(sound::have_tool("set-audio"));
    sound::env().fake = false;
  }

  // ---- entries: volume tool only (what the image has today)
  {
    Rig r("entries", "audio.volume=50\nsystem.battery.warning=1\nsystem.battery.warning_threshold=30\n");
    std::unique_ptr<Screen> hold;
    Menu* m = open_menu(r, hold);
    CHECK_EQ(m->title(), std::string("SOUND SETTINGS"));
    CHECK_EQ(m->size(), size_t(6));
    CHECK(m->entry(0).kind == EntryKind::Header);
    CHECK_EQ(m->entry(0).label, std::string("VOLUME"));
    CHECK(m->entry(1).kind == EntryKind::Slider);
    CHECK_EQ(m->entry(1).label, std::string("SYSTEM VOLUME"));
    CHECK_EQ(m->entry(1).i, 50);
    CHECK_EQ(m->entry(1).max, 100);
    CHECK(m->entry(2).kind == EntryKind::Toggle);
    CHECK_EQ(m->entry(2).label, std::string("SHOW OVERLAY WHEN VOLUME CHANGES"));
    CHECK(m->entry(2).b);  // default true
    CHECK_EQ(m->entry(3).label, std::string("SOUNDS"));
    CHECK_EQ(m->entry(4).label, std::string("ENABLE AUDIBLE BATTERY WARNING"));
    CHECK(m->entry(4).b);
    CHECK_EQ(m->entry(5).label, std::string("BATTERY WARNING THRESHOLD"));
    CHECK_EQ(m->entry(5).i, 30);
    CHECK_EQ(m->entry(5).max, 50);
    CHECK_EQ(find(*m, "AUDIO OUTPUT"), -1);    // no set-audio: hidden, as in ES
    CHECK_EQ(find(*m, "MUSIC VOLUME"), -1);    // no music player
    CHECK_EQ(find(*m, "ENABLE NAVIGATION SOUNDS"), -1);
    CHECK_EQ(m->cursor(), 1);
    CHECK(r.cx.sv->cmd.log.empty());           // opening the menu runs nothing
  }

  // ---- no tools at all: only the battery warning is left, with its own header
  {
    Rig r("notools", "", false, false);
    std::unique_ptr<Screen> hold;
    Menu* m = open_menu(r, hold);
    CHECK_EQ(m->size(), size_t(3));
    CHECK_EQ(m->entry(0).label, std::string("SOUNDS"));
    CHECK_EQ(m->entry(2).i, 25);  // ES default when the key is missing
    CHECK(!m->entry(1).b);
  }

  // ---- opening re-reads system.cfg: hardware keys wrote a newer volume than the one zfront loaded at start
  {
    Rig r("stale", "audio.volume=50\n");
    write_file(r.cfg, "audio.volume=80\n");
    std::unique_ptr<Screen> hold;
    Menu* m = open_menu(r, hold);
    CHECK_EQ(m->entry(1).i, 80);
    CHECK_EQ(sound::volume(r.cx), 80);
  }

  // ---- empty audio.volume means 100 (ES: VolumeControl::init)
  {
    Rig r("empty", "");
    std::unique_ptr<Screen> hold;
    Menu* m = open_menu(r, hold);
    CHECK_EQ(m->entry(1).i, 100);
  }

  // ---- the slider: live OSD, one `volume N` at a time, the last command is the final value
  {
    Rig r("slider", "audio.volume=50\n");
    std::unique_ptr<Screen> hold;
    Menu* m = open_menu(r, hold);
    CHECK_EQ(m->cursor(), 1);
    for (int i = 0; i < 5; ++i) m->handle(Action::Right, r.cx);
    CHECK_EQ(m->entry(1).i, 55);
    CHECK(osd_visible());  // VolumePopup defaults to on
    r.settle();
    CHECK(!r.cx.sv->cmd.log.empty());
    CHECK(r.cx.sv->cmd.log.size() <= 5);
    CHECK_EQ(r.cx.sv->cmd.log.back(), std::string("volume 55"));
    for (const std::string& c : r.cx.sv->cmd.log) CHECK(c.rfind("volume ", 0) == 0);
    // the slider keeps its value after the rebuild the framework does after an interaction
    m->tick(0.0f, r.cx);
    CHECK_EQ(m->entry(1).i, 55);
    // going back down also ends on the last value; the same value twice runs nothing
    r.cx.sv->cmd.log.clear();
    m->handle(Action::Left, r.cx);
    m->handle(Action::Left, r.cx);
    r.settle();
    CHECK_EQ(r.cx.sv->cmd.log.back(), std::string("volume 53"));
    r.cx.sv->cmd.log.clear();
    sound::set_volume(r.cx, 53);
    r.settle();
    CHECK(r.cx.sv->cmd.log.empty());
    // clamps
    sound::set_volume(r.cx, 250);
    r.settle();
    CHECK_EQ(r.cx.sv->cmd.log.back(), std::string("volume 100"));
    sound::set_volume(r.cx, -3);
    r.settle();
    CHECK_EQ(r.cx.sv->cmd.log.back(), std::string("volume 0"));
    // the command is the OS script, which writes audio.volume itself: zfront does not touch the file
    CHECK_EQ(read_all(r.cfg), std::string("audio.volume=50\n"));
    // the file watcher must ignore the writes our own runs make
    CHECK(sound::quiet());
  }

  // ---- OSD follows VolumePopup
  {
    Rig r("popup", "audio.volume=50\n");
    r.cx.es.set_bool("VolumePopup", false);
    std::unique_ptr<Screen> hold;
    Menu* m = open_menu(r, hold);
    CHECK(!m->entry(2).b);
    osd_clear();
    m->handle(Action::Right, r.cx);
    CHECK(!osd_visible());
    r.settle();
    CHECK_EQ(r.cx.sv->cmd.log.back(), std::string("volume 51"));  // it still changes the volume
    sound::external_volume(r.cx, 70, true);
    CHECK(!osd_visible());
  }

  // ---- toggles and the threshold land in the right files
  {
    Rig r("files", "audio.volume=50\nother=keep\n");
    std::unique_ptr<Screen> hold;
    Menu* m = open_menu(r, hold);
    m->handle(Action::Down, r.cx);  // overlay
    CHECK_EQ(m->cursor(), 2);
    m->handle(Action::A, r.cx);
    CHECK_EQ(EsSettings::load(r.es).get("VolumePopup") ? *EsSettings::load(r.es).get("VolumePopup") : "", std::string("false"));
    CHECK(read_all(r.es).find("<bool name=\"VolumePopup\" value=\"false\" />") != std::string::npos);
    m->handle(Action::A, r.cx);
    CHECK_EQ(*EsSettings::load(r.es).get("VolumePopup"), std::string("true"));
    m->handle(Action::Down, r.cx);  // battery warning
    CHECK_EQ(m->cursor(), 4);
    m->handle(Action::A, r.cx);
    CHECK(read_all(r.cfg).find("system.battery.warning=1\n") != std::string::npos);
    CHECK(read_all(r.cfg).find("other=keep\n") != std::string::npos);  // foreign lines survive
    m->handle(Action::A, r.cx);
    CHECK(read_all(r.cfg).find("system.battery.warning=0\n") != std::string::npos);
    m->handle(Action::Down, r.cx);  // threshold
    CHECK_EQ(m->cursor(), 5);
    for (int i = 0; i < 3; ++i) m->handle(Action::Right, r.cx);
    CHECK_EQ(m->entry(5).i, 28);
    CHECK(read_all(r.cfg).find("warning_threshold") == std::string::npos);  // debounced: nothing written yet
    Transition t = m->handle(Action::B, r.cx);                              // leaving flushes the pending commit
    CHECK(t.kind == Transition::Pop);
    CHECK(read_all(r.cfg).find("system.battery.warning_threshold=28\n") != std::string::npos);
    CHECK_EQ(r.cx.conf.get_or("system.battery.warning_threshold", ""), std::string("28"));
  }

  // ---- audio output / profile: only with set-audio, lists come from a job, values go to the script and system.cfg
  {
    Rig r("output", "audio.volume=50\naudio.device=headphone\naudio.profile=stereo\n", true, true);
    std::vector<std::string> asked;
    sound::env().lister = [&asked](const std::string& c) -> std::string {
      asked.push_back(c);
      if (c == "set-audio list") return "speaker\tInternal speaker\nheadphone\tHeadphones\nhdmi\tHDMI\n";
      if (c == "set-audio list-profiles") return "auto\tAutomatic\nstereo\tAnalog stereo\n";
      return {};
    };
    std::unique_ptr<Screen> hold;
    Menu* m = open_menu(r, hold);
    CHECK_EQ(asked.size(), size_t(2));
    CHECK_EQ(m->size(), size_t(8));
    int out = find(*m, "AUDIO OUTPUT"), prof = find(*m, "AUDIO PROFILE");
    CHECK_EQ(out, 3);
    CHECK_EQ(prof, 4);
    CHECK(m->entry(out).kind == EntryKind::Choice);
    CHECK_EQ(m->entry(out).options.size(), size_t(3));
    CHECK_EQ(m->entry(out).value_text, std::string("Headphones"));  // audio.device=headphone is selected
    CHECK_EQ(m->entry(prof).value_text, std::string("Analog stereo"));
    CHECK(!m->entry(out).hint_text.size() && m->entry(prof).hint_text.size());
    // pick HDMI
    m->entry(out).set_s("hdmi");
    r.settle();
    CHECK(log_has(r, "set-audio set 'hdmi'"));
    CHECK(read_all(r.cfg).find("audio.device=hdmi\n") != std::string::npos);
    CHECK_EQ(asked.size(), size_t(3));  // the profiles are listed again for the new output
    CHECK_EQ(asked.back(), std::string("set-audio list-profiles"));
    // a profile
    m->entry(prof).set_s("auto");
    r.settle();
    CHECK(log_has(r, "set-audio set-profile 'auto'"));
    CHECK(read_all(r.cfg).find("audio.profile") == std::string::npos);  // "auto" removes the key (ES)
    m->entry(prof).set_s("stereo");
    r.settle();
    CHECK(read_all(r.cfg).find("audio.profile=stereo\n") != std::string::npos);
    // "auto" output keeps its line (ES: audio.device is never removed)
    m->entry(out).set_s("auto");
    r.settle();
    CHECK(read_all(r.cfg).find("audio.device=auto\n") != std::string::npos);
    CHECK(log_has(r, "set-audio set 'auto'"));
    // text from the outside is quoted
    m->entry(out).set_s("a'b; rm -rf x");
    r.settle();
    CHECK(log_has(r, "set-audio set " + shell_quote("a'b; rm -rf x")));
  }

  // ---- change_volume (the helper for the right-stick shortcut of ES)
  {
    Rig r("step", "audio.volume=50\n");
    sound::change_volume(r.cx, +5);
    r.settle();
    CHECK_EQ(r.cx.sv->cmd.log.back(), std::string("volume 55"));
    sound::change_volume(r.cx, -60);
    r.settle();
    CHECK_EQ(r.cx.sv->cmd.log.back(), std::string("volume 0"));
    CHECK(osd_visible());
  }

  // ---- hardware-key change: updates the open menu and shows the OSD only when fresh
  {
    Rig r("external", "audio.volume=50\n");
    std::unique_ptr<Screen> hold;
    Menu* m = open_menu(r, hold);
    osd_clear();
    sound::external_volume(r.cx, 65, false);
    CHECK(!osd_visible());  // a change from long ago (made during a game)
    m->tick(0.0f, r.cx);
    CHECK_EQ(m->entry(1).i, 65);  // the slider follows
    sound::external_volume(r.cx, 70, true);
    CHECK(osd_visible());
    m->tick(0.0f, r.cx);
    CHECK_EQ(m->entry(1).i, 70);
    CHECK(r.cx.sv->cmd.log.empty());  // nothing is run for a change somebody else made
    // the next step starts from the new value
    m->handle(Action::Right, r.cx);
    r.settle();
    CHECK_EQ(r.cx.sv->cmd.log.back(), std::string("volume 71"));
  }

  // ---- quiet window
  {
    sound::quiet_for(-1);
    CHECK(!sound::quiet());
    sound::quiet_for(5000);
    CHECK(sound::quiet());
    sound::quiet_for(0);
    CHECK(!sound::quiet());
  }

  // ---- the watcher sees what set_setting does (rename of a temp file, then an append) and nothing else
  {
    std::string d = g_dir + "/watch";
    CHECK(std::system(("mkdir -p " + d).c_str()) == 0);
    write_file(d + "/system.cfg", "audio.volume=10\n");
    std::atomic<int> hits{0};
    sound::FileWatch w;
    std::string err;
    CHECK(w.start(d, "system.cfg", [&hits] { ++hits; }, err, 30));
    CHECK(w.running());
    auto wait_for = [&](int n) {
      for (int i = 0; i < 400 && hits.load() < n; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    // sed -i: temp file in the same directory, renamed over the target
    write_file(d + "/sedXXXX", "other=1\n");
    CHECK(std::rename((d + "/sedXXXX").c_str(), (d + "/system.cfg").c_str()) == 0);
    {  // >> append
      std::ofstream f(d + "/system.cfg", std::ios::app);
      f << "audio.volume=40\n";
    }
    wait_for(1);
    CHECK_EQ(hits.load(), 1);  // both writes fall in one settle window
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK_EQ(hits.load(), 1);
    CHECK_EQ(sound::parse_volume(read_all(d + "/system.cfg")), 40);
    write_file(d + "/unrelated.txt", "x");  // another file of the directory
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    CHECK_EQ(hits.load(), 1);
    {
      std::ofstream f(d + "/system.cfg", std::ios::app);
      f << "x=1\n";
    }
    wait_for(2);
    CHECK_EQ(hits.load(), 2);
    w.stop();
    CHECK(!w.running());
    w.stop();  // twice is fine
    {
      std::ofstream f(d + "/system.cfg", std::ios::app);
      f << "y=1\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK_EQ(hits.load(), 2);  // stopped
    // a missing directory is an error, not a crash
    sound::FileWatch bad;
    CHECK(!bad.start(d + "/nope", "system.cfg", [] {}, err));
    CHECK(!err.empty());
  }

  // ---- drawing at the four sizes (needs the real theme and font)
  {
    const std::string dir = "/home/epsil/zenith-frontend/theme-dark";
    if (is_dir(dir)) {
      const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
      for (const auto& wh : sizes) {
        Rig r("draw", "audio.volume=50\n", true, true);
        sound::env().lister = [](const std::string& c) -> std::string {
          return c == "set-audio list" ? "speaker\tInternal speaker\nhdmi\tHDMI\n" : "auto\tAutomatic\n";
        };
        std::string err;
        r.cx.w = float(wh[0]);
        r.cx.h = float(wh[1]);
        CHECK(Theme::load(dir, ratio_name(wh[0], wh[1]), r.cx.th, err));
        CHECK(r.cx.assets.load_font(r.cx.th.path(r.cx.th.str("font")), err));
        std::unique_ptr<Screen> hold;
        Menu* m = open_menu(r, hold);
        std::vector<Op> ops;
        m->draw(r.cx, ops);
        CHECK(ops.size() > 20);
        osd_show_level(r.cx, "volume", 0.5f, 3000);
        draw_osd(r.cx, ops);
        CHECK(ops.size() > 30);
      }
    }
  }

  std::string rm = "rm -rf " + g_dir;
  CHECK(std::system(rm.c_str()) == 0);
  return zt::done();
}
