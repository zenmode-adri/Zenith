#include "players.hpp"

#include <algorithm>
#include <numeric>

namespace zf::players {

namespace {
Slots g_slots;

std::string key(size_t player, const char* what) { return "INPUT P" + std::to_string(player + 1) + what; }
std::string value_of(const EsSettings& es, const std::string& k) {
  const std::string* v = es.get(k);
  return v ? *v : std::string();
}
}  // namespace

Slots read(const EsSettings& es) {
  Slots s;
  for (size_t p = 0; p < kSlots; ++p) {
    s[p].name = value_of(es, key(p, "NAME"));
    s[p].guid = value_of(es, key(p, "GUID"));
    s[p].path = value_of(es, key(p, "PATH"));
  }
  return s;
}

void set(const Slots& s) { g_slots = s; }
const Slots& current() { return g_slots; }

std::vector<JoyInfo> order(const std::vector<JoyInfo>& joys, const Slots& slots) {
  std::vector<size_t> avail(joys.size());
  std::iota(avail.begin(), avail.end(), size_t(0));
  // ES sorts by `getSortDevicePath()`: the udev parent of the device, which zfront cannot compute (and sorting the raw /dev/input/eventN
  // strings would put event10 before event2), so the free pads keep the SDL connection order, as zfront always did
  std::stable_sort(avail.begin(), avail.end(), [&](size_t a, size_t b) { return joys[a].index < joys[b].index; });

  std::array<long, kSlots + 1> pl;  // the last one is never filled: ES reads it too when it moves pads into holes
  pl.fill(-1);
  auto take = [&](size_t p, auto match) {
    for (auto it = avail.begin(); it != avail.end(); ++it)
      if (match(joys[*it])) {
        pl[p] = long(*it);
        avail.erase(it);
        return;
      }
  };
  for (size_t p = 0; p < kSlots; ++p)
    if (!slots[p].path.empty()) take(p, [&](const JoyInfo& j) { return j.path == slots[p].path; });
  for (size_t p = 0; p < kSlots; ++p)
    if (pl[p] < 0 && !slots[p].name.empty()) take(p, [&](const JoyInfo& j) { return j.name == slots[p].name && j.guid == slots[p].guid; });
  for (size_t p = 0; p < kSlots; ++p)
    if (pl[p] < 0 && !slots[p].name.empty()) take(p, [&](const JoyInfo& j) { return j.name == slots[p].name; });
  for (size_t p = 0; p < kSlots; ++p)
    if (pl[p] < 0) take(p, [](const JoyInfo&) { return true; });
  for (size_t p = 0; p < kSlots; ++p)
    for (size_t rep = kSlots; rep > p; --rep)
      if (pl[p] < 0 && pl[rep] >= 0) {
        pl[p] = pl[rep];
        pl[rep] = -1;
      }

  std::vector<JoyInfo> out;
  for (size_t p = 0; p < kSlots; ++p)
    if (pl[p] >= 0) out.push_back(joys[size_t(pl[p])]);
  return out;
}

std::vector<int> chosen(const std::vector<JoyInfo>& joys, const Slots& slots) {
  std::vector<int> out(kSlots, -1);
  std::vector<bool> taken(joys.size(), false);
  for (size_t p = 0; p < kSlots; ++p) {
    const Slot& s = slots[p];
    for (size_t i = 0; i < joys.size(); ++i) {
      // ES: the path alone when there is one. A path saved by ES may be a udev parent path that zfront never sees, so a pad that does not
      // match any saved path is looked up by name + GUID as well (the same second step `order` takes)
      const bool match = (!s.path.empty() && joys[i].path == s.path) || (!s.name.empty() && s.name == joys[i].name && s.guid == joys[i].guid);
      if (match && !taken[i]) {
        taken[i] = true;
        out[p] = int(i);
        break;
      }
    }
  }
  return out;
}

bool assign(EsSettings& es, size_t player, const JoyInfo* pad) {
  if (player >= kSlots) return false;
  const std::string name = pad ? pad->name : std::string("DEFAULT"), guid = pad ? pad->guid : std::string(), path = pad ? pad->path : std::string();
  const Slot old = read(es)[player];
  if (old.name == name && old.guid == guid && old.path == path) return false;
  es.set_string(key(player, "NAME"), name);
  es.set_string(key(player, "GUID"), guid);
  es.set_string(key(player, "PATH"), path);
  return true;
}

}  // namespace zf::players
