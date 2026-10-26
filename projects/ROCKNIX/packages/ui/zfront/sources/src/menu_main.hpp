// `menu.main`: the real main menu (the entries of ES's GuiMenu). Each entry opens the screen another module registers and is
// shown only while `Registry::has(<screen>)`, so the menu always matches what this build can do.
//
//   GAME SETTINGS menu.game | CONTROLLER & BLUETOOTH menu.controllers | USER INTERFACE SETTINGS menu.ui |
//   GAME COLLECTION SETTINGS menu.collections | SOUND SETTINGS menu.sound | NETWORK SETTINGS menu.network |
//   SCRAPER menu.scraper | UPDATES & DOWNLOADS menu.updates | SYSTEM SETTINGS menu.system | QUIT menu.quit
//
// `menu.demo` (every widget of the framework) is reachable from here only in the harness / dry-run (`--preview`, `--dry-run`,
// tests), never on the device.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "app.hpp"

namespace zf {

struct MainRow {
  std::string label;   // already translated
  std::string screen;  // registered screen name
  std::string icon;    // theme `assets/menu/menu-icon-<icon>.svg`
};

/// The ten rows of ES's main menu, in ES order, with their exact screen names.
std::vector<MainRow> main_rows();
/// A main menu over `rows` (tests use names that are or are not registered; empty rows = the ten ES rows).
std::unique_ptr<Screen> make_main_menu(std::vector<MainRow> rows, bool with_demo);
/// What `menu.main` registers.
std::unique_ptr<Screen> make_main_menu(Ctx& cx);

}  // namespace zf
