#include <chrono>
#include <cstdlib>
#include <string>

#include "check.hpp"
#include "pad.hpp"
#include "util.hpp"

using namespace zf;
using std::chrono::milliseconds;

namespace {
// r36s_Gamepad: d-pad = buttons 13..16, a=1 b=0
const char* R36S_G = "19003982010000008811000088010000";
const char* R36S_N = "r36s_Gamepad";
// InputPlumber: d-pad = hat 0, select=6
const char* PL_G = "0300f5a35e040000120b000001000000";
const char* PL_N = "InputPlumber GameController";
const char* RETRO_G = "19009b4d4b4800000111000000010000";
const char* RETRO_N = "retrogame_joypad";

PadConfig load_cfg() {
  PadConfig c;
  std::string xml;
  CHECK(read_file(std::string(ZF_TEST_DATA) + "/es_input.cfg", xml));
  CHECK(c.parse(xml));
  return c;
}

std::vector<Action> ev(PadCore& p, RawEvent e, TimePoint t) {
  std::vector<Action> v;
  p.event(e, t, v);
  return v;
}
RawEvent btn(DevId d, uint16_t id, bool down) { return {RawEvent::Button, d, id, down}; }
RawEvent hat(DevId d, uint16_t id, int v) { return {RawEvent::Hat, d, id, v}; }
RawEvent axis(DevId d, uint16_t id, int v) { return {RawEvent::Axis, d, id, v}; }
using V = std::vector<Action>;

void test_config() {
  PadConfig c = load_cfg();
  CHECK(c.size() > 50);
  CHECK(c.find(R36S_G, R36S_N) >= 0);
  // same name, different GUID CRC bytes -> approximate
  CHECK(c.find("19003982ffffffff8811000088010000", R36S_N) >= 0);
  CHECK(c.find("00000000000000000000000000000000", "nonexistent pad") < 0);
  // GUID only (name unknown) and name only (GUID unknown)
  CHECK(c.find(R36S_G, "other name") >= 0);
  CHECK(c.find("ffffffffffffffffffffffffffffffff", R36S_N) >= 0);
  // upper-case GUID from the driver still matches
  CHECK(c.find("19003982010000008811000088010000", R36S_N) == c.find("19003982010000008811000088010000", R36S_N));
  PadConfig bad;
  CHECK(!bad.parse("<inputList><inputConfig"));
  CHECK(bad.parse("<inputList><inputConfig type=\"keyboard\" deviceName=\"k\" deviceGUID=\"-1\"/></inputList>"));
  CHECK_EQ(bad.size(), size_t(0));
}

void test_ab_invert() {
  PadCore p(load_cfg(), true);
  DevId d = p.add_device(R36S_G, R36S_N, nullptr, 0);
  TimePoint t = Clock::now();
  CHECK(ev(p, btn(d, 1, true), t) == V{Action::A});
  CHECK(ev(p, btn(d, 0, true), t) == V{Action::B});
  CHECK(ev(p, btn(d, 2, true), t) == V{Action::X});
  CHECK(ev(p, btn(d, 3, true), t) == V{Action::Y});
  CHECK(ev(p, btn(d, 9, true), t) == V{Action::Start});
  CHECK(ev(p, btn(d, 8, true), t) == V{Action::Select});
  CHECK(ev(p, btn(d, 10, true), t) == V{Action::Hotkey});
  CHECK(ev(p, btn(d, 4, true), t) == V{Action::L});
  CHECK(ev(p, btn(d, 5, true), t) == V{Action::R});
  p.set_invert_buttons(false);
  CHECK(ev(p, btn(d, 1, true), t) == V{Action::B});
  CHECK(ev(p, btn(d, 0, true), t) == V{Action::A});
}

void test_hotkey_select() {
  PadCore p(load_cfg(), true);
  DevId d = p.add_device(RETRO_G, RETRO_N, nullptr, 0);
  CHECK(ev(p, btn(d, 8, true), Clock::now()) == V{Action::Select});
}

void test_unknown() {
  PadCore p(load_cfg(), true);
  DevId d = p.add_device("00", "who knows", nullptr, 0);
  CHECK(!p.is_configured(d));
  CHECK(ev(p, btn(d, 1, true), Clock::now()).empty());
  CHECK(ev(p, btn(99, 1, true), Clock::now()).empty());  // unknown handle
}

void test_repeat_quick() {
  PadCore p(load_cfg(), true);
  DevId d = p.add_device(R36S_G, R36S_N, nullptr, 0);
  TimePoint t0 = Clock::now();
  CHECK(ev(p, btn(d, 14, true), t0) == V{Action::Down});  // immediate step
  CHECK(p.next_deadline() == t0 + milliseconds(500));
  V out;
  p.repeat_due(t0 + milliseconds(499), out);
  CHECK(out.empty());
  p.repeat_due(t0 + milliseconds(500), out);
  CHECK(out == V{Action::Down});
  CHECK(p.next_deadline() == t0 + milliseconds(614));  // 114 ms stage
  out.clear();
  p.repeat_due(t0 + milliseconds(614 + 114), out);
  CHECK_EQ(out.size(), size_t(2));
  CHECK(ev(p, btn(d, 14, false), t0).empty());  // release stops everything
  CHECK(!p.next_deadline().has_value());
  out.clear();
  p.repeat_due(t0 + std::chrono::seconds(10), out);
  CHECK(out.empty());
}

void test_stages() {
  CHECK_EQ(repeat_delay_at(kQuick, 0), 500u);
  CHECK_EQ(repeat_delay_at(kQuick, 500), 114u);
  CHECK_EQ(repeat_delay_at(kQuick, 2499), 114u);
  CHECK_EQ(repeat_delay_at(kQuick, 2500), 32u);
  CHECK_EQ(repeat_delay_at(kQuick, 6500), 16u);
  CHECK_EQ(repeat_delay_at(kQuick, 100000), 16u);
  CHECK_EQ(repeat_delay_at(kSlow, 0), 500u);
  CHECK_EQ(repeat_delay_at(kSlow, 500), 200u);
  CHECK_EQ(repeat_delay_at(kSlow, 9000), 200u);
}

void test_slow() {
  PadCore p(load_cfg(), true);
  p.set_profile(kSlow);
  DevId d = p.add_device(R36S_G, R36S_N, nullptr, 0);
  TimePoint t0 = Clock::now();
  ev(p, btn(d, 16, true), t0);
  V out;
  p.repeat_due(t0 + milliseconds(500), out);
  CHECK(p.next_deadline() == t0 + milliseconds(700));
}

void test_hat() {
  PadCore p(load_cfg(), true);
  DevId d = p.add_device(PL_G, PL_N, nullptr, 0);
  TimePoint t = Clock::now();
  CHECK(ev(p, hat(d, 0, 4), t) == V{Action::Down});
  CHECK(p.next_deadline().has_value());
  // diagonal: down stays held, right is a new press (and becomes the repeating one)
  CHECK(ev(p, hat(d, 0, 4 | 2), t) == V{Action::Right});
  // releasing down (still right) must not stop right's repeat
  CHECK(ev(p, hat(d, 0, 2), t).empty());
  CHECK(p.next_deadline().has_value());
  CHECK(ev(p, hat(d, 0, 0), t).empty());  // centre stops
  CHECK(!p.next_deadline().has_value());
  CHECK(ev(p, hat(d, 0, 8), t) == V{Action::Left});
}

void test_axis() {
  PadCore p(load_cfg(), true);
  const int16_t init[4] = {0, 0, 0, 0};
  DevId d = p.add_device(R36S_G, R36S_N, init, 4);
  TimePoint t = Clock::now();
  CHECK(ev(p, axis(d, 1, 10000), t).empty());
  CHECK(ev(p, axis(d, 1, 23000), t).empty());  // not above the threshold yet
  CHECK(ev(p, axis(d, 1, 23001), t) == V{Action::Down});
  CHECK(ev(p, axis(d, 1, 32000), t).empty());  // already beyond: nothing new
  CHECK(p.next_deadline().has_value());
  CHECK(ev(p, axis(d, 1, 5000), t).empty());  // back inside: release
  CHECK(!p.next_deadline().has_value());
  CHECK(ev(p, axis(d, 1, -30000), t) == V{Action::Up});
  CHECK(ev(p, axis(d, 1, 30000), t) == V{Action::Down});  // straight flip
}

void test_axis_init() {
  PadCore p(load_cfg(), true);
  const int16_t init[2] = {0, -20000};
  DevId d = p.add_device(R36S_G, R36S_N, init, 2);
  TimePoint t = Clock::now();
  CHECK(ev(p, axis(d, 1, -20000), t).empty());  // resting value is not "up"
  CHECK(ev(p, axis(d, 1, 2000), t).empty());    // 22000 above rest
  CHECK(ev(p, axis(d, 1, 3100), t) == V{Action::Down});  // 23100
}

void test_no_timer_for_face_buttons() {
  PadCore p(load_cfg(), true);
  DevId d = p.add_device(R36S_G, R36S_N, nullptr, 0);
  ev(p, btn(d, 1, true), Clock::now());
  CHECK(!p.next_deadline().has_value());
}

void test_remove_and_reuse() {
  PadCore p(load_cfg(), true);
  DevId d = p.add_device(R36S_G, R36S_N, nullptr, 0);
  ev(p, btn(d, 14, true), Clock::now());
  p.remove_device(d);
  CHECK(!p.next_deadline().has_value());
  CHECK(ev(p, btn(d, 14, true), Clock::now()).empty());
  DevId d2 = p.add_device(PL_G, PL_N, nullptr, 0);  // slot is reused, state is fresh
  CHECK_EQ(d2, d);
  CHECK(ev(p, hat(d2, 0, 1), Clock::now()) == V{Action::Up});
}

void test_stall() {
  PadCore p(load_cfg(), true);
  DevId d = p.add_device(R36S_G, R36S_N, nullptr, 0);
  TimePoint t0 = Clock::now();
  ev(p, btn(d, 13, true), t0);
  V out;
  p.repeat_due(t0 + std::chrono::seconds(30), out);
  CHECK_EQ(out.size(), size_t(4));
  CHECK(p.next_deadline().has_value());
}

void test_clear() {
  PadCore p(load_cfg(), true);
  DevId d = p.add_device(PL_G, PL_N, nullptr, 0);
  ev(p, hat(d, 0, 4), Clock::now());
  p.clear();
  CHECK(!p.next_deadline().has_value());
  CHECK(ev(p, hat(d, 0, 4), Clock::now()) == V{Action::Down});  // hat state forgotten: a press again
}

// ---- reserve mapping from SDL game controller mapping strings (pads without an es_input.cfg entry)

// gamecontrollerdb-style entries: Xbox pad (hat d-pad, trigger axes), d-pad as half axes, inverted left stick
const char* kXbox =
    "0300000011111111aaaa000000000000,Zeta Test Pad,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b8,"
    "leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,"
    "start:b7,x:b2,y:b3,platform:Linux,";
const char* kHalfAxis =
    "03000000c82d00000090000011010000,8BitDo FC30 Pro,a:b0,b:b1,back:b10,dpdown:+a1,dpleft:-a0,dpright:+a0,dpup:-a1,start:b11,"
    "leftx:a0,lefty:a1,lefttrigger:b8,righttrigger:b9,leftshoulder:b6,rightshoulder:b7,platform:Linux,";
const char* kInverted = "0300000000000000aaaa000000000000,Odd pad,a:b0,b:b1,leftx:a0,lefty:a1~,lefttrigger:a2~,dpup:b12,dpdown:b13,platform:Linux,";

void test_sdl_mapping_parse() {
  PadConfig::Device d;
  CHECK(PadConfig::from_sdl_mapping(kXbox, d));
  CHECK(d.name == "Zeta Test Pad");
  CHECK(d.guid == "0300000011111111aaaa000000000000");
  auto find = [&](const char* n) -> const PadConfig::Input* {
    for (const auto& i : d.inputs)
      if (i.name == n) return &i;
    return nullptr;
  };
  // ES's table: SDL a (south) -> "b", SDL b (east) -> "a", x <-> y
  CHECK(find("b") && find("b")->kind == InKind::Button && find("b")->id == 0);
  CHECK(find("a") && find("a")->id == 1);
  CHECK(find("y") && find("y")->id == 2 && find("x") && find("x")->id == 3);
  CHECK(find("select") && find("select")->id == 6 && find("hotkeyenable") && find("hotkeyenable")->id == 6);
  CHECK(find("up") && find("up")->kind == InKind::Hat && find("up")->id == 0 && find("up")->value == 1);
  CHECK(find("left") && find("left")->value == 8);
  CHECK(find("l2") && find("l2")->kind == InKind::Axis && find("l2")->id == 2 && find("l2")->value == 1);
  CHECK(find("pageup") && find("pagedown") && find("l3") && find("r3"));
  CHECK(find("leftanalogleft") && find("leftanalogleft")->value == -1 && find("leftanalogright")->value == 1);
  CHECK(find("leftanalogup")->id == 1 && find("leftanalogup")->value == -1);
  CHECK(!find("guide") && !find("rightanalogleft"));

  CHECK(PadConfig::from_sdl_mapping(kInverted, d));
  CHECK(find("leftanalogup")->value == 1 && find("leftanalogdown")->value == -1);  // lefty:a1~
  CHECK(find("l2")->value == -1);                                                   // lefttrigger:a2~
  CHECK(find("up")->kind == InKind::Button && find("up")->id == 12);

  CHECK(!PadConfig::from_sdl_mapping("", d));
  CHECK(!PadConfig::from_sdl_mapping("guid,name,platform:Linux,crc:1234,", d));
  CHECK(!PadConfig::from_sdl_mapping("guid,name,a:bX,b:h0,x:a,y:h0.99,", d));  // malformed values are skipped
}

void test_sdl_fallback_xbox() {
  PadCore p(load_cfg(), true);
  const int16_t init[6] = {0, 0, -32768, 0, 0, -32768};  // triggers rest at -32768
  DevId d = p.add_device("0300000011111111aaaa000000000000", "Zeta Test Pad", init, 6);
  CHECK(!p.is_configured(d) && p.mapping_source(d) == 0);
  TimePoint t = Clock::now();
  CHECK(ev(p, btn(d, 1, true), t).empty());  // before the reserve mapping it does nothing
  CHECK(p.set_sdl_mapping(d, kXbox));
  CHECK(!p.is_configured(d) && p.mapping_source(d) == 2);
  CHECK(ev(p, btn(d, 1, true), t) == V{Action::A});  // east = OK (InvertButtons default)
  CHECK(ev(p, btn(d, 0, true), t) == V{Action::B});  // south = BACK
  CHECK(ev(p, btn(d, 2, true), t) == V{Action::Y});  // west = ES "y"
  CHECK(ev(p, btn(d, 3, true), t) == V{Action::X});  // north = ES "x"
  CHECK(ev(p, btn(d, 7, true), t) == V{Action::Start});
  CHECK(ev(p, btn(d, 6, true), t) == V{Action::Select});  // hotkey shares the button: select wins
  CHECK(ev(p, btn(d, 4, true), t) == V{Action::L});
  CHECK(ev(p, btn(d, 5, true), t) == V{Action::R});
  CHECK(ev(p, btn(d, 9, true), t) == V{Action::L3});
  CHECK(ev(p, btn(d, 10, true), t) == V{Action::R3});
  CHECK(ev(p, hat(d, 0, 1), t) == V{Action::Up});
  CHECK(ev(p, hat(d, 0, 0), t).empty());
  CHECK(ev(p, hat(d, 0, 8), t) == V{Action::Left});
  CHECK(ev(p, hat(d, 0, 0), t).empty());
  // triggers: resting value is not a press, a deep pull is
  CHECK(ev(p, axis(d, 2, -32768), t).empty());
  CHECK(ev(p, axis(d, 2, 32767), t) == V{Action::L2});
  CHECK(ev(p, axis(d, 2, -32768), t).empty());
  CHECK(ev(p, axis(d, 5, 20000), t) == V{Action::R2});  // 52768 above rest
  CHECK(ev(p, axis(d, 5, -32768), t).empty());
  // left stick navigates
  CHECK(ev(p, axis(d, 0, 30000), t) == V{Action::Right});
  CHECK(p.next_deadline().has_value());
  CHECK(ev(p, axis(d, 0, 0), t).empty());
  CHECK(!p.next_deadline().has_value());
  // swapping A/B recompiles the reserve mapping too
  p.set_invert_buttons(false);
  CHECK(ev(p, btn(d, 1, true), t) == V{Action::B});
  CHECK(ev(p, btn(d, 0, true), t) == V{Action::A});
}

void test_sdl_fallback_half_axes() {
  PadCore p(load_cfg(), true);
  const int16_t init[2] = {0, 0};
  DevId d = p.add_device("03000000c82d00000090000011010000", "8BitDo FC30 Pro", init, 2);
  CHECK(p.set_sdl_mapping(d, kHalfAxis));
  TimePoint t = Clock::now();
  // the d-pad half axes and the left stick are the same axis and direction: one action, not two
  CHECK(ev(p, axis(d, 0, -30000), t) == V{Action::Left});
  CHECK(ev(p, axis(d, 0, 0), t).empty());
  CHECK(ev(p, axis(d, 1, 30000), t) == V{Action::Down});
  CHECK(ev(p, axis(d, 1, -30000), t) == V{Action::Up});
  CHECK(ev(p, btn(d, 8, true), t) == V{Action::L2});  // triggers as buttons
  CHECK(ev(p, btn(d, 9, true), t) == V{Action::R2});
  CHECK(ev(p, btn(d, 10, true), t) == V{Action::Select});
}

void test_sdl_fallback_misc() {
  PadCore p(load_cfg(), true);
  const int16_t init[3] = {0, 0, 0};
  DevId odd = p.add_device("0300000000000000aaaa000000000000", "Odd pad", init, 3);
  CHECK(p.set_sdl_mapping(odd, kInverted));
  TimePoint t = Clock::now();
  CHECK(ev(p, axis(odd, 1, 30000), t) == V{Action::Up});  // lefty:a1~ is inverted
  CHECK(ev(p, axis(odd, 1, 0), t).empty());
  CHECK(ev(p, btn(odd, 12, true), t) == V{Action::Up});
  // a pad that has an es_input.cfg entry keeps it; the reserve mapping is refused
  DevId known = p.add_device(R36S_G, R36S_N, nullptr, 0);
  CHECK(p.is_configured(known) && p.mapping_source(known) == 1);
  CHECK(!p.set_sdl_mapping(known, kXbox));
  CHECK(ev(p, btn(known, 1, true), t) == V{Action::A});
  // unusable string: stays unmapped
  DevId none = p.add_device("00", "nothing", nullptr, 0);
  CHECK(!p.set_sdl_mapping(none, "guid,name,platform:Linux,"));
  CHECK(p.mapping_source(none) == 0);
  // removing the pad drops the mapping with it; a reused slot starts clean
  p.remove_device(odd);
  DevId again = p.add_device("00", "other", nullptr, 0);
  CHECK_EQ(again, odd);
  CHECK(p.mapping_source(again) == 0 && ev(p, btn(again, 12, true), t).empty());
}

// the names ES cfg files use for the extra buttons
void test_cfg_extra_names() {
  PadConfig c;
  CHECK(c.parse(
      "<inputList><inputConfig type=\"joystick\" deviceName=\"Extra\" deviceGUID=\"aa\">"
      "<input name=\"pageup\" type=\"button\" id=\"4\" value=\"1\"/><input name=\"leftshoulder\" type=\"button\" id=\"4\" value=\"1\"/>"
      "<input name=\"pagedown\" type=\"button\" id=\"5\" value=\"1\"/>"
      "<input name=\"l2\" type=\"button\" id=\"6\" value=\"1\"/><input name=\"r2\" type=\"axis\" id=\"3\" value=\"1\"/>"
      "<input name=\"l3\" type=\"button\" id=\"10\" value=\"1\"/><input name=\"r3\" type=\"button\" id=\"11\" value=\"1\"/>"
      "<input name=\"hotkeyenable\" type=\"button\" id=\"10\" value=\"1\"/>"
      "</inputConfig></inputList>"));
  PadCore p(std::move(c), true);
  const int16_t init[4] = {};
  DevId d = p.add_device("aa", "Extra", init, 4);
  TimePoint t = Clock::now();
  CHECK(ev(p, btn(d, 4, true), t) == V{Action::L});  // leftshoulder + pageup on one button: once
  CHECK(ev(p, btn(d, 5, true), t) == V{Action::R});
  CHECK(ev(p, btn(d, 6, true), t) == V{Action::L2});
  CHECK(ev(p, axis(d, 3, 30000), t) == V{Action::R2});
  CHECK(ev(p, btn(d, 10, true), t) == V{Action::L3});  // l3 wins over a hotkeyenable on the same button
  CHECK(ev(p, btn(d, 11, true), t) == V{Action::R3});
}

// ES writes the stick clicks as leftthumb/rightthumb
void test_thumb_names() {
  PadConfig c;
  CHECK(c.parse(
      "<inputList><inputConfig type=\"joystick\" deviceName=\"Thumb\" deviceGUID=\"bb\">"
      "<input name=\"leftthumb\" type=\"button\" id=\"8\" value=\"1\"/><input name=\"rightthumb\" type=\"button\" id=\"9\" value=\"1\"/>"
      "</inputConfig></inputList>"));
  PadCore p(std::move(c), true);
  DevId d = p.add_device("bb", "Thumb", nullptr, 0);
  TimePoint t = Clock::now();
  CHECK(ev(p, btn(d, 8, true), t) == V{Action::L3});
  CHECK(ev(p, btn(d, 9, true), t) == V{Action::R3});
}

// a press from a pad with no mapping at all asks for the detection screen, once per press (ES ViewController::input)
void test_unconfigured_press() {
  PadCore p(load_cfg(), true);
  const int16_t init[2] = {};
  DevId d = p.add_device("0300000011111111bbbb000000000000", "Nameless Pad", init, 2);
  TimePoint t = Clock::now();
  CHECK(!p.take_unconfigured());
  CHECK(ev(p, axis(d, 0, 30000), t).empty());  // axes do not ask
  CHECK(!p.take_unconfigured());
  CHECK(ev(p, btn(d, 3, false), t).empty());  // neither does a release
  CHECK(!p.take_unconfigured());
  CHECK(ev(p, btn(d, 3, true), t).empty());
  CHECK(p.take_unconfigured());
  CHECK(!p.take_unconfigured());  // consumed
  CHECK(ev(p, hat(d, 0, 1), t).empty());
  CHECK(p.take_unconfigured());
  CHECK(ev(p, hat(d, 0, 0), t).empty());
  CHECK(!p.take_unconfigured());
  // while the wizard captures events nothing is asked (the sink declines here, so the normal path runs)
  p.set_capture([](const CaptureEvent&) { return false; });
  CHECK(ev(p, btn(d, 3, true), t).empty());
  CHECK(!p.take_unconfigured());
  p.set_capture(nullptr);
  // a pad with an es_input.cfg entry or the SDL reserve mapping never asks
  DevId known = p.add_device(R36S_G, R36S_N, nullptr, 0);
  CHECK(ev(p, btn(known, 1, true), t) == V{Action::A});
  CHECK(!p.take_unconfigured());
  DevId sdl = p.add_device("0300000011111111cccc000000000000", "Reserve Pad", init, 2);
  CHECK(p.set_sdl_mapping(sdl, "0300000011111111cccc000000000000,Reserve Pad,a:b0,b:b1,"));
  CHECK(!ev(p, btn(sdl, 1, true), t).empty());
  CHECK(!p.take_unconfigured());
}

// ns/event of the hot path (prints, no pass/fail): ZF_PAD_BENCH=1 ./test_pad
void bench() {
  PadCore p(load_cfg(), true);
  const int16_t init[6] = {};
  DevId d = p.add_device(PL_G, PL_N, init, 6);
  V out;
  out.reserve(8);
  TimePoint t0 = Clock::now();
  const int n = 4000000;
  size_t sink = 0;
  auto s = Clock::now();
  for (int i = 0; i < n; ++i) {
    out.clear();
    RawEvent e = i % 4 == 0 ? btn(d, 1, true) : i % 4 == 1 ? btn(d, 1, false) : i % 4 == 2 ? hat(d, 0, 4) : hat(d, 0, 0);
    p.event(e, t0, out);
    sink += out.size();
  }
  double ev_ns = std::chrono::duration<double, std::nano>(Clock::now() - s).count() / n;
  ev(p, hat(d, 0, 4), t0);
  s = Clock::now();
  for (int i = 0; i < n; ++i) {
    out.clear();
    p.repeat_due(t0 + milliseconds(i * 20LL), out);
    sink += out.size() + p.next_deadline().has_value();
  }
  double rep_ns = std::chrono::duration<double, std::nano>(Clock::now() - s).count() / n;
  std::fprintf(stderr, "[pad bench] event: %.1f ns/event, repeat_due+next_deadline: %.1f ns/call (sink %zu)\n", ev_ns, rep_ns, sink);
}
}  // namespace

int main() {
  test_config();
  test_ab_invert();
  test_hotkey_select();
  test_unknown();
  test_repeat_quick();
  test_stages();
  test_slow();
  test_hat();
  test_axis();
  test_axis_init();
  test_no_timer_for_face_buttons();
  test_remove_and_reuse();
  test_stall();
  test_clear();
  test_sdl_mapping_parse();
  test_sdl_fallback_xbox();
  test_sdl_fallback_half_axes();
  test_sdl_fallback_misc();
  test_cfg_extra_names();
  test_thumb_names();
  test_unconfigured_press();
  if (std::getenv("ZF_PAD_BENCH")) bench();
  return zt::done();
}
