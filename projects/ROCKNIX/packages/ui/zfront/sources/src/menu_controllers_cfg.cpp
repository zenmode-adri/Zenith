#include "menu_controllers_cfg.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <pugixml.hpp>

#include "settings.hpp"
#include "util.hpp"

namespace zf::ctl {

Paths& paths() {
  static Paths p;
  return p;
}

// ---------------------------------------------------------------------------------------------------------------
// es_input.cfg

namespace {

const char* type_name(Mapping::Type t) {
  switch (t) {
    case Mapping::Button: return "button";
    case Mapping::Hat: return "hat";
    case Mapping::Axis: return "axis";
    case Mapping::Key: return "key";
  }
  return "button";
}

struct StrWriter : pugi::xml_writer {
  std::string out;
  void write(const void* data, size_t size) override { out.append(static_cast<const char*>(data), size); }
};

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = char(x + 32);
    if (y >= 'A' && y <= 'Z') y = char(y + 32);
    if (x != y) return false;
  }
  return true;
}

// ES InputConfig::writeToXML: attribute order type, deviceName, deviceGUID; inputs in the order of a std::map<name>.
void append_config(pugi::xml_node root, const std::string& guid, const std::string& name, const std::vector<Mapping>& maps) {
  pugi::xml_node cfg = root.append_child("inputConfig");
  cfg.append_attribute("type") = "joystick";
  cfg.append_attribute("deviceName") = name.c_str();
  cfg.append_attribute("deviceGUID") = guid.c_str();
  for (const Mapping& m : maps) {
    pugi::xml_node in = cfg.append_child("input");
    in.append_attribute("name") = m.name.c_str();
    in.append_attribute("type") = type_name(m.type);
    in.append_attribute("id").set_value(m.id);
    in.append_attribute("value").set_value(m.value);
  }
}

bool save_doc(const pugi::xml_document& doc, const std::string& path, std::string& err) {
  StrWriter w;
  doc.save(w);  // default flags like save_file: tab indent, `<?xml version="1.0"?>` declaration
  return write_atomic(path, w.out, &err);
}

}  // namespace

bool write_device_config(const std::string& path, const std::string& guid, const std::string& name, std::vector<Mapping> maps, std::string& err) {
  std::stable_sort(maps.begin(), maps.end(), [](const Mapping& a, const Mapping& b) { return a.name < b.name; });
  // one entry per name, the last wins (a map keyed by name)
  for (size_t i = 0; i + 1 < maps.size();) {
    if (maps[i].name == maps[i + 1].name) maps.erase(maps.begin() + long(i));
    else ++i;
  }

  pugi::xml_document doc;
  if (file_exists(path)) {
    pugi::xml_parse_result r = doc.load_file(path.c_str());
    if (!r) {
      if (!writes_blocked()) std::rename(path.c_str(), (path + ".bad").c_str());  // keep what we could not read
      doc.reset();
    }
  }
  pugi::xml_node root = doc.child("inputList");
  if (!root) root = doc.append_child("inputList");
  for (pugi::xml_node c = root.child("inputConfig"); c;) {
    pugi::xml_node next = c.next_sibling("inputConfig");
    if (ieq(c.attribute("deviceGUID").value(), guid) && name == c.attribute("deviceName").value()) root.remove_child(c);
    c = next;
  }
  append_config(root, guid, name, maps);
  if (!save_doc(doc, path, err)) return false;

  // ES also keeps the last configuration alone in es_last_input.cfg so people can share it
  pugi::xml_document last;
  append_config(last.append_child("inputList"), guid, name, maps);
  std::string e2;
  save_doc(last, path_join(path_dir(path), "es_last_input.cfg"), e2);  // best effort
  return true;
}

std::vector<std::string> onfinish_commands(const std::string& path) {
  std::vector<std::string> out;
  pugi::xml_document doc;
  if (!file_exists(path) || !doc.load_file(path.c_str())) return out;
  pugi::xml_node root = doc.child("inputList").find_child_by_attribute("inputAction", "type", "onfinish");
  for (pugi::xml_node c = root.child("command"); c; c = c.next_sibling("command")) out.emplace_back(c.text().get());
  return out;
}

std::vector<Mapping> read_device_config(const std::string& path, const std::string& guid, const std::string& name) {
  std::vector<Mapping> out;
  pugi::xml_document doc;
  if (!file_exists(path) || !doc.load_file(path.c_str())) return out;
  for (pugi::xml_node c : doc.child("inputList").children("inputConfig")) {
    if (!ieq(c.attribute("deviceGUID").value(), guid) || name != c.attribute("deviceName").value()) continue;
    for (pugi::xml_node i : c.children("input")) {
      Mapping m;
      std::string_view t = i.attribute("type").value();
      if (t == "button") m.type = Mapping::Button;
      else if (t == "hat") m.type = Mapping::Hat;
      else if (t == "axis") m.type = Mapping::Axis;
      else if (t == "key") m.type = Mapping::Key;
      else continue;
      m.name = i.attribute("name").value();
      m.id = i.attribute("id").as_int();
      m.value = i.attribute("value").as_int();
      out.push_back(std::move(m));
    }
    break;
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// wizard

const Slot kSlots[kSlotCount] = {
    {"up", NC_("joystick", "D-PAD UP"), false},
    {"down", NC_("joystick", "D-PAD DOWN"), false},
    {"left", NC_("joystick", "D-PAD LEFT"), false},
    {"right", NC_("joystick", "D-PAD RIGHT"), false},
    {"start", NC_("joystick", "START"), true},
    {"select", NC_("joystick", "SELECT"), true},
    {"a", NC_("joystick", "BUTTON A / EAST"), false},
    {"b", NC_("joystick", "BUTTON B / SOUTH"), true},
    {"x", NC_("joystick", "BUTTON X / NORTH"), true},
    {"y", NC_("joystick", "BUTTON Y / WEST"), true},
    {"leftshoulder", NC_("joystick", "LEFT SHOULDER"), true},
    {"rightshoulder", NC_("joystick", "RIGHT SHOULDER"), true},
    {"lefttrigger", NC_("joystick", "LEFT TRIGGER"), true},
    {"righttrigger", NC_("joystick", "RIGHT TRIGGER"), true},
    {"leftthumb", NC_("joystick", "LEFT THUMB"), true},
    {"rightthumb", NC_("joystick", "RIGHT THUMB"), true},
    {"leftanalogup", NC_("joystick", "LEFT ANALOG UP"), true},
    {"leftanalogdown", NC_("joystick", "LEFT ANALOG DOWN"), true},
    {"leftanalogleft", NC_("joystick", "LEFT ANALOG LEFT"), true},
    {"leftanalogright", NC_("joystick", "LEFT ANALOG RIGHT"), true},
    {"hotkeyenable", NC_("joystick", "HOTKEY ENABLE"), true},
    {"rightanalogup", NC_("joystick", "RIGHT ANALOG UP"), true},
    {"rightanalogdown", NC_("joystick", "RIGHT ANALOG DOWN"), true},
    {"rightanalogleft", NC_("joystick", "RIGHT ANALOG LEFT"), true},
    {"rightanalogright", NC_("joystick", "RIGHT ANALOG RIGHT"), true},
};

namespace {
constexpr size_t kDown = 1, kA = 6, kD = 9, kHotkey = 20, kSelect = 5;
}

InputWizard::InputWizard(int buttons, int hats, int axes) {
  for (size_t i = 0; i < kSlotCount; ++i) {
    st_[i] = RowState::Pending;
    map_[i].name = kSlots[i].name;
    skip_[i] = kSlots[i].skippable;
  }
  // GuiInputConfig::initInputConfigStructure (only for a real joystick, i.e. when the counts are known). The indices are
  // ES's: [1] is "Down" and [6..9] are a, b, x, y.
  if (buttons >= 0 && hats >= 0 && axes >= 0) {
    skip_[kDown] = buttons <= 1 || (buttons == 5 && axes == 0 && hats == 0);
    for (size_t i = kA; i <= kD; ++i) skip_[i] = hats == 0;
  }
  changed_ = true;
}

float InputWizard::hold_progress() const {
  if (!holding_ || phase_ != Phase::Rows || !skip_[row_]) return 0;
  return std::min(held_s_ / kHoldSeconds, 1.0f);
}

bool InputWizard::matches(const Mapping& have, InKind kind, uint16_t id, int value) const {
  Mapping::Type t = kind == InKind::Button ? Mapping::Button : (kind == InKind::Hat ? Mapping::Hat : Mapping::Axis);
  if (have.type != t || have.id != id) return false;
  if (t == Mapping::Hat) return value == 0 || (value & have.value) != 0;
  if (t == Mapping::Axis) return value == 0 || have.value == value;
  return true;
}

// GuiInputConfig::assign: refused when the input is mapped to another row (hotkeyenable may share, e.g. with select).
bool InputWizard::assign(const Mapping& m) {
  InKind kind = m.type == Mapping::Button ? InKind::Button : (m.type == Mapping::Hat ? InKind::Hat : InKind::Axis);
  bool any = false, self = false;
  for (size_t i = 0; i < kSlotCount; ++i) {
    if (st_[i] != RowState::Set || !matches(map_[i], kind, uint16_t(m.id), m.value)) continue;
    any = true;
    if (i == row_) self = true;
  }
  if (any && !self && row_ != kHotkey) return false;
  map_[row_] = m;
  map_[row_].name = kSlots[row_].name;
  st_[row_] = RowState::Set;
  return true;
}

void InputWizard::row_done() {
  holding_ = false;
  held_s_ = 0;
  seen_.clear();
  if (row_ + 1 >= kSlotCount) {
    phase_ = Phase::Review;
  } else {
    ++row_;
  }
  changed_ = true;
}

void InputWizard::input(InKind kind, uint16_t id, int value) {
  if (phase_ != Phase::Rows) return;
  if (holding_) seen_.push_back({kind, id, value});
  if (value != 0) {  // down
    if (holding_) return;
    holding_ = true;
    held_s_ = 0;
    held_.type = kind == InKind::Button ? Mapping::Button : (kind == InKind::Hat ? Mapping::Hat : Mapping::Axis);
    held_.id = id;
    held_.value = kind == InKind::Button ? 1 : value;
    seen_.clear();
    seen_.push_back({kind, id, value});
    taken_ = false;
    changed_ = true;
    return;
  }
  // up: only the release of what we are holding counts
  if (!holding_ || held_.id != id || (kind == InKind::Button) != (held_.type == Mapping::Button) || (kind == InKind::Hat) != (held_.type == Mapping::Hat)) return;
  holding_ = false;
  held_s_ = 0;
  changed_ = true;
  if (held_.type == Mapping::Button) {
    // a button held together with a moving axis (two or more events, all on the same axis) is that axis
    size_t n = 0;
    uint16_t first_id = 0;
    bool same = true;
    const Seen* first = nullptr;
    for (const Seen& s : seen_)
      if (s.kind == InKind::Axis) {
        if (!n) first_id = s.id, first = &s;
        else if (s.id != first_id) same = false;
        ++n;
      }
    if (n >= 2 && same && first) {
      held_.type = Mapping::Axis;
      held_.id = first->id;
      held_.value = first->value;
    }
  }
  if (assign(held_)) {
    taken_ = false;
    row_done();
  } else {
    taken_ = true;
    seen_.clear();
  }
}

void InputWizard::tick(float dt) {
  if (phase_ != Phase::Rows || !holding_ || !skip_[row_]) return;
  held_s_ += dt;
  changed_ = true;
  if (held_s_ >= kHoldSeconds) {
    st_[row_] = RowState::Skipped;
    row_done();
  }
}

bool InputWizard::skip_row() {
  if (phase_ != Phase::Rows || !skip_[row_]) return false;
  st_[row_] = RowState::Skipped;
  taken_ = false;
  row_done();
  return true;
}

bool InputWizard::back_row() {
  if (row_ == 0 && phase_ == Phase::Rows) return false;
  if (phase_ == Phase::Review) phase_ = Phase::Rows;
  else --row_;
  st_[row_] = RowState::Pending;
  holding_ = false;
  taken_ = false;
  held_s_ = 0;
  seen_.clear();
  changed_ = true;
  return true;
}

const char* InputWizard::resolve(InKind kind, uint16_t id, int value) const {
  if (value == 0) return nullptr;
  for (size_t i = 0; i < kSlotCount; ++i)
    if (st_[i] == RowState::Set && matches(map_[i], kind, id, value)) return kSlots[i].name;
  return nullptr;
}

std::vector<Mapping> InputWizard::result(Hotkey choice) const {
  std::vector<Mapping> out;
  for (size_t i = 0; i < kSlotCount; ++i)
    if (st_[i] == RowState::Set) out.push_back(map_[i]);
  if (!has_hotkey()) {
    Mapping h;
    h.name = kSlots[kHotkey].name;
    if (choice == Hotkey::Select && has_select()) {
      h = map_[kSelect];
      h.name = kSlots[kHotkey].name;
    } else {  // "DO NOT ASSIGN HOTKEY": a key with id 0, which nothing can press
      h.type = Mapping::Key;
      h.id = 0;
      h.value = 1;
    }
    out.push_back(std::move(h));
  }
  return out;
}

size_t InputWizard::count(RowState s) const {
  size_t n = 0;
  for (size_t i = 0; i < kSlotCount; ++i) n += st_[i] == s;
  return n;
}

std::string describe(const Mapping& m) {
  switch (m.type) {
    case Mapping::Button: return "Button " + std::to_string(m.id);
    case Mapping::Axis: return "Axis " + std::to_string(m.id) + (m.value > 0 ? "+" : "-");
    case Mapping::Hat: {
      const char* d = m.value == 1 ? "Up" : m.value == 2 ? "Right" : m.value == 4 ? "Down" : m.value == 8 ? "Left" : "Unknown";
      return "Hat " + std::to_string(m.id) + " " + d;
    }
    case Mapping::Key: return "Key";
  }
  return {};
}

// ---------------------------------------------------------------------------------------------------------------
// bluetooth

namespace {
// ES Utils::String::extractString(s, "name=\"", "\""): text after the first occurrence of the prefix up to the next quote
std::string attr(std::string_view line, std::string_view key) {
  std::string pat(key);
  pat += "=\"";
  size_t p = 0;
  for (;;) {
    p = line.find(pat, p);
    if (p == std::string_view::npos) return {};
    if (p == 0 || line[p - 1] == ' ') break;  // not the tail of another attribute name
    p += pat.size();
  }
  p += pat.size();
  size_t e = line.find('"', p);
  if (e == std::string_view::npos) return {};
  return std::string(line.substr(p, e - p));
}

template <class F>
void each_device_line(std::string_view text, F&& f) {
  size_t pos = 0;
  while (pos < text.size()) {
    size_t e = text.find('\n', pos);
    if (e == std::string_view::npos) e = text.size();
    std::string_view line = trim(text.substr(pos, e - pos));
    pos = e + 1;
    if (starts_with(line, "<device ")) f(line);
  }
}
}  // namespace

std::vector<BtDevice> parse_bt_list(std::string_view text) {
  std::vector<BtDevice> v;
  each_device_line(text, [&](std::string_view line) {
    BtDevice d;
    d.id = attr(line, "id");
    d.name = attr(line, "name");
    d.type = attr(line, "type");
    d.connected = attr(line, "connected") == "yes";
    if (d.type.empty()) d.type = "unknown";
    if (!d.id.empty()) v.push_back(std::move(d));
  });
  return v;
}

void apply_bt_live(std::vector<BtDevice>& devs, std::string_view text) {
  each_device_line(text, [&](std::string_view line) {
    std::string id = attr(line, "id");
    if (id.empty()) return;
    auto it = std::find_if(devs.begin(), devs.end(), [&](const BtDevice& d) { return d.id == id; });
    if (attr(line, "status") == "removed") {
      if (it != devs.end()) devs.erase(it);
      return;
    }
    BtDevice d;
    d.id = id;
    d.name = attr(line, "name");
    d.type = attr(line, "type");
    if (d.type.empty()) d.type = "unknown";
    if (it != devs.end()) *it = std::move(d);
    else devs.push_back(std::move(d));
  });
}

bool valid_mac(std::string_view s) {
  if (s.size() != 17) return false;
  for (size_t i = 0; i < 17; ++i) {
    char c = s[i];
    if (i % 3 == 2) {
      if (c != ':') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// probes

std::string rumble_path() {
  DIR* d = ::opendir(paths().sysfs_platform.c_str());
  if (!d) return {};
  std::string found;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    std::string low = n;
    for (char& c : low)
      if (c >= 'A' && c <= 'Z') c = char(c + 32);
    if (low.find("retroid-pocket-gamepad") != std::string::npos || low.find("rocknix") != std::string::npos) {
      found = path_join(paths().sysfs_platform, n);
      break;
    }
  }
  ::closedir(d);
  if (found.empty()) return {};
  found = path_join(found, "rumble_enable");
  return file_exists(found) ? found : std::string();
}

bool bt_available() { return file_exists(paths().bt_tool); }

}  // namespace zf::ctl
