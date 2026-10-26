#include "pad.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <pugixml.hpp>

#include "settings.hpp"

#ifdef ZF_WITH_SDL
#include <SDL.h>
#endif

namespace zf {

namespace {
const Stage kQuickStages[] = {{500, 500}, {2000, 114}, {4000, 32}, {UINT32_MAX, 16}};
const Stage kSlowStages[] = {{500, 500}, {UINT32_MAX, 200}};

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return s;
}

// GUID hex string with bytes 4..7 (chars 8..16) replaced by '0' (vendor-specific CRC that differs per driver).
std::string zero_mid(std::string g) {
  if (g.size() >= 16) g.replace(8, 8, "00000000");
  return g;
}

// es_input.cfg input name -> logical action (nullopt: not used by the frontend). `invert` swaps A/B like ES's
// `InvertButtons` (default true: physical "a", the east button, is OK).
std::optional<Action> name_action(const std::string& n, bool invert) {
  if (n == "up" || n == "leftanalogup" || n == "rightanalogup") return Action::Up;
  if (n == "down" || n == "leftanalogdown" || n == "rightanalogdown") return Action::Down;
  if (n == "left" || n == "leftanalogleft" || n == "rightanalogleft") return Action::Left;
  if (n == "right" || n == "leftanalogright" || n == "rightanalogright") return Action::Right;
  if (n == "a") return invert ? Action::A : Action::B;
  if (n == "b") return invert ? Action::B : Action::A;
  if (n == "x") return Action::X;
  if (n == "y") return Action::Y;
  if (n == "start") return Action::Start;
  if (n == "select") return Action::Select;
  // ES cfg files name the shoulders leftshoulder/rightshoulder or pageup/pagedown, the triggers lefttrigger/righttrigger or
  // l2/r2 and the stick clicks l3/r3 or leftthumb/rightthumb (a pad with both names on one button is bound once: see `compile`)
  if (n == "leftshoulder" || n == "pageup") return Action::L;
  if (n == "rightshoulder" || n == "pagedown") return Action::R;
  if (n == "lefttrigger" || n == "l2") return Action::L2;
  if (n == "righttrigger" || n == "r2") return Action::R2;
  if (n == "l3" || n == "leftthumb") return Action::L3;    // ES writes leftthumb/rightthumb, other tools l3/r3
  if (n == "r3" || n == "rightthumb") return Action::R3;
  if (n == "hotkeyenable") return Action::Hotkey;
  return std::nullopt;
}

// -1 / 0 / +1: outside the dead zone and on which side.
inline int zone(int32_t v) { return v > kDeadZone ? 1 : (v < -kDeadZone ? -1 : 0); }
}  // namespace

const Profile kQuick = {kQuickStages, 4};
const Profile kSlow = {kSlowStages, 2};

uint32_t repeat_delay_at(Profile p, uint32_t elapsed_ms) {
  uint64_t end = 0;
  for (size_t i = 0; i < p.count; ++i) {
    end += p.stages[i].duration_ms;
    if (elapsed_ms < end) return p.stages[i].delay_ms;
  }
  return p.count ? p.stages[p.count - 1].delay_ms : 200;
}

// ---------------------------------------------------------------------------------------------------------------
// es_input.cfg

bool PadConfig::parse(const std::string& xml) {
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return false;
  for (pugi::xml_node n : doc.document_element().children("inputConfig")) {
    if (std::string_view(n.attribute("type").value()) != "joystick") continue;
    pugi::xml_attribute name = n.attribute("deviceName"), guid = n.attribute("deviceGUID");
    if (!name || !guid) continue;
    Device d;
    d.name = name.value();
    d.guid = lower(guid.value());
    for (pugi::xml_node i : n.children("input")) {
      std::string_view t = i.attribute("type").value();
      InKind kind;
      if (t == "button") kind = InKind::Button;
      else if (t == "hat") kind = InKind::Hat;
      else if (t == "axis") kind = InKind::Axis;
      else continue;
      pugi::xml_attribute nm = i.attribute("name"), id = i.attribute("id"), val = i.attribute("value");
      long idv, vv;
      if (!nm || !parse_int(trim(id.value()), idv) || !parse_int(trim(val.value()), vv) || idv < 0 || idv > 0xffff) continue;
      d.inputs.push_back({lower(nm.value()), kind, uint16_t(idv), int(vv)});
    }
    devices_.push_back(std::move(d));
  }
  return true;
}

bool PadConfig::load_file(const std::string& path) {
  std::string xml;
  if (!read_file(path, xml)) return false;
  if (!parse(xml)) {
    std::fprintf(stderr, "zfront: %s: malformed XML\n", path.c_str());
    return false;
  }
  return true;
}

PadConfig PadConfig::load_default() {
  PadConfig c;
  c.load_file("/storage/.config/emulationstation/es_input.cfg");
  char buf[512];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) {
    buf[n] = 0;
    c.load_file(path_join(path_dir(buf), "es_input.cfg"));
  }
  return c;
}

int PadConfig::find(const std::string& guid, const std::string& name) const {
  std::string g = lower(guid), gz = zero_mid(g);
  std::string_view nt = trim(name);
  auto first = [&](auto&& pred) -> int {
    for (size_t i = 0; i < devices_.size(); ++i)
      if (pred(devices_[i])) return int(i);
    return -1;
  };
  int r = first([&](const Device& d) { return d.guid == g && d.name == name; });
  if (r < 0) r = first([&](const Device& d) { return d.name == name && zero_mid(d.guid) == gz; });
  if (r < 0) r = first([&](const Device& d) { return d.guid == g || zero_mid(d.guid) == gz; });
  if (r < 0) r = first([&](const Device& d) { return trim(d.name) == nt; });
  return r;
}

// ---------------------------------------------------------------------------------------------------------------
// PadCore

PadCore::PadCore(PadConfig cfg, bool invert_buttons) : cfg_(std::move(cfg)), invert_(invert_buttons), profile_(kQuick) {}

// ---------------------------------------------------------------------------------------------------------------
// SDL game controller mapping -> cfg entry (reserve mapping for pads ES has no entry for)

namespace {
// ES `_sdlToEsMapping` (InputManager.cpp), Linux variant: a/b and x/y are crossed.
const struct {
  const char* sdl;
  const char* es;
} kSdlToEs[] = {
    {"a", "b"},          {"b", "a"},           {"x", "y"},          {"y", "x"},         {"back", "select"}, {"start", "start"},
    {"leftshoulder", "pageup"}, {"rightshoulder", "pagedown"}, {"dpup", "up"}, {"dpright", "right"}, {"dpdown", "down"},
    {"dpleft", "left"},  {"lefttrigger", "l2"}, {"righttrigger", "r2"}, {"leftstick", "l3"}, {"rightstick", "r3"},
};

// "b7", "h0.1", "a2", "+a1", "-a0", "a3~" (the `~` inverts the axis).
bool parse_sdl_input(std::string_view v, InKind& kind, uint16_t& id, int& value) {
  size_t i = 0;
  int sign = 1;
  if (i < v.size() && (v[i] == '+' || v[i] == '-')) sign = v[i++] == '-' ? -1 : 1;
  if (i >= v.size()) return false;
  char t = v[i++];
  bool inverted = !v.empty() && v.back() == '~';
  if (inverted) v.remove_suffix(1);
  auto num = [&](size_t& pos, long& out) {
    if (pos >= v.size() || v[pos] < '0' || v[pos] > '9') return false;
    out = 0;
    while (pos < v.size() && v[pos] >= '0' && v[pos] <= '9') {
      out = out * 10 + (v[pos++] - '0');
      if (out > 0xffff) return false;
    }
    return true;
  };
  long n = 0, m = 0;
  if (!num(i, n)) return false;
  if (t == 'b') {
    kind = InKind::Button;
    value = 1;
  } else if (t == 'a') {
    kind = InKind::Axis;
    value = inverted ? -sign : sign;
  } else if (t == 'h') {
    if (i >= v.size() || v[i++] != '.' || !num(i, m) || m < 1 || m > 15) return false;
    kind = InKind::Hat;
    value = int(m);
  } else {
    return false;
  }
  id = uint16_t(n);
  return i == v.size();
}
}  // namespace

bool PadConfig::from_sdl_mapping(std::string_view mapping, Device& out) {
  out = Device();
  size_t pos = 0, field = 0;
  while (pos <= mapping.size()) {
    size_t e = mapping.find(',', pos);
    if (e == std::string_view::npos) e = mapping.size();
    std::string_view tok = trim(mapping.substr(pos, e - pos));
    pos = e + 1;
    if (field < 2 && tok.find(':') == std::string_view::npos) {  // guid, name (a name may not contain ':' in practice)
      (field == 0 ? out.guid : out.name) = field == 0 ? lower(std::string(tok)) : std::string(tok);
      ++field;
      continue;
    }
    ++field;
    size_t c = tok.find(':');
    if (c == std::string_view::npos) continue;
    std::string key(tok.substr(0, c));
    InKind kind;
    uint16_t id;
    int value;
    if (!parse_sdl_input(tok.substr(c + 1), kind, id, value)) continue;
    if (key == "leftx" || key == "lefty") {  // an axis becomes two directions; the sign of the value is the polarity
      if (kind != InKind::Axis) continue;
      int pol = value < 0 ? -1 : 1;
      const char* neg = key == "leftx" ? "leftanalogleft" : "leftanalogup";
      const char* pos_name = key == "leftx" ? "leftanalogright" : "leftanalogdown";
      out.inputs.push_back({neg, kind, id, -pol});
      out.inputs.push_back({pos_name, kind, id, pol});
      continue;
    }
    for (const auto& m : kSdlToEs)
      if (key == m.sdl) {
        out.inputs.push_back({m.es, kind, id, value});
        break;
      }
  }
  // ES: "use select button if hotkey is undefined"
  for (const Input& i : std::vector<Input>(out.inputs))
    if (i.name == "select") {
      out.inputs.push_back({"hotkeyenable", i.kind, i.id, i.value});
      break;
    }
  return !out.inputs.empty();
}

void PadCore::compile(Dev& d) const {
  d.binds.clear();
  const PadConfig::Device* cd = d.cfg >= 0 ? &cfg_.device(size_t(d.cfg)) : (d.has_sdl ? &d.sdl : nullptr);
  if (!cd) return;
  // hotkeyenable last: when it shares the button with select (retrogame_joypad) select wins. Two names for the same
  // input and action (leftshoulder + pageup on one button) are bound once.
  for (int hot = 0; hot < 2; ++hot) {
    for (const PadConfig::Input& i : cd->inputs) {
      if ((i.name == "hotkeyenable") != (hot == 1)) continue;
      std::optional<Action> a = name_action(i.name, invert_);
      if (!a) continue;
      int value = i.kind == InKind::Axis ? (i.value > 0) - (i.value < 0) : i.value;
      bool dup = std::any_of(d.binds.begin(), d.binds.end(), [&](const Bind& b) { return b.kind == i.kind && b.id == i.id && b.value == value && (hot || b.action == *a); });
      if (!dup) d.binds.push_back({i.kind, i.id, value, *a});
    }
  }
}

void PadCore::set_invert_buttons(bool invert) {
  invert_ = invert;
  for (Dev& d : devs_)
    if (d.used) compile(d);
}

DevId PadCore::add_device(const std::string& guid, const std::string& name, const int16_t* axis_init, size_t n_axes) {
  size_t slot = devs_.size();
  for (size_t i = 0; i < devs_.size(); ++i)
    if (!devs_[i].used) {
      slot = i;
      break;
    }
  if (slot == devs_.size()) devs_.emplace_back();
  Dev& d = devs_[slot];
  d = Dev();
  d.used = true;
  d.guid = guid;
  d.name = name;
  d.cfg = cfg_.find(guid, name);
  for (size_t i = 0; i < n_axes && i < kMaxAxes; ++i) d.axis_init[i] = axis_init[i];
  compile(d);
  return slot;
}

void PadCore::remove_device(DevId id) {
  if (id < devs_.size()) devs_[id].used = false;
  if (hold_ && hold_->src.dev == id) hold_.reset();
}

const std::string& PadCore::device_name(DevId id) const {
  static const std::string none;
  return id < devs_.size() && devs_[id].used ? devs_[id].name : none;
}

bool PadCore::is_configured(DevId id) const { return id < devs_.size() && devs_[id].used && devs_[id].cfg >= 0; }

bool PadCore::set_sdl_mapping(DevId id, std::string_view mapping) {
  if (id >= devs_.size() || !devs_[id].used || devs_[id].cfg >= 0) return false;
  Dev& d = devs_[id];
  d.has_sdl = PadConfig::from_sdl_mapping(mapping, d.sdl);
  compile(d);
  return d.has_sdl && !d.binds.empty();
}

int PadCore::mapping_source(DevId id) const {
  if (id >= devs_.size() || !devs_[id].used) return 0;
  return devs_[id].cfg >= 0 ? 1 : (devs_[id].has_sdl && !devs_[id].binds.empty() ? 2 : 0);
}

void PadCore::clear() {
  hold_.reset();
  for (Dev& d : devs_) {
    std::fill(std::begin(d.axis_prev), std::end(d.axis_prev), 0);
    std::fill(std::begin(d.hat_prev), std::end(d.hat_prev), uint8_t(0));
  }
}

void PadCore::press(Src s, Action a, TimePoint now, std::vector<Action>& out) {
  out.push_back(a);
  if (action_repeats(a)) {
    uint32_t d = repeat_delay_at(profile_, 0);
    hold_ = Hold{s, a, now, now + std::chrono::milliseconds(d)};
  }
}

void PadCore::release(Src s, Action a) {
  if (hold_ && hold_->src == s && hold_->action == a) hold_.reset();
}

void PadCore::set_capture(CaptureSink sink) {
  capture_ = std::move(sink);
  hold_.reset();  // a direction held when the wizard opens must not keep repeating into it
}

void PadCore::set_config(PadConfig cfg) {
  cfg_ = std::move(cfg);
  for (Dev& d : devs_) {
    if (!d.used) continue;
    d.cfg = cfg_.find(d.guid, d.name);
    compile(d);
  }
}

// Offers the decoded event to the capture sink. True when it consumed the event; the resting/previous state of the axis or
// hat is updated then, so nothing reads as pressed when the sink goes away. When it declines (or nothing crossed the dead
// zone) the normal path below does its own bookkeeping.
bool PadCore::capture_event(const RawEvent& e, Dev& d) {
  CaptureEvent ce{e.dev, InKind::Button, e.id, 0, &d.guid, &d.name};
  switch (e.kind) {
    case RawEvent::Button:
      ce.kind = InKind::Button;
      ce.value = e.value ? 1 : 0;
      return capture_(ce);
    case RawEvent::Hat: {
      if (e.id >= kMaxHats) return false;
      ce.kind = InKind::Hat;
      ce.value = e.value;
      if (d.hat_prev[e.id] == uint8_t(e.value)) return false;
      bool taken = capture_(ce);
      if (taken) d.hat_prev[e.id] = uint8_t(e.value);
      return taken;
    }
    case RawEvent::Axis: {
      if (e.id >= kMaxAxes) return false;
      int32_t cur = e.value - d.axis_init[e.id];
      int s0 = zone(d.axis_prev[e.id]), s1 = zone(cur);
      if (s0 == s1) return false;
      ce.kind = InKind::Axis;
      ce.value = s1;
      bool taken = capture_(ce);
      if (taken) d.axis_prev[e.id] = cur;
      return taken;
    }
  }
  return false;
}

void PadCore::event(const RawEvent& e, TimePoint now, std::vector<Action>& out) {
  if (e.dev >= devs_.size() || !devs_[e.dev].used) return;
  Dev& d = devs_[e.dev];
  if (capture_ && capture_event(e, d)) return;
  // ES `ViewController::input`: a button or hat press from a pad with no mapping at all suggests the configuration
  if (!capture_ && d.binds.empty() && ((e.kind == RawEvent::Button && e.value) || (e.kind == RawEvent::Hat && e.value && e.id < kMaxHats && d.hat_prev[e.id] == 0))) unconfigured_ = true;
  switch (e.kind) {
    case RawEvent::Button: {
      Src s{e.dev, InKind::Button, e.id};
      for (const Bind& b : d.binds) {
        if (b.kind != InKind::Button || b.id != e.id) continue;
        if (e.value) press(s, b.action, now, out);
        else release(s, b.action);
      }
      break;
    }
    case RawEvent::Hat: {
      if (e.id >= kMaxHats) return;
      uint8_t old = d.hat_prev[e.id], nw = uint8_t(e.value);
      d.hat_prev[e.id] = nw;
      Src s{e.dev, InKind::Hat, e.id};
      for (const Bind& b : d.binds) {
        if (b.kind != InKind::Hat || b.id != e.id) continue;
        bool was = old & b.value, is = nw & b.value;
        if (!was && is) press(s, b.action, now, out);
        else if (was && !is) release(s, b.action);
      }
      break;
    }
    case RawEvent::Axis: {
      if (e.id >= kMaxAxes) return;
      int32_t cur = e.value - d.axis_init[e.id];
      int s0 = zone(d.axis_prev[e.id]), s1 = zone(cur);
      d.axis_prev[e.id] = cur;
      if (s0 == s1) return;
      Src s{e.dev, InKind::Axis, e.id};
      for (const Bind& b : d.binds) {
        if (b.kind != InKind::Axis || b.id != e.id) continue;
        if (b.value == s0) release(s, b.action);
        if (b.value == s1) press(s, b.action, now, out);
      }
      break;
    }
  }
}

void PadCore::repeat_due(TimePoint now, std::vector<Action>& out) {
  if (!hold_) return;
  Hold& h = *hold_;
  int n = 0;
  while (h.next <= now) {
    if (n == 4) {  // loop was stalled: resynchronise instead of bursting
      h.next = now + std::chrono::milliseconds(1);
      break;
    }
    out.push_back(h.action);
    ++n;
    auto el = std::chrono::duration_cast<std::chrono::milliseconds>(h.next - h.t0).count();
    h.next += std::chrono::milliseconds(repeat_delay_at(profile_, uint32_t(el)));
  }
}

// ---------------------------------------------------------------------------------------------------------------
// SDL glue (SDL_Joystick API for the events; SDL_GameController only supplies the mapping string of pads that
// es_input.cfg does not know, and is never opened)

#ifdef ZF_WITH_SDL
namespace {

class SdlPad final : public PadCore {
 public:
  using PadCore::PadCore;
  ~SdlPad() override {
    for (Open& o : open_) SDL_JoystickClose(o.js);
  }

  void attach() override { resync(); }

  bool on_event(const void* ev, TimePoint now, std::vector<Action>& out) override {
    const SDL_Event& e = *static_cast<const SDL_Event*>(ev);
    switch (e.type) {
      case SDL_JOYDEVICEADDED:
        if (add(e.jdevice.which) && plug_) notify(true, open_.back().dev);
        break;
      case SDL_JOYDEVICEREMOVED: {
        std::string name;
        if (const Open* o = find(e.jdevice.which)) name = device_name(o->dev);
        if (drop(e.jdevice.which) && plug_ && !name.empty()) notify_name(false, name);
        break;
      }
      case SDL_JOYBUTTONDOWN:
      case SDL_JOYBUTTONUP:
        if (const Open* o = find(e.jbutton.which)) event({RawEvent::Button, o->dev, e.jbutton.button, e.type == SDL_JOYBUTTONDOWN}, now, out);
        break;
      case SDL_JOYHATMOTION:
        if (const Open* o = find(e.jhat.which)) event({RawEvent::Hat, o->dev, e.jhat.hat, e.jhat.value}, now, out);
        break;
      case SDL_JOYAXISMOTION:
        if (const Open* o = find(e.jaxis.which)) event({RawEvent::Axis, o->dev, e.jaxis.axis, e.jaxis.value}, now, out);
        break;
      default: return false;
    }
    return true;
  }

  // Before SDL_Quit (a game is about to run): close every joystick so no handle is left dangling.
  void detach() override {
    while (!open_.empty()) drop(open_.back().instance);
    PadCore::clear();
  }

  // Events typed while a game ran were flushed by the loop, including hot-plug ones: reconcile with SDL instead.
  void clear() override {
    PadCore::clear();
    resync();
  }

  std::vector<JoyInfo> joysticks() const override {
    std::vector<JoyInfo> v;
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; ++i) {
      char g[40];
      SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), g, sizeof g);
      const char* nm = SDL_JoystickNameForIndex(i);
      JoyInfo ji{uint32_t(i), g, nm ? nm : "", -1, -1, -1};
      if (const Open* o = find(SDL_JoystickGetDeviceInstanceID(i))) {
        ji.buttons = SDL_JoystickNumButtons(o->js);
        ji.hats = SDL_JoystickNumHats(o->js);
        ji.axes = SDL_JoystickNumAxes(o->js);
      }
      v.push_back(std::move(ji));
    }
    return v;
  }

 private:
  struct Open {
    SDL_JoystickID instance;
    DevId dev;
    SDL_Joystick* js;
  };

  const Open* find(SDL_JoystickID inst) const {
    for (const Open& o : open_)
      if (o.instance == inst) return &o;
    return nullptr;
  }

  void notify(bool added, DevId dev) { notify_name(added, device_name(dev)); }
  void notify_name(bool added, const std::string& raw) {
    std::string_view t = trim(raw);
    if (!t.empty()) plug_(added, std::string(t));
  }

  // true when a new device was opened
  bool add(int index) {
    SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(index);
    if (inst < 0 || find(inst)) return false;
    SDL_Joystick* js = SDL_JoystickOpen(index);
    if (!js) return false;
    char g[40];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(js), g, sizeof g);
    const char* nm = SDL_JoystickName(js);
    int na = std::min(SDL_JoystickNumAxes(js), 16);
    int16_t init[16] = {};
    for (int a = 0; a < na; ++a) {
      Sint16 v = 0;
      if (!SDL_JoystickGetAxisInitialState(js, a, &v)) v = SDL_JoystickGetAxis(js, a);
      init[a] = v;
    }
    DevId dev = add_device(g, nm ? nm : "", init, size_t(na));
    // no es_input.cfg entry: reserve mapping from SDL's game controller database (needs SDL_INIT_GAMECONTROLLER)
    if (!is_configured(dev) && SDL_IsGameController(index)) {
      if (char* m = SDL_GameControllerMappingForDeviceIndex(index)) {
        set_sdl_mapping(dev, m);
        SDL_free(m);
      }
    }
    static const char* const kSrc[] = {"NONE (the pad will not work)", "es_input.cfg", "SDL mapping"};
    std::fprintf(stderr, "zfront: joystick %d '%s' %s (mapping: %s)\n", index, nm ? nm : "", g, kSrc[mapping_source(dev)]);
    open_.push_back({inst, dev, js});
    return true;
  }

  bool drop(SDL_JoystickID inst) {
    for (size_t i = 0; i < open_.size(); ++i) {
      if (open_[i].instance != inst) continue;
      remove_device(open_[i].dev);
      SDL_JoystickClose(open_[i].js);
      open_.erase(open_.begin() + long(i));
      return true;
    }
    return false;
  }

  // Open what is present and not open yet, close what disappeared.
  void resync() {
    for (size_t i = open_.size(); i-- > 0;)
      if (!SDL_JoystickGetAttached(open_[i].js)) drop(open_[i].instance);
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; ++i) add(i);
  }

  std::vector<Open> open_;
};

}  // namespace
#endif

std::unique_ptr<Pad> make_pad(const std::string& es_settings_path) {
  bool invert = EsSettings::load(es_settings_path).get_bool("InvertButtons", true);
  PadConfig cfg = PadConfig::load_default();
#ifdef ZF_WITH_SDL
  return std::make_unique<SdlPad>(std::move(cfg), invert);
#else
  return std::make_unique<PadCore>(std::move(cfg), invert);
#endif
}

}  // namespace zf
