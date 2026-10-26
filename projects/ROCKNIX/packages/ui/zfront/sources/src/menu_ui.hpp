// `menu.ui`: USER INTERFACE SETTINGS (ES's GuiMenu::openUISettings), limited to what zfront really does. See menu_ui.cpp
// for the table of ES entries and why each one is shown or left out.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "i18n.hpp"
#include "menu.hpp"

namespace zf {

/// Where the language list and `set_language` look for catalogs (default: `i18n::default_options()`; tests point it at
/// fixtures).
i18n::Options& ui_language_options();

/// ES's language list: the code stored in `system.language` and the language's own name.
struct LanguageName {
  const char* code;
  const char* name;
};
const std::vector<LanguageName>& language_names();

/// The languages that can be chosen: English plus every ES language that has a catalog in `opts`.
std::vector<Option> available_language_options(const i18n::Options& opts);

/// Forgets the brightness a dry-run "wrote" (tests and the harness; on the device nothing is kept).
void ui_reset_dry_state();

}  // namespace zf
