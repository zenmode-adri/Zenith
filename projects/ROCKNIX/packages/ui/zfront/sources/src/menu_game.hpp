// MODULE `menu-game`: the game settings menus.
//
//   menu.game            GAME SETTINGS (global.* keys of system.cfg, ES `GuiMenu::openGamesSettings`)
//   menu.game.systems    PER SYSTEM ADVANCED CONFIGURATION (list of systems, each opens `menu.system_options`)
//   menu.system_options  arg = system name           ADVANCED SYSTEM OPTIONS (`<system>.<key>`)
//   menu.game_options    arg = `game_arg(system, rom)` ADVANCED GAME OPTIONS (`<system>["<rom file>"].<key>`)
//   menu.game.sysopt     arg = config name           SYSTEM OPTIONS (governors, cooling...)
//   menu.game.latency    arg = config name           LATENCY REDUCTION
//   menu.game.ai         AI GAME TRANSLATION (global)
//   menu.game.bios       MISSING BIOS CHECK
//
// Everything is stored where ES stores it, in `system.cfg` through `cx.conf.persist` (same lock as the shell scripts), so
// `setsettings.sh` / `runemu.sh` and ES itself see the same values. A value of "" / "auto" / "default" removes the line
// (ES `SystemConf::saveSystemConf`).
//
// The options a system/emulator/core offers come from `es_features.cfg` (the file ES reads), see `FeatureDb`.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "app.hpp"

namespace pugi {
class xml_node;
}

namespace zf::mg {

// ---- where things live (re-rootable, for tests and the preview harness) ----------------------------------------------
struct Env {
  /// Prefix of every absolute path below (`$ZF_MG_ROOT` when set). Empty on the device.
  std::string root;
  std::string features_file;  // empty: user dir, then /usr/config
  std::string user_dir = "/storage/.config/emulationstation";
  std::string shaders_dir = "/tmp/shaders";
  std::string filters_dir = "/usr/share/retroarch/filters/64bit/video";
  std::vector<std::string> bezel_dirs = {"/tmp/overlays/bezels", "/storage/roms/bezels"};
  std::string cpu_governors = "/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors";
  std::string profile_governors = "/storage/.config/profile.d/010-governors";   // defines GPU_FREQ
  std::string profile_shader = "/storage/.config/profile.d/091-ui_shader";      // defines UI_SHADER
  std::string gpu_freq_default = "/sys/devices/platform/ff400000.gpu/devfreq/ff400000.gpu";
  std::string bin_dir = "/usr/bin";
  std::string rooted(const std::string& p) const { return root + p; }
};
Env& env();
/// Forgets every cached list (shaders, filters, governors, es_features.cfg). Tests call it after changing `env()`.
void reset_caches();

// ---- system.cfg ------------------------------------------------------------------------------------------------------
std::string conf_get(const Ctx& cx, const std::string& key);
/// Stores `key=value`; "", "auto" and "default" remove the line. A failed write shows a toast. Returns false then.
bool conf_put(Ctx& cx, const std::string& key, const std::string& value);
/// Config name of a game: `<system>["<file>"]`, with `=` and `#` taken out of the file name as ES does.
std::string game_cfg(const std::string& system, std::string_view rom_file);

/// `menu.game_options` argument: system name, a TAB, the ROM path.
std::string game_arg(std::string_view system, std::string_view rom_path);
bool split_game_arg(std::string_view arg, std::string& system, std::string& rom_path);

// ---- es_features.cfg ------------------------------------------------------------------------------------------------
enum Flag : uint32_t {
  F_none = 0,
  F_ratio = 1u << 0,
  F_rewind = 1u << 1,
  F_smooth = 1u << 2,
  F_shaders = 1u << 3,
  F_pixel_perfect = 1u << 4,
  F_decoration = 1u << 5,
  F_latency_reduction = 1u << 6,
  F_game_translation = 1u << 7,
  F_autosave = 1u << 8,
  F_netplay = 1u << 9,
  F_fullboot = 1u << 10,
  F_emulated_wiimotes = 1u << 11,
  F_screen_layout = 1u << 12,
  F_internal_resolution = 1u << 13,
  F_videomode = 1u << 14,
  F_colorization = 1u << 15,
  F_padtokeyboard = 1u << 16,
  F_cheevos = 1u << 17,
  F_autocontrollers = 1u << 18,
  F_videofilters = 1u << 19,
  F_all = 0x0FFFFFFFu,
};

struct FeatureChoice {
  std::string name, value;
};
/// One `<feature>` of es_features.cfg (ES `CustomFeature`).
struct Feature {
  std::string name, value, description, submenu, preset, preset_parameters, group;
  int order = 0;
  std::vector<FeatureChoice> choices;
};

struct SysFeat {
  std::string name;
  uint32_t flags = 0;
  std::vector<Feature> custom;
};
struct CoreDef {
  std::string name;
  uint32_t flags = 0;
  std::vector<Feature> custom;
  std::vector<SysFeat> systems;
};
struct EmuDef {
  std::string name;
  uint32_t flags = 0;
  std::vector<Feature> custom;
  std::vector<CoreDef> cores;
  std::vector<SysFeat> systems;
};

/// What the options of one (system, emulator, core) are.
struct Resolved {
  uint32_t flags = 0;
  std::vector<Feature> custom;  // stable-sorted by `order`
  bool has(Flag f) const { return (flags & f) == uint32_t(f); }
  bool has_custom(const std::string& value) const;
};

/// The parsed es_features.cfg (plus the `es_features_*.cfg` of the user dir): ES `CustomFeatures`.
class FeatureDb {
 public:
  /// Parses one file's text and merges it in. False when it is not a `<features>` document.
  bool add_text(const std::string& xml);
  bool loaded() const { return loaded_; }
  const std::vector<Feature>& global() const { return global_; }
  bool has_global(const std::string& name) const;
  /// ES `SystemData::loadFeatures` + `isFeatureSupported` + `getCustomFeatures` for the pair `emulator`/`core` of `sys`.
  /// With no features file loaded everything is "supported" only when the emulator is unknown, as in ES.
  Resolved resolve(const System& sys, const std::string& emulator, const std::string& core) const;
  /// `hasFeatures()` of ES: some emulator of the system has flags or custom features.
  bool system_has_features(const System& sys) const;

 private:
  uint32_t parse_flags(const std::string& list) const;
  std::vector<Feature> parse_features(const pugi::xml_node& node) const;
  bool loaded_ = false;
  std::vector<Feature> shared_, global_;
  std::vector<EmuDef> emus_;
};

/// The device's database, loaded on first use.
const FeatureDb& features();
/// Drops the cached database (tests, after `env()` changed).
void reset_features();

// ---- sources of lists ------------------------------------------------------------------------------------------------
/// RetroArch shader presets (`getshaders`): paths relative to /tmp/shaders. Cached; `loaded` tells whether the walk ran.
const std::vector<std::string>& shader_list(bool* loaded = nullptr);
void load_shader_list();  // does the walk (call from a job or from a synchronous path)
const std::vector<std::string>& filter_list(bool* loaded = nullptr);
void load_filter_list();
/// "default" + the governors in sysfs.
std::vector<std::string> cpu_governors();
std::vector<std::string> gpu_governors();
/// Subfolders of the bezel folders (decoration sets), sorted, no duplicates.
std::vector<std::string> decoration_sets();
bool bin_exists(const char* name);

/// The ratios of libretro as ES lists them: {label, value}.
const std::vector<std::pair<const char*, const char*>>& ratios();

// ---- system / game helpers ---------------------------------------------------------------------------------------------
const System* find_system(const Ctx& cx, const std::string& name);
/// ES `hasEmulatorSelection`: more than one emulator or more than one core.
bool has_emulator_selection(const System& sys);

/// The (emulator, core) the picker shows as chosen: what the config forces, "" = AUTO.
struct Pick {
  std::string emu, core;
};
Pick explicit_pick(const System& sys, const ConfMap& conf, const std::string& cfg);
/// The pair a launch would use for this config (system, or game when `rom_file` is not empty).
Pick resolved_pick(const System& sys, const ConfMap& conf, std::string_view rom_file);

}  // namespace zf::mg
