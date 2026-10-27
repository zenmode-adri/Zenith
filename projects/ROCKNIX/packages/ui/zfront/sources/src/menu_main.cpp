#include "menu_main.hpp"

#include "menu_groups.hpp"
#include "menu_lang.hpp"
#include "registry.hpp"

namespace zf {

std::vector<MainRow> main_rows() {
  // tr() with literals, so tools/extract-strings.sh finds every key; evaluated at every build, so a language change reaches it
  return {
      {tr("GAMES"), "menu.game", "games"},
      {tr("SCREEN & SOUND"), "menu.g.screen", "screen"},
      {tr("BATTERY & PERFORMANCE"), "menu.g.power", "battery"},
      {tr("CONTROLLER & BLUETOOTH"), "menu.controllers", "controller"},
      {tr("NETWORK"), "menu.network", "network"},
      {tr("LIBRARY"), "menu.g.library", "library"},
      {tr("SYSTEM"), "menu.g.system", "system"},
      {tr("QUIT"), "menu.quit", "quit"},
  };
}

std::unique_ptr<Screen> make_main_menu(std::vector<MainRow> rows, bool with_demo, std::vector<MenuSpec> quick) {
  MenuSpec s;
  s.title = tr("MAIN MENU");
  s.title_key = N_("MAIN MENU");
  auto quick_parts = std::make_shared<std::vector<MenuSpec>>(std::move(quick));
  s.on_open = [quick_parts](Ctx& cx, const Refresh& r) {
    for (const MenuSpec& p : *quick_parts)
      if (p.on_open) p.on_open(cx, r);
  };
  s.on_close = [quick_parts](Ctx& cx) {
    for (const MenuSpec& p : *quick_parts)
      if (p.on_close) p.on_close(cx);
  };
  s.build = [rows = std::move(rows), with_demo, quick_parts](MenuBuilder& m, Ctx& cx) {
    // quick settings (none by default, see make_main_menu(Ctx&)): rows of other sections, then a line
    for (const MenuSpec& p : *quick_parts)
      if (p.build) p.build(m, cx);
    if (m.count() > 0) m.separator();
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
  // no quick settings: the volume and the brightness have hardware buttons (input_sense), so a slider here is a second way in
  return make_main_menu({}, cx.sv->cmd.dry_run, {});
}

ZF_REGISTER_SCREEN("menu.main", [](Ctx& cx, const std::string&) { return make_main_menu(cx); });

}  // namespace zf
