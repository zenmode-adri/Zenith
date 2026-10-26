// menu-game: es_features.cfg, the file that tells which options each emulator / core / system offers. A port of the logic
// of ES (`CustomFeatures::loadEsFeaturesFile`, `SystemData::loadFeatures/isFeatureSupported/getCustomFeatures`), reading
// the same files so both frontends offer the same options.
#include <dirent.h>

#include <algorithm>
#include <mutex>
#include <pugixml.hpp>

#include "menu_game.hpp"
#include "util.hpp"

namespace zf::mg {

namespace {

std::vector<std::string> split_list(std::string_view s, char sep) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i <= s.size()) {
    size_t e = s.find(sep, i);
    if (e == std::string_view::npos) e = s.size();
    out.emplace_back(trim(s.substr(i, e - i)));
    i = e + 1;
  }
  return out;
}

std::string underscored(std::string s) {
  std::replace(s.begin(), s.end(), ' ', '_');
  return s;
}

bool any_value(const std::vector<Feature>& v, const char* value) {
  return std::any_of(v.begin(), v.end(), [&](const Feature& f) { return f.value == value; });
}

template <class T>
T* find_named(std::vector<T>& v, const std::string& name) {
  for (T& x : v)
    if (x.name == name) return &x;
  return nullptr;
}

void append(std::vector<Feature>& to, const std::vector<Feature>& from) { to.insert(to.end(), from.begin(), from.end()); }

}  // namespace

bool Resolved::has_custom(const std::string& value) const {
  return std::any_of(custom.begin(), custom.end(), [&](const Feature& f) { return f.value == value; });
}

bool FeatureDb::has_global(const std::string& name) const {
  return std::any_of(global_.begin(), global_.end(), [&](const Feature& f) { return f.value == name || f.value == "global." + name; });
}

uint32_t FeatureDb::parse_flags(const std::string& list) const {
  struct Item {
    const char* name;
    uint32_t flag;
    bool overridable;  // a shared feature of the same value replaces the built-in option
  };
  static const Item kItems[] = {
      {"autosave", F_autosave, false},   {"netplay", F_netplay, false},      {"cheevos", F_cheevos, false},
      {"padtokeyboard", F_padtokeyboard, false}, {"joystick2pad", F_padtokeyboard, false},
      {"ratio", F_ratio, true},          {"rewind", F_rewind, true},          {"smooth", F_smooth, true},
      {"shaders", F_shaders, true},      {"videofilters", F_videofilters, true}, {"pixel_perfect", F_pixel_perfect, true},
      {"decoration", F_decoration, true}, {"latency_reduction", F_latency_reduction, true}, {"game_translation", F_game_translation, true},
      {"fullboot", F_fullboot, true},    {"emulated_wiimotes", F_emulated_wiimotes, true}, {"screen_layout", F_screen_layout, true},
      {"internal_resolution", F_internal_resolution, true}, {"videomode", F_videomode, true}, {"colorization", F_colorization, true},
      {"autocontrollers", F_autocontrollers, true},
  };
  uint32_t r = 0;
  for (const std::string& n : split_list(list, ',')) {
    for (const Item& it : kItems) {
      if (n != it.name) continue;
      if (it.overridable && any_value(shared_, it.name)) break;
      r |= it.flag;
      break;
    }
  }
  return r;
}

std::vector<Feature> FeatureDb::parse_features(const pugi::xml_node& node) const {
  std::vector<Feature> ret;
  if (pugi::xml_attribute fa = node.attribute("features")) {
    for (const std::string& v : split_list(fa.value(), ',')) {
      auto it = std::find_if(shared_.begin(), shared_.end(), [&](const Feature& f) { return f.value == v; });
      if (it != shared_.end()) ret.push_back(*it);
    }
  }
  pugi::xml_node holder = node.child("features");
  if (!holder) holder = node;
  for (pugi::xml_node f : holder.children()) {
    std::string_view tag = f.name();
    if (tag == "sharedFeature") {
      auto it = shared_.end();
      if (f.attribute("name")) {
        std::string nm = f.attribute("name").value();
        it = std::find_if(shared_.begin(), shared_.end(), [&](const Feature& x) { return x.name == nm; });
      }
      if (it == shared_.end() && f.attribute("value")) {
        std::string vl = f.attribute("value").value();
        it = std::find_if(shared_.begin(), shared_.end(), [&](const Feature& x) { return x.value == vl; });
      }
      if (it == shared_.end()) continue;
      Feature cs = *it;
      if (f.attribute("submenu")) cs.submenu = f.attribute("submenu").value();
      if (f.attribute("preset")) cs.preset = f.attribute("preset").value();
      if (f.attribute("preset-parameters")) cs.preset_parameters = f.attribute("preset-parameters").value();
      if (f.attribute("group")) cs.group = f.attribute("group").value();
      if (f.attribute("order")) cs.order = f.attribute("order").as_int();
      ret.push_back(std::move(cs));
      continue;
    }
    if (tag != "feature" || !f.attribute("name")) continue;
    Feature ft;
    ft.name = f.attribute("name").value();
    ft.description = f.attribute("description").value();
    ft.submenu = f.attribute("submenu").value();
    ft.preset = f.attribute("preset").value();
    ft.preset_parameters = f.attribute("preset-parameters").value();
    ft.group = f.attribute("group").value();
    ft.order = f.attribute("order").as_int();
    ft.value = f.attribute("value") ? f.attribute("value").value() : underscored(ft.name);
    for (pugi::xml_node c : f.children("choice")) {
      if (!c.attribute("name")) continue;
      FeatureChoice ch;
      ch.name = c.attribute("name").value();
      ch.value = c.attribute("value") ? c.attribute("value").value() : underscored(ch.name);
      ft.choices.push_back(std::move(ch));
    }
    if (!ft.choices.empty() || !ft.preset.empty()) ret.push_back(std::move(ft));
  }
  return ret;
}

bool FeatureDb::add_text(const std::string& xml) {
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return false;
  pugi::xml_node root = doc.child("features");
  if (!root) return false;

  if (pugi::xml_node sf = root.child("sharedFeatures")) {
    for (Feature& f : parse_features(sf)) {
      auto it = std::find_if(shared_.begin(), shared_.end(), [&](const Feature& x) { return x.value == f.value; });
      if (it != shared_.end()) *it = std::move(f);
      else shared_.push_back(std::move(f));
    }
  }
  if (pugi::xml_node gf = root.child("globalFeatures")) {
    for (Feature& f : parse_features(gf)) {
      auto it = std::find_if(global_.begin(), global_.end(), [&](const Feature& x) { return x.value == f.value; });
      if (it != global_.end()) *it = std::move(f);
      else global_.push_back(std::move(f));
    }
    std::stable_sort(global_.begin(), global_.end(), [](const Feature& a, const Feature& b) { return a.order < b.order; });
  }
  loaded_ = true;

  for (pugi::xml_node emulator : root.children("emulator")) {
    if (!emulator.attribute("name")) continue;
    for (const std::string& ename : split_list(emulator.attribute("name").value(), ',')) {
      EmuDef* emu = find_named(emus_, ename);
      if (!emu) {
        emus_.emplace_back();
        emu = &emus_.back();
        emu->name = ename;
      }
      std::vector<Feature> custom = parse_features(emulator);
      uint32_t flags = any_value(custom, "autosave") ? uint32_t(F_autosave) : 0u;
      if (emulator.attribute("features") || !custom.empty()) {
        if (emulator.attribute("features")) flags = parse_flags(emulator.attribute("features").value());  // as ES: replaces
        emu->flags |= flags;
        append(emu->custom, custom);
      }

      pugi::xml_node cores = emulator.child("cores");
      if (!cores) cores = emulator;
      for (pugi::xml_node core : cores.children("core")) {
        if (!core.attribute("name")) continue;
        for (const std::string& cname : split_list(core.attribute("name").value(), ',')) {
          uint32_t cflags = core.attribute("features") ? parse_flags(core.attribute("features").value()) : 0u;
          std::vector<Feature> ccustom = parse_features(core);
          if (any_value(ccustom, "autosave")) cflags |= F_autosave;
          CoreDef* cd = find_named(emu->cores, cname);
          if (!cd) {
            emu->cores.emplace_back();
            cd = &emu->cores.back();
            cd->name = cname;
          }
          cd->flags |= cflags;
          append(cd->custom, ccustom);

          pugi::xml_node systems = core.child("systems");
          if (!systems) systems = core;
          for (pugi::xml_node sn : systems.children("system")) {
            if (!sn.attribute("name")) continue;
            for (const std::string& sname : split_list(sn.attribute("name").value(), ',')) {
              uint32_t sflags = sn.attribute("features") ? parse_flags(sn.attribute("features").value()) : 0u;
              std::vector<Feature> scustom = parse_features(sn);
              if (any_value(scustom, "autosave")) sflags |= F_autosave;
              if (sflags == 0 && scustom.empty()) continue;
              SysFeat* sf = find_named(cd->systems, sname);
              if (!sf) {
                cd->systems.emplace_back();
                sf = &cd->systems.back();
                sf->name = sname;
              }
              sf->flags |= sflags;
              append(sf->custom, scustom);
            }
          }
        }
      }

      pugi::xml_node systems = emulator.child("systems");
      if (!systems) systems = emulator;
      for (pugi::xml_node sn : systems.children("system")) {
        if (!sn.attribute("name")) continue;
        for (const std::string& sname : split_list(sn.attribute("name").value(), ',')) {
          uint32_t sflags = sn.attribute("features") ? parse_flags(sn.attribute("features").value()) : 0u;
          std::vector<Feature> scustom = parse_features(sn);
          SysFeat* sf = find_named(emu->systems, sname);
          if (!sf) {
            emu->systems.emplace_back();
            sf = &emu->systems.back();
            sf->name = sname;
          }
          sf->flags |= sflags;
          append(sf->custom, scustom);
        }
      }
    }
  }
  return true;
}

Resolved FeatureDb::resolve(const System& sys, const std::string& emulator, const std::string& core) const {
  Resolved r;
  const std::pair<std::string, std::vector<std::string>>* se = nullptr;
  for (const auto& e : sys.emus)
    if (e.first == emulator) {
      se = &e;
      break;
    }
  if (!se) {
    r.flags = loaded_ ? 0u : uint32_t(F_all);
    return r;
  }
  std::string key = starts_with(emulator, "lr-") ? "libretro" : emulator;
  const EmuDef* d = nullptr;
  for (const EmuDef& e : emus_)
    if (e.name == key) d = &e;
  if (d) {
    r.flags |= d->flags;
    append(r.custom, d->custom);
    for (const SysFeat& s : d->systems)
      if (s.name == sys.name) {
        r.flags |= s.flags;
        append(r.custom, s.custom);
      }
    if (std::find(se->second.begin(), se->second.end(), core) != se->second.end()) {
      for (const CoreDef& c : d->cores) {
        if (c.name != core) continue;
        r.flags |= c.flags;
        append(r.custom, c.custom);
        for (const SysFeat& s : c.systems)
          if (s.name == sys.name) {
            r.flags |= s.flags;
            append(r.custom, s.custom);
          }
      }
    }
  }
  std::stable_sort(r.custom.begin(), r.custom.end(), [](const Feature& a, const Feature& b) { return a.order < b.order; });
  return r;
}

bool FeatureDb::system_has_features(const System& sys) const {
  if (!loaded_) return true;
  for (const auto& e : sys.emus) {
    // any core of the emulator (the flags/custom of the emulator count for every core)
    if (e.second.empty()) {
      Resolved r = resolve(sys, e.first, std::string());
      if (r.flags || !r.custom.empty()) return true;
    }
    for (const std::string& c : e.second) {
      Resolved r = resolve(sys, e.first, c);
      if (r.flags || !r.custom.empty()) return true;
    }
  }
  return false;
}

// ---- the device's database --------------------------------------------------------------------------------------------
namespace {

std::mutex g_db_m;
FeatureDb* g_db = nullptr;

}  // namespace

const FeatureDb& features() {
  std::lock_guard<std::mutex> lk(g_db_m);
  if (g_db) return *g_db;
  g_db = new FeatureDb();  // lives for the process; replaced only by reset_caches
  const Env& e = env();
  std::string text;
  std::vector<std::string> main_candidates;
  if (!e.features_file.empty()) main_candidates.push_back(e.features_file);
  else {
    main_candidates.push_back(e.rooted(e.user_dir + "/es_features.cfg"));
    main_candidates.push_back(e.rooted("/usr/config/emulationstation/es_features.cfg"));
  }
  for (const std::string& p : main_candidates)
    if (read_file(p, text)) {
      g_db->add_text(text);
      break;
    }
  // es_features_*.cfg of the user folder, in name order
  std::vector<std::string> extra;
  std::string dir = e.rooted(e.user_dir);
  if (DIR* d = ::opendir(dir.c_str())) {
    while (const struct dirent* ent = ::readdir(d)) {
      std::string_view n = ent->d_name;
      if (starts_with(n, "es_features_") && ends_with(n, ".cfg")) extra.emplace_back(n);
    }
    ::closedir(d);
  }
  std::sort(extra.begin(), extra.end());
  for (const std::string& n : extra)
    if (read_file(dir + "/" + n, text)) g_db->add_text(text);
  return *g_db;
}

void reset_features() {
  std::lock_guard<std::mutex> lk(g_db_m);
  delete g_db;
  g_db = nullptr;
}

}  // namespace zf::mg
