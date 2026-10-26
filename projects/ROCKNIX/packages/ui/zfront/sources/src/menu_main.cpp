#include "menu_main.hpp"

#include "menu_lang.hpp"
#include "registry.hpp"

namespace zf {

std::vector<MainRow> main_rows() {
  // tr() with literals, so tools/extract-strings.sh finds every key; evaluated at every build, so a language change reaches it
  return {
      {tr("GAME SETTINGS"), "menu.game", "games"},
      {tr("CONTROLLER & BLUETOOTH"), "menu.controllers", "controller"},
      {tr("USER INTERFACE SETTINGS"), "menu.ui", "ui"},
      {tr("GAME COLLECTION SETTINGS"), "menu.collections", "advanced"},
      {tr("SOUND SETTINGS"), "menu.sound", "sound"},
      {tr("NETWORK SETTINGS"), "menu.network", "network"},
      {tr("SCRAPER"), "menu.scraper", "scraper"},
      {tr("UPDATES & DOWNLOADS"), "menu.updates", "update"},
      {tr("SYSTEM SETTINGS"), "menu.system", "system"},
      {tr("QUIT"), "menu.quit", "quit"},
  };
}

std::unique_ptr<Screen> make_main_menu(std::vector<MainRow> rows, bool with_demo) {
  MenuSpec s;
  s.title = tr("MAIN MENU");
  s.title_key = N_("MAIN MENU");
  s.build = [rows = std::move(rows), with_demo](MenuBuilder& m, Ctx&) {
    std::vector<MainRow> own;
    if (rows.empty()) own = main_rows();
    const Registry& reg = Registry::instance();
    for (const MainRow& r : rows.empty() ? own : rows)
      if (reg.has(r.screen)) m.submenu(r.label, r.screen).icon(r.icon);  // a screen nobody registered is not shown
    if (with_demo && reg.has("menu.demo")) m.submenu(tr("WIDGET DEMO"), "menu.demo").icon("manual").hint(tr("Development only: every widget of the menu framework."));
  };
  return std::make_unique<LangMenu>(std::move(s));
}

std::unique_ptr<Screen> make_main_menu(Ctx& cx) {
  // dry-run = the harness and the tests; the device loop never runs dry
  return make_main_menu({}, cx.sv->cmd.dry_run);
}

ZF_REGISTER_SCREEN("menu.main", [](Ctx& cx, const std::string&) { return make_main_menu(cx); });

}  // namespace zf
