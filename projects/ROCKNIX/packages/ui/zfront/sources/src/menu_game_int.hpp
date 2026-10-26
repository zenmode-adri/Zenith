// menu-game: helpers shared by menu_game.cpp (global settings) and menu_game_cfg.cpp (per system / per game options).
#pragma once
#include <string>
#include <vector>

#include "menu.hpp"
#include "menu_game.hpp"

namespace zf::mg::detail {

/// A group title that only appears when an entry is added after it (a section with nothing to show leaves no header).
struct Sec {
  MenuBuilder& m;
  std::string pending;
  bool any = false;
  explicit Sec(MenuBuilder& mb) : m(mb) {}
  void header(std::string h) { pending = std::move(h); }
  /// The builder, with the pending header already added.
  MenuBuilder& b() {
    if (!pending.empty()) {
      m.header(pending);
      pending.clear();
    }
    any = true;
    return m;
  }
};

/// AUTO / ON / OFF stored as "auto" / "1" / "0" (a missing line is AUTO).
std::vector<Option> tri_options();

/// A choice bound to `key` of system.cfg. A missing line shows the option whose value is `def`.
Entry& conf_choice(MenuBuilder& m, Ctx& cx, std::string label, std::string key, std::vector<Option> options, std::string def = "auto");
/// AUTO / ON / OFF.
Entry& conf_tri(MenuBuilder& m, Ctx& cx, std::string label, std::string key);
/// A switch: `on_value` when on; off writes `off_value` ("" removes the line).
Entry& conf_switch(MenuBuilder& m, Ctx& cx, std::string label, std::string key, std::string on_value = "1", std::string off_value = "");
/// A text entry bound to `key` (empty removes the line).
Entry& conf_text(MenuBuilder& m, Ctx& cx, std::string label, std::string key, bool secret = false);

/// Option lists from the sources of `menu_game_src.cpp`.
std::vector<Option> shader_options(bool* loaded);
std::vector<Option> filter_options(bool* loaded);
std::vector<Option> decoration_options();

/// Starts the loads of the shader / filter lists the screen needs: synchronous in dry-run (tests, preview harness), on the
/// job thread otherwise, calling `refresh` when done. Does nothing when the lists are already loaded.
void ensure_lists(Ctx& cx, const Refresh& refresh, bool shaders, bool filters);

/// Adds the entries of es_features.cfg `items` (ES `addFeatures`): groups, submenus, presets. `cfg` is the config name
/// (`global`, `gb`, `gb["Tetris.gb"]`). `target` is the argument of `menu.game.features` for the submenus (`feat_arg`).
void add_features(Sec& s, Ctx& cx, const std::vector<Feature>& items, const std::string& cfg, const std::string& default_group, bool group_only_if_not_first, const std::string& feat_target);
/// `menu.game.features` argument: `<g|s|r> TAB <system> TAB <rom path> TAB <submenu>`.
std::string feat_arg(char kind, const std::string& system, const std::string& rom_path, const std::string& submenu);

/// Capitalises the first letter of a feature name (they are written lower case in es_features.cfg), translating it with the
/// `game_options` context first.
std::string feature_label(const std::string& name);

}  // namespace zf::mg::detail
