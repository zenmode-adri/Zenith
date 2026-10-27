// `menu.main`: the real main menu (the entries of ES's GuiMenu). Each entry opens the screen another module registers and is
// shown only while `Registry::has(<screen>)`, so the menu always matches what this build can do.
//
//   GAMES menu.game | SCREEN & SOUND menu.g.screen | BATTERY & PERFORMANCE menu.g.power |
//   CONTROLLER & BLUETOOTH menu.controllers | NETWORK menu.network | LIBRARY menu.g.library | SYSTEM menu.g.system | QUIT menu.quit
// (no quick settings: the volume and the brightness have hardware buttons; `make_main_menu(rows, demo, quick)` still takes some)
// (the ES-order sections menu.ui, menu.sound, menu.system, menu.collections, menu.scraper, menu.updates stay registered)
//
// `menu.demo` (every widget of the framework) is reachable from here only in the harness / dry-run (`--preview`, `--dry-run`,
// tests), never on the device.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "app.hpp"
#include "menu.hpp"

namespace zf {

struct MainRow {
  std::string label;   // already translated
  std::string screen;  // registered screen name
  std::string icon;    // theme `assets/menu/menu-icon-<icon>.svg`
};

/// The nine rows of the main menu, grouped by what the person wants to do (see menu_groups.hpp), with their screen names.
std::vector<MainRow> main_rows();
/// A main menu over `rows` (tests use names that are or are not registered; empty rows = the ten ES rows).
std::unique_ptr<Screen> make_main_menu(std::vector<MainRow> rows, bool with_demo, std::vector<MenuSpec> quick = {});
/// What `menu.main` registers.
std::unique_ptr<Screen> make_main_menu(Ctx& cx);

}  // namespace zf
