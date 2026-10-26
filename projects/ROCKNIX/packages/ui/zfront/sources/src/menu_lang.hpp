// Kept for the menus that name it: every `Menu` now follows the interface language by itself (it compares
// `i18n::language()` with the one it was built for, rebuilds its entries and re-translates a title given with
// `MenuSpec::title_key`), so `LangMenu` is just a `Menu`.
#pragma once
#include <memory>
#include <string>

#include "i18n.hpp"
#include "menu.hpp"

namespace zf {

class LangMenu : public Menu {
 public:
  explicit LangMenu(MenuSpec spec) : Menu(std::move(spec)) {}
};

}  // namespace zf
