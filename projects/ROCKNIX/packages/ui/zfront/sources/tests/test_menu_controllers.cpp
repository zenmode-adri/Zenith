// Controller section: es_input.cfg writer, mapping wizard, pad capture hook, Bluetooth parsers and the screens, all against
// temporary files and the dry-run executor (nothing touches /storage, /run or the network). With ZF_CTL_PNG_DIR set it also
// writes review PNGs of every screen/state at the four resolutions in dark and light.
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <new>
#include <pugixml.hpp>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "alloc_nothrow.hpp"
#include "cpu.hpp"
#include "menu.hpp"
#include "menu_controllers.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

using namespace zf;
using namespace zf::ctl;

// ---- allocation counter (this binary only): the steady frame of the custom screens must not allocate
static thread_local size_t g_allocs = 0;
void* operator new(size_t n) {
  ++g_allocs;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

void make_ctx(Ctx& cx, float w = 640, float h = 480) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = w;
  cx.h = h;
  cx.sv->cmd.dry_run = true;
}

std::string tmpdir() {
  static int n = 0;
  char b[128];
  std::snprintf(b, sizeof b, "/tmp/zf-ctl-test-%d-%d", int(::getpid()), n++);
  std::string cmd = std::string("rm -rf ") + b + " && mkdir -p " + b;
  CHECK(std::system(cmd.c_str()) == 0);
  return b;
}

void put(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
}
std::string slurp(const std::string& path) {
  std::string s;
  read_file(path, s);
  return s;
}

bool has_cmd(Ctx& cx, const std::string& needle) {
  for (const std::string& c : cx.sv->cmd.log)
    if (c.find(needle) != std::string::npos) return true;
  return false;
}

void drain_jobs(Ctx& cx) {
  for (int i = 0; i < 500; ++i) {
    cx.sv->jobs.drain();
    if (cx.sv->jobs.idle()) {
      cx.sv->jobs.drain();
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

const char* kGuid = "19009b4d4b4800000111000000010000";
const char* kName = "retrogame_joypad";

// a pad whose "SDL view" the page reads
struct FakePad : PadCore {
  FakePad() : PadCore(PadConfig(), true) {}
  std::vector<JoyInfo> list;
  int reloads = 0;
  std::string reload_from;
  std::vector<JoyInfo> joysticks() const override { return list; }
  void reload_config() override {
    ++reloads;
    PadConfig c;
    c.load_file(reload_from);
    set_config(std::move(c));
  }
};

Transition press(Screen& s, Ctx& cx, Action a) { return s.handle(a, cx); }

// sends a button press + release through the wizard sink
void tap_button(ConfigScreen& s, uint16_t id) {
  std::string g = kGuid, n = kName;
  s.on_capture({0, InKind::Button, id, 1, &g, &n});
  s.on_capture({0, InKind::Button, id, 0, &g, &n});
}

}  // namespace

int main() {
  std::string dir = tmpdir();
  paths().user_input = dir + "/es_input.cfg";
  paths().bt_tool = dir + "/rocknix-bluetooth";
  paths().bt_live_log = dir + "/bt-live.log";
  paths().sysfs_platform = dir + "/platform";
  SystemConf::lock_path() = dir + "/.system.cfg.lock";

  // ---- es_input.cfg writer: edits the DOM, keeps everything else
  {
    put(paths().user_input,
        "<?xml version=\"1.0\"?>\n<inputList>\n"
        "<inputConfig type=\"keyboard\" deviceName=\"Keyboard\" deviceGUID=\"-1\"><input name=\"a\" type=\"key\" id=\"120\" value=\"1\"/></inputConfig>\n"
        "<inputConfig type=\"joystick\" deviceName=\"Other Pad\" deviceGUID=\"0300aaaa\"><input name=\"b\" type=\"button\" id=\"3\" value=\"1\"/></inputConfig>\n"
        "<inputConfig type=\"joystick\" deviceName=\"retrogame_joypad\" deviceGUID=\"19009B4D4B4800000111000000010000\"><input name=\"b\" type=\"button\" id=\"99\" value=\"1\"/></inputConfig>\n"
        "<inputAction type=\"onfinish\"><command>echo done</command></inputAction>\n"
        "</inputList>\n");
    std::vector<Mapping> maps;
    maps.push_back({Mapping::Button, "start", 9, 1});
    maps.push_back({Mapping::Axis, "leftanalogup", 1, -1});
    maps.push_back({Mapping::Hat, "up", 0, 1});
    maps.push_back({Mapping::Key, "hotkeyenable", 0, 1});
    std::string err;
    CHECK(write_device_config(paths().user_input, kGuid, kName, maps, err));
    std::string out = slurp(paths().user_input);
    CHECK(out.find("type=\"keyboard\"") != std::string::npos);
    CHECK(out.find("Other Pad") != std::string::npos);
    CHECK(out.find("id=\"99\"") == std::string::npos);  // the old entry of the same GUID + name is gone
    CHECK(out.find("<?xml version=\"1.0\"?>") == 0);
    std::vector<std::string> cmds = onfinish_commands(paths().user_input);
    CHECK_EQ(cmds.size(), size_t(1));
    CHECK(cmds[0] == "echo done");
    // re-read with the pad's own parser: found, sorted by name (ES writes a std::map), attributes in ES order
    PadConfig pc;
    CHECK(pc.load_file(paths().user_input));
    int i = pc.find(kGuid, kName);
    CHECK(i >= 0);
    if (i >= 0) {
      const auto& d = pc.device(size_t(i));
      CHECK_EQ(d.inputs.size(), size_t(3));  // the `key` marker is not a joystick input
      CHECK(d.inputs[0].name == "leftanalogup");
      CHECK(d.inputs[1].name == "start");
      CHECK(d.inputs[2].name == "up");
      CHECK(d.inputs[0].kind == InKind::Axis && d.inputs[0].value == -1);
    }
    CHECK(pc.find("0300aaaa", "Other Pad") >= 0);
    std::vector<Mapping> back = read_device_config(paths().user_input, kGuid, kName);
    CHECK_EQ(back.size(), size_t(4));
    CHECK(back.size() == 4 && back[0].name == "hotkeyenable" && back[0].type == Mapping::Key && back[0].id == 0);
    size_t p1 = out.find("deviceName=\"retrogame_joypad\"");
    CHECK(p1 != std::string::npos && out.find("deviceGUID", p1) > p1 && out.find("type=\"joystick\"") < p1);
    // es_last_input.cfg holds this device alone
    std::string last = slurp(dir + "/es_last_input.cfg");
    CHECK(last.find("retrogame_joypad") != std::string::npos && last.find("Other Pad") == std::string::npos);
    // writing the same pad again replaces it (no duplicates)
    CHECK(write_device_config(paths().user_input, kGuid, kName, {{Mapping::Button, "a", 1, 1}}, err));
    PadConfig p2;
    p2.load_file(paths().user_input);
    int count = 0;
    for (size_t k = 0; k < p2.size(); ++k) count += p2.device(k).name == kName;
    CHECK_EQ(count, 1);
    // a file that does not parse is kept as .bad
    put(paths().user_input, "<inputList><oops");
    CHECK(write_device_config(paths().user_input, kGuid, kName, {{Mapping::Button, "a", 1, 1}}, err));
    CHECK(file_exists(paths().user_input + ".bad"));
    PadConfig p3;
    CHECK(p3.load_file(paths().user_input) && p3.find(kGuid, kName) >= 0);
    std::remove((paths().user_input + ".bad").c_str());
  }

  // ---- wizard state machine
  {
    InputWizard w;  // unknown pad: the table defaults
    CHECK(!w.skippable(0));      // d-pad up
    CHECK(w.skippable(4));       // start
    CHECK(!w.skippable(6));      // a
    CHECK(w.phase() == InputWizard::Phase::Rows);
    // ordinary assignment: press + release
    w.input(InKind::Hat, 0, 1);
    CHECK(w.holding());
    w.input(InKind::Hat, 0, 0);
    CHECK_EQ(w.row(), size_t(1));
    CHECK(w.mapping(0) && w.mapping(0)->type == Mapping::Hat && w.mapping(0)->value == 1);
    // the same input again on the next row is refused and the row stays
    w.input(InKind::Hat, 0, 1);
    w.input(InKind::Hat, 0, 0);
    CHECK_EQ(w.row(), size_t(1));  // hat 0 mask 1 is "up" already... mask 1 again
    CHECK(w.taken());
    w.input(InKind::Hat, 0, 4);  // down
    w.input(InKind::Hat, 0, 0);
    CHECK(!w.taken());
    CHECK_EQ(w.row(), size_t(2));
    // a button held for a second on a skippable row is skipped; on a mandatory row it is not
    w.input(InKind::Button, 3, 1);
    w.tick(1.5f);
    CHECK_EQ(w.row(), size_t(2));  // left may not be skipped
    w.input(InKind::Button, 3, 0);
    CHECK_EQ(w.row(), size_t(3));
    CHECK(w.mapping(2) && w.mapping(2)->id == 3);
    w.input(InKind::Hat, 0, 2);
    w.input(InKind::Hat, 0, 0);  // right
    CHECK_EQ(w.row(), size_t(4));
    CHECK(w.skippable(4));
    w.input(InKind::Button, 9, 1);
    w.tick(0.5f);
    CHECK(w.hold_progress() > 0.45f && w.hold_progress() < 0.55f);
    w.tick(0.6f);
    CHECK(w.state(4) == RowState::Skipped && w.row() == 5);
    CHECK(!w.holding());
    w.input(InKind::Button, 9, 0);  // the release of the skipped press does nothing
    CHECK_EQ(w.row(), size_t(5));
    // keyboard helpers
    CHECK(w.skip_row());
    CHECK(w.state(5) == RowState::Skipped);
    CHECK(w.back_row());
    CHECK(w.row() == 5 && w.state(5) == RowState::Pending);
    // go through the rest quickly: a, b, x, y get buttons 1,0,2,3 ... skip the others
    w.skip_row();  // select
    w.input(InKind::Button, 1, 1), w.input(InKind::Button, 1, 0);  // a (id 1 is free: button 3 is "left")
    w.input(InKind::Button, 0, 1), w.input(InKind::Button, 0, 0);  // b
    w.input(InKind::Button, 2, 1), w.input(InKind::Button, 2, 0);  // x
    CHECK_EQ(w.row(), size_t(9));
    w.skip_row();  // y
    while (w.phase() == InputWizard::Phase::Rows) {
      if (w.row() == 20) break;
      w.skip_row();
    }
    CHECK_EQ(w.row(), size_t(20));  // hotkeyenable
    // hotkeyenable may share an input with another row (here: b, i.e. button 0)
    w.input(InKind::Button, 0, 1), w.input(InKind::Button, 0, 0);
    CHECK(w.state(20) == RowState::Set);
    while (w.phase() == InputWizard::Phase::Rows) w.skip_row();
    CHECK(w.phase() == InputWizard::Phase::Review);
    CHECK(w.has_hotkey());
    CHECK(std::string(w.resolve(InKind::Button, 1, 1)) == "a");
    CHECK(w.resolve(InKind::Button, 1, 0) == nullptr);  // releases mean nothing
    CHECK(w.resolve(InKind::Button, 77, 1) == nullptr);
    std::vector<Mapping> r = w.result(Hotkey::None);
    CHECK_EQ(r.size(), size_t(8));  // up down left right a b x hotkeyenable
  }
  {
    // the DualShock rule: a button held together with a moving axis becomes that axis
    InputWizard w;
    w.input(InKind::Button, 5, 1);
    w.input(InKind::Axis, 2, 1);
    w.input(InKind::Axis, 2, 0);
    w.input(InKind::Button, 5, 0);
    CHECK(w.mapping(0) && w.mapping(0)->type == Mapping::Axis && w.mapping(0)->id == 2 && w.mapping(0)->value == 1);
    // pad capabilities decide what may be skipped (ES: no hats -> a/b/x/y skippable; one button -> down skippable)
    InputWizard console(18, 0, 4), arcade(1, 1, 0), five(5, 0, 0);
    CHECK(console.skippable(6) && console.skippable(7) && !console.skippable(1));
    CHECK(!arcade.skippable(6) && arcade.skippable(1));
    CHECK(five.skippable(1));
    // no select + choose select: the marker is written
    InputWizard n;
    while (n.phase() == InputWizard::Phase::Rows) {
      if (!n.skip_row()) {
        n.input(InKind::Button, uint16_t(n.row() + 20), 1);
        n.input(InKind::Button, uint16_t(n.row() + 20), 0);
      }
    }
    std::vector<Mapping> r = n.result(Hotkey::Select);
    CHECK(!r.empty() && r.back().name == "hotkeyenable" && r.back().type == Mapping::Key);
    CHECK(describe({Mapping::Axis, "x", 1, -1}) == "Axis 1-");
    CHECK(describe({Mapping::Hat, "x", 0, 8}) == "Hat 0 Left");
    CHECK(describe({Mapping::Button, "x", 7, 1}) == "Button 7");
  }

  // ---- bluetooth parsers: the exact line formats of the script and the agent
  {
    std::vector<BtDevice> l = parse_bt_list(
        "<device id=\"AA:BB:CC:DD:EE:01\" name=\"8BitDo Pro 2\" type=\"joystick\" connected=\"yes\" />\n"
        "garbage line\n"
        "<device id=\"AA:BB:CC:DD:EE:02\" name=\"Headset\" type=\"\" connected=\"no\" />\n");
    CHECK_EQ(l.size(), size_t(2));
    CHECK(l[0].connected && l[0].type == "joystick" && l[0].name == "8BitDo Pro 2");
    CHECK(!l[1].connected && l[1].type == "unknown");
    std::vector<BtDevice> live;
    apply_bt_live(live,
                  "<device id=\"AA:BB:CC:DD:EE:01\" name=\"Pad\" status=\"added\" type=\"joystick\" />\n"
                  "<device id=\"AA:BB:CC:DD:EE:02\" name=\"Buds\" status=\"added\" type=\"audio\" />\n"
                  "<device id=\"AA:BB:CC:DD:EE:01\" name=\"Pad\" status=\"removed\" type=\"joystick\" />\n"
                  "ERROR: Bluetooth agent is not running\n");
    CHECK_EQ(live.size(), size_t(1));
    CHECK(live[0].id == "AA:BB:CC:DD:EE:02" && live[0].type == "audio");
    CHECK(valid_mac("AA:BB:CC:DD:EE:01"));
    CHECK(!valid_mac("aa:bb:cc:dd:ee:01"));  // the script's grep is upper case only
    CHECK(!valid_mac("AA:BB:CC:DD:EE:01; reboot"));
    CHECK(!valid_mac("AA:BB:CC:DD:EE"));
  }

  // ---- the capture hook in PadCore
  {
    int16_t init[4] = {0, 0, -32767, 0};
    PadConfig cfg;
    cfg.parse("<inputList><inputConfig type=\"joystick\" deviceName=\"p\" deviceGUID=\"abcd\"><input name=\"a\" type=\"button\" id=\"1\" value=\"1\"/>"
              "<input name=\"up\" type=\"hat\" id=\"0\" value=\"1\"/><input name=\"leftanalogright\" type=\"axis\" id=\"0\" value=\"1\"/></inputConfig></inputList>");
    PadCore core(cfg, true);
    DevId d0 = core.add_device("abcd", "p", init, 4), d1 = core.add_device("abcd", "p", init, 4);
    std::vector<Action> acts;
    TimePoint t0 = Clock::now();
    std::vector<CaptureEvent> got;
    core.set_capture([&](const CaptureEvent& e) {
      got.push_back(e);
      return e.dev == d0;  // consume the first pad only
    });
    core.event({RawEvent::Button, d0, 1, 1}, t0, acts);
    core.event({RawEvent::Button, d1, 1, 1}, t0, acts);
    CHECK_EQ(got.size(), size_t(2));
    CHECK_EQ(acts.size(), size_t(1));  // d1 was not consumed: its A went through the mapping
    CHECK(got[0].kind == InKind::Button && got[0].value == 1 && *got[0].guid == "abcd" && *got[0].name == "p");
    got.clear();
    acts.clear();
    core.event({RawEvent::Hat, d0, 0, 1}, t0, acts);   // up
    core.event({RawEvent::Hat, d0, 0, 1}, t0, acts);   // the same state again: no event
    core.event({RawEvent::Hat, d0, 0, 0}, t0, acts);
    CHECK_EQ(got.size(), size_t(2));
    CHECK(got.size() == 2 && got[0].kind == InKind::Hat && got[0].value == 1 && got[1].value == 0);
    CHECK(acts.empty());
    got.clear();
    // axis: only the dead-zone crossings, as -1/0/+1, relative to the resting value
    core.event({RawEvent::Axis, d0, 0, 1000}, t0, acts);    // inside the zone
    core.event({RawEvent::Axis, d0, 0, 30000}, t0, acts);   // crosses
    core.event({RawEvent::Axis, d0, 0, 32000}, t0, acts);   // still outside
    core.event({RawEvent::Axis, d0, 0, 0}, t0, acts);       // back
    core.event({RawEvent::Axis, d0, 2, -32767}, t0, acts);  // trigger at rest: nothing
    core.event({RawEvent::Axis, d0, 2, 32767}, t0, acts);   // trigger pressed: +1
    CHECK_EQ(got.size(), size_t(3));
    CHECK(got.size() == 3 && got[0].value == 1 && got[1].value == 0 && got[2].id == 2 && got[2].value == 1);
    CHECK(acts.empty());
    // removing the sink: nothing reads as pressed afterwards (the consumed release updated the state)
    core.set_capture({});
    acts.clear();
    core.event({RawEvent::Axis, d0, 2, -32767}, t0, acts);
    core.event({RawEvent::Hat, d0, 0, 1}, t0, acts);
    CHECK_EQ(acts.size(), size_t(1));  // the hat press (up) is a normal press now; the axis release made no action
    CHECK(acts[0] == Action::Up);
    // installing a sink forgets a held direction
    core.event({RawEvent::Hat, d0, 0, 0}, t0, acts);
    core.event({RawEvent::Hat, d0, 0, 1}, t0, acts);
    CHECK(core.next_deadline().has_value());
    core.set_capture([](const CaptureEvent&) { return false; });
    CHECK(!core.next_deadline().has_value());
    core.set_capture({});
    // set_config recomputes the mapping of the open pads
    PadCore c2(PadConfig(), true);
    DevId e0 = c2.add_device("abcd", "p", init, 4);
    CHECK(!c2.is_configured(e0));
    c2.set_config(cfg);
    CHECK(c2.is_configured(e0));
  }

  // ---- the page
  {
    Ctx cx;
    make_ctx(cx);
    FakePad pad;
    pad.list.push_back({0, kGuid, kName, 18, 0, 4});
    pad.list.push_back({1, "0300bbbb", "Some BT Pad", 12, 1, 4});
    cx.pad = &pad;
    put(paths().user_input, "<inputList><inputConfig type=\"joystick\" deviceName=\"retrogame_joypad\" deviceGUID=\"" + std::string(kGuid) + "\"><input name=\"a\" type=\"button\" id=\"1\" value=\"1\"/></inputConfig></inputList>");
    put(dir + "/system.cfg", "controllers.bluetooth.enabled=1\n");
    cx.conf = SystemConf::load(dir + "/system.cfg");
    put(paths().bt_tool, "#!/bin/sh\n");
    cx.es = EsSettings::from_text("<?xml version=\"1.0\"?><config><bool name=\"ShowControllerActivity\" value=\"false\"/></config>", dir + "/es_settings.cfg");
    CHECK(Registry::instance().has("menu.controllers"));
    CHECK(Registry::instance().has("menu.controllers.detect"));
    CHECK(Registry::instance().has("menu.controllers.config"));
    CHECK(Registry::instance().has("menu.controllers.btpair"));
    CHECK(Registry::instance().has("menu.controllers.btdevices"));
    CHECK(Registry::instance().has("menu.controllers.btdevice"));
    std::unique_ptr<Screen> sc = Registry::instance().make("menu.controllers", cx, "");
    CHECK(sc != nullptr);
    Menu& m = *static_cast<Menu*>(sc.get());
    press(m, cx, Action::Down);  // builds
    std::vector<std::string> labels;
    for (size_t i = 0; i < m.size(); ++i) labels.push_back(m.entry(i).label);
    auto find = [&](const char* l) -> int {
      for (size_t i = 0; i < m.size(); ++i)
        if (m.entry(i).label == l) return int(i);
      return -1;
    };
    CHECK(find("CONTROLLER MAPPING") >= 0);
    CHECK(find("AUTOCONFIGURE RETROARCH HOTKEYS") >= 0);
    CHECK(find("retrogame_joypad") >= 0 && find("Some BT Pad") >= 0);
    CHECK(find("ENABLE RUMBLE") < 0);                  // no sysfs file: hidden
    CHECK(find("ENABLE BLUETOOTH") >= 0);
    CHECK(find("PAIR BLUETOOTH PADS AUTOMATICALLY") >= 0 && find("BLUETOOTH DEVICE LIST") >= 0);  // enabled in system.cfg
    CHECK(find("SHOW CONTROLLER NOTIFICATIONS") >= 0);
    CHECK(find("SHOW CONTROLLER BATTERY LEVEL") < 0);  // activity off: no battery option
    int pad0 = find("retrogame_joypad"), pad1 = find("Some BT Pad");
    CHECK(m.entry(size_t(pad0)).value_text.empty());       // configured
    CHECK(m.entry(size_t(pad1)).value_text == "Not configured");
    // autohotkeys: system.cfg through the locked read-modify-write; absent = off (ES getBool)
    int ah = find("AUTOCONFIGURE RETROARCH HOTKEYS");
    CHECK(!m.entry(size_t(ah)).get_b());
    m.entry(size_t(ah)).set_b(true);
    CHECK(slurp(dir + "/system.cfg").find("system.autohotkeys=1") != std::string::npos);
    CHECK(slurp(dir + "/system.cfg").find("controllers.bluetooth.enabled=1") != std::string::npos);
    // the display options go to es_settings.cfg
    int sn = find("SHOW CONTROLLER NOTIFICATIONS");
    CHECK(m.entry(size_t(sn)).get_b());
    m.entry(size_t(sn)).set_b(false);
    CHECK(slurp(dir + "/es_settings.cfg").find("ShowControllerNotifications") != std::string::npos);
    // bluetooth toggle: setting + the exact ES command, run as a job
    int bt = find("ENABLE BLUETOOTH");
    m.entry(size_t(bt)).set_b(false);
    drain_jobs(cx);
    CHECK(slurp(dir + "/system.cfg").find("controllers.bluetooth.enabled=0") != std::string::npos);
    CHECK(has_cmd(cx, "rocknix-bluetooth disable"));
    m.entry(size_t(bt)).set_b(true);
    drain_jobs(cx);
    CHECK(has_cmd(cx, "rocknix-bluetooth enable 2>&1 >/dev/null"));
    // auto pair: spawned (not a job), the second press says it is running
    cx.sv->cmd.log.clear();
    press(m, cx, Action::Down);  // rebuilt after the job: the cursor is somewhere; find the entry again
    int ap = find("PAIR BLUETOOTH PADS AUTOMATICALLY");
    CHECK(ap >= 0);
    m.entry(size_t(ap)).on_a(cx);
    CHECK(has_cmd(cx, "rocknix-bluetooth trust input"));
    size_t n_before = cx.sv->cmd.log.size();
    ap = find("PAIR BLUETOOTH PADS AUTOMATICALLY");
    m.entry(size_t(ap)).on_a(cx);
    CHECK_EQ(cx.sv->cmd.log.size(), n_before);  // not started twice
    Transition t = m.entry(size_t(find("PAIR A BLUETOOTH DEVICE MANUALLY"))).on_a(cx);
    CHECK(t.kind == Transition::None);          // refused while the automatic scan runs
    // a pad row opens the intro, which leads to the wizard with guid + name
    Transition intro = m.entry(size_t(find("Some BT Pad"))).on_a(cx);
    CHECK(intro.screen != nullptr);
    sc.reset();
  }

  // ---- the page with the rumble file and no Bluetooth tool
  {
    Ctx cx;
    make_ctx(cx);
    put(dir + "/system.cfg", "");
    cx.conf = SystemConf::load(dir + "/system.cfg");
    std::remove(paths().bt_tool.c_str());
    std::string plat = dir + "/platform/rocknix-singleadc-joypad";
    CHECK(std::system(("mkdir -p " + plat).c_str()) == 0);
    put(plat + "/rumble_enable", "0\n");
    CHECK(rumble_path() == plat + "/rumble_enable");
    std::unique_ptr<Screen> sc = Registry::instance().make("menu.controllers", cx, "");
    Menu& m = *static_cast<Menu*>(sc.get());
    press(m, cx, Action::Down);
    int ru = -1, bt = -1, hdr = -1;
    for (size_t i = 0; i < m.size(); ++i) {
      if (m.entry(i).label == "ENABLE RUMBLE") ru = int(i);
      if (m.entry(i).label == "ENABLE BLUETOOTH") bt = int(i);
      if (m.entry(i).label == "BLUETOOTH") hdr = int(i);
      if (m.entry(i).label == "CONNECTED CONTROLLERS") CHECK(false);  // no pad: no list
    }
    CHECK(ru >= 0);
    CHECK(bt < 0 && hdr < 0);  // no rocknix-bluetooth: the whole group is hidden
    m.entry(size_t(ru)).set_b(true);
    CHECK(has_cmd(cx, "echo 1 >'" + plat + "/rumble_enable'"));
    CHECK(slurp(dir + "/system.cfg").find("rumble.enabled=1") != std::string::npos);
  }

  // ---- the wizard screen with a real PadCore: consumption, review, save
  {
    Ctx cx;
    make_ctx(cx);
    put(dir + "/system.cfg", "");
    cx.conf = SystemConf::load(dir + "/system.cfg");
    int16_t init[4] = {};
    FakePad pad;
    pad.reload_from = paths().user_input;
    DevId id = pad.add_device(kGuid, kName, init, 4);
    cx.pad = &pad;
    std::remove(paths().user_input.c_str());
    oscontract::idle().set_timeout(60000);
    ConfigScreen scr(cx, kGuid, kName, 18, 1, 4);
    scr.tick(0.01f, cx);  // installs the sink
    std::vector<Action> acts;
    TimePoint now = Clock::now();
    // the target pad's events never become actions while the wizard listens
    pad.event({RawEvent::Button, id, 1, 1}, now, acts);
    pad.event({RawEvent::Button, id, 1, 0}, now, acts);
    CHECK(acts.empty());
    CHECK_EQ(scr.wizard().row(), size_t(1));  // button 1 became "up"
    CHECK(scr.wizard().mapping(0) != nullptr);
    // the activity stamp moves with every consumed event (the loop never sees an Action)
    oscontract::idle().input(now - std::chrono::seconds(50));
    pad.event({RawEvent::Button, id, 2, 1}, now, acts);
    pad.event({RawEvent::Button, id, 2, 0}, now, acts);
    CHECK(oscontract::idle().deadline().has_value() && *oscontract::idle().deadline() > now + std::chrono::seconds(30));
    // another device is not ours: its events go through the normal mapping
    std::string other = "0300ffff";
    CHECK(!scr.on_capture({7, InKind::Button, 1, 1, &other, &other}));
    // finish: skip everything that may be skipped, give the rest buttons
    int guard = 0;
    while (scr.wizard().phase() == InputWizard::Phase::Rows && ++guard < 100) {
      if (!scr.wizard().skip_row()) tap_button(scr, uint16_t(30 + scr.wizard().row()));
    }
    CHECK(scr.wizard().phase() == InputWizard::Phase::Review);
    CHECK(!scr.wizard().has_hotkey());
    // review through the pad: "b" (south) cancels, "a" saves; with the new mapping nothing is written yet
    CHECK(!file_exists(paths().user_input));
    // a pad button that means "a" in the new mapping asks to save; it is applied in take_transition
    uint16_t a_id = scr.wizard().mapping(6) ? uint16_t(scr.wizard().mapping(6)->id) : 0;
    CHECK(scr.wizard().mapping(6) != nullptr);
    pad.event({RawEvent::Button, id, a_id, 1}, now, acts);
    CHECK(acts.empty());
    Transition tr = scr.take_transition(cx);
    CHECK(tr.kind == Transition::Pop);
    CHECK(file_exists(paths().user_input));
    CHECK_EQ(pad.reloads, 1);
    CHECK(scr.saved());
    PadConfig pc;
    pc.load_file(paths().user_input);
    CHECK(pc.find(kGuid, kName) >= 0);
    CHECK(pad.is_configured(id));  // the live pad picked the new mapping up
    std::vector<Mapping> back = read_device_config(paths().user_input, kGuid, kName);
    bool hot = false;
    for (const Mapping& mp : back) hot = hot || (mp.name == "hotkeyenable" && mp.type == Mapping::Key);
    CHECK(hot);  // "A" = SET SELECT AS HOTKEY, but select was skipped: the disabled marker
    // the sink is removed when the screen goes
  }
  {
    // keyboard path: Escape cancels, nothing is written; X in review writes "no hotkey"
    Ctx cx;
    make_ctx(cx);
    std::remove(paths().user_input.c_str());
    ConfigScreen scr(cx, kGuid, kName, 18, 0, 4);
    CHECK(press(scr, cx, Action::B).kind == Transition::Pop);
    CHECK(!file_exists(paths().user_input));
    ConfigScreen s2(cx, kGuid, kName, 18, 0, 4);
    while (s2.wizard().phase() == InputWizard::Phase::Rows) {
      if (!s2.wizard().skip_row()) tap_button(s2, uint16_t(40 + s2.wizard().row()));
    }
    Transition t = press(s2, cx, Action::X);
    CHECK(t.kind == Transition::Pop && file_exists(paths().user_input));
    // idle for 90 s closes it without saving
    ConfigScreen s3(cx, kGuid, kName, 18, 0, 4);
    s3.tick(95.0f, cx);
    CHECK(s3.take_transition(cx).kind == Transition::Pop);
  }

  // ---- detect: holding a button for a second picks the pad and replaces itself with the wizard
  {
    Ctx cx;
    make_ctx(cx);
    FakePad pad;
    pad.list.push_back({0, kGuid, kName, 18, 0, 4});
    cx.pad = &pad;
    int16_t init[4] = {};
    DevId id = pad.add_device(kGuid, kName, init, 4);
    DetectScreen d(cx);
    d.tick(0.01f, cx);
    std::vector<Action> acts;
    TimePoint now = Clock::now();
    pad.event({RawEvent::Button, id, 4, 1}, now, acts);
    CHECK(d.holding());
    d.tick(0.5f, cx);
    CHECK(d.take_transition(cx).kind == Transition::None);
    d.tick(0.6f, cx);
    Transition t = d.take_transition(cx);
    CHECK(t.kind == Transition::Replace && t.screen != nullptr);
    // releasing early cancels the hold
    DetectScreen d2(cx);
    d2.tick(0.01f, cx);
    pad.event({RawEvent::Button, id, 4, 1}, now, acts);
    d2.tick(0.3f, cx);
    pad.event({RawEvent::Button, id, 4, 0}, now, acts);
    CHECK(!d2.holding());
    d2.tick(2.0f, cx);
    CHECK(d2.take_transition(cx).kind == Transition::None);
    // B cancels (the picker never consumes the pad)
    CHECK(press(d2, cx, Action::B).kind == Transition::Pop);
  }

  // ---- bluetooth screens
  {
    Ctx cx;
    make_ctx(cx);
    cx.sv->cmd.keep_log = true;
    {
      std::unique_ptr<Screen> sc = Registry::instance().make("menu.controllers.btpair", cx, "");
      CHECK(sc != nullptr);
      Menu& m = *static_cast<Menu*>(sc.get());
      press(m, cx, Action::Down);  // build + on_open: the scan is started with spawn
      CHECK(has_cmd(cx, "rocknix-bluetooth live_devices >"));
      CHECK_EQ(m.size(), size_t(1));  // "scanning" placeholder
      m.tick(0.01f, cx);  // arms the 1 Hz poll
      CHECK(m.wake_in().has_value());
      put(paths().bt_live_log,
          "<device id=\"AA:BB:CC:DD:EE:01\" name=\"8BitDo Pro 2\" status=\"added\" type=\"joystick\" />\n"
          "<device id=\"AA:BB:CC:DD:EE:02\" name=\"Buds\" status=\"added\" type=\"audio\" />\n");
      m.tick(1.2f, cx);   // the poll fires and asks for a rebuild
      m.tick(0.01f, cx);  // rebuild happens on the next sync
      CHECK_EQ(m.size(), size_t(2));
      CHECK(m.entry(0).label == "8BitDo Pro 2" && m.entry(0).icon_name == "controller");
      CHECK(m.entry(1).icon_name == "sound");
      // pairing: the exact command, as a job; success closes the screen
      cx.sv->cmd.log.clear();
      m.entry(0).on_a(cx);
      drain_jobs(cx);
      CHECK(has_cmd(cx, "rocknix-bluetooth trust 'AA:BB:CC:DD:EE:01'"));
      CHECK(has_cmd(cx, "rocknix-bluetooth list"));
      cx.sv->cmd.log.clear();
    }
    CHECK(has_cmd(cx, "rocknix-bluetooth stop_live_devices"));  // sent when the screen is destroyed
    // an agent that is not running shows the script's complaint
    cx.sv->cmd.log.clear();
    {
      std::unique_ptr<Screen> sc = Registry::instance().make("menu.controllers.btpair", cx, "");
      Menu& m = *static_cast<Menu*>(sc.get());
      press(m, cx, Action::Down);
      m.tick(0.01f, cx);  // arms the poll
      put(paths().bt_live_log, "ERROR: Bluetooth agent is not running\n");
      m.tick(1.2f, cx);
      m.tick(0.01f, cx);
      CHECK(m.size() == 1 && m.entry(0).label == "ERROR: Bluetooth agent is not running");
    }
    // device screen: argument parsing, commands and confirmations
    CHECK(Registry::instance().make("menu.controllers.btdevice", cx, "nonsense") == nullptr);
    CHECK(Registry::instance().make("menu.controllers.btdevice", cx, "AA:BB:CC:DD:EE:01; reboot 0 x") == nullptr);
    {
      std::unique_ptr<Screen> sc = Registry::instance().make("menu.controllers.btdevice", cx, "AA:BB:CC:DD:EE:01 1 8BitDo Pro 2");
      Menu& m = *static_cast<Menu*>(sc.get());
      press(m, cx, Action::Down);
      CHECK(m.title() == "8BitDo Pro 2");
      CHECK_EQ(m.size(), size_t(2));
      CHECK(m.entry(0).label == "DISCONNECT");
      CHECK(m.entry(0).confirm_msg == "ARE YOU SURE YOU WANT TO DISCONNECT '8BitDo Pro 2' ?");
      CHECK(m.entry(1).label == "FORGET" && m.entry(1).is_danger);
      cx.sv->cmd.log.clear();
      m.entry(1).on_a(cx);
      drain_jobs(cx);
      CHECK(has_cmd(cx, "rocknix-bluetooth remove 'AA:BB:CC:DD:EE:01'"));
      CHECK(sc->take_transition(cx).kind == Transition::Pop);  // closes by itself when the job ends
    }
    {
      std::unique_ptr<Screen> sc = Registry::instance().make("menu.controllers.btdevice", cx, "AA:BB:CC:DD:EE:02 0 Buds");
      Menu& m = *static_cast<Menu*>(sc.get());
      press(m, cx, Action::Down);
      CHECK(m.entry(0).label == "CONNECT");
      cx.sv->cmd.log.clear();
      m.entry(0).on_a(cx);
      drain_jobs(cx);
      CHECK(has_cmd(cx, "rocknix-bluetooth connect 'AA:BB:CC:DD:EE:02'"));
    }
    // paired list: `list` runs as a job, rows open the device screen, REMOVE ALL uses rocknix-config forgetBT
    {
      std::unique_ptr<Screen> sc = Registry::instance().make("menu.controllers.btdevices", cx, "");
      Menu& m = *static_cast<Menu*>(sc.get());
      press(m, cx, Action::Down);
      drain_jobs(cx);  // dry-run output is empty
      CHECK(has_cmd(cx, "rocknix-bluetooth list"));
      m.tick(0.01f, cx);
      CHECK(m.size() == 1 && m.entry(0).label == "NO BLUETOOTH DEVICES FOUND");
    }
  }

  // ---- drawing at the four sizes: steady state allocates nothing, wizard states all draw
  {
    const std::string tdir = "/home/epsil/zenith-frontend/theme-dark";
    if (is_dir(tdir)) {
      const unsigned sizes[4][2] = {{480, 320}, {640, 480}, {720, 405}, {720, 720}};
      for (const auto& wh : sizes) {
        Ctx cx;
        std::string err;
        cx.w = float(wh[0]);
        cx.h = float(wh[1]);
        CHECK(Theme::load(tdir, ratio_name(wh[0], wh[1]), cx.th, err));
        CHECK(cx.assets.load_font(cx.th.path(cx.th.str("font")), err));
        cx.sv->cmd.dry_run = true;
        ConfigScreen scr(cx, kGuid, kName, 18, 0, 4);
        std::vector<Op> a1, a2;
        scr.draw(cx, a1);
        CHECK(a1.size() > 40);
        scr.draw(cx, a2);
        CHECK_EQ(a1.size(), a2.size());
        for (int pass = 0; pass < 2; ++pass) {
          a2.clear();
          scr.draw(cx, a2);
          size_t before = g_allocs;
          for (int i = 0; i < 50; ++i) {
            a2.clear();
            scr.draw(cx, a2);
          }
          if (pass) CHECK_EQ(g_allocs - before, size_t(0));
        }
        // move through the states
        tap_button(scr, 3);
        scr.draw(cx, a2 = {});
        {
          std::string g = kGuid, n = kName;
          scr.on_capture({0, InKind::Button, 9, 1, &g, &n});
        }
        DetectScreen d(cx);
        std::vector<Op> d1;
        d.draw(cx, d1);
        CHECK(d1.size() > 3);
        for (int pass = 0; pass < 2; ++pass) {
          d1.clear();
          d.draw(cx, d1);
          size_t before = g_allocs;
          for (int i = 0; i < 50; ++i) {
            d1.clear();
            d.draw(cx, d1);
          }
          if (pass) CHECK_EQ(g_allocs - before, size_t(0));
        }
      }
    }
  }

  std::string cmd = "rm -rf " + dir;
  (void)!std::system(cmd.c_str());
  return zt::done();
}
