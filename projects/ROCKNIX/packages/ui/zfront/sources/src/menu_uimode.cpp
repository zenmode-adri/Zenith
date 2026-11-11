// USER INTERFACE MODE (ES: GuiMenu::openSystemSettings row, ExitKidModeMsgBox). Two screens, see uimode.hpp for what each mode hides:
//   `menu.uimode`          Full / Kiosk / Kid. A restricted mode first shows ES's warning with the unlock code; YES stores it.
//   `menu.uimode.unlock`   the way back: the code of `UIMode_passkey` typed with the pad (ES: UNLOCK USER INTERFACE MODE).
// The second one is reachable in every mode (it is the main menu's row in Kiosk and Kid); the first only in Full.
#include <memory>
#include <string>

#include "i18n.hpp"
#include "menu.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "uimode.hpp"

namespace zf {
namespace {

/// A menu that reads the pad itself: every key goes to the code, Start leaves (ES has no other way out than the code).
class UnlockMenu : public Menu {
 public:
  UnlockMenu(MenuSpec spec, std::string code) : Menu(std::move(spec)), un_(std::move(code)) {}
  Transition handle(Action a, Ctx& cx) override {
    if (un_.feed(a, cx.es.get_bool("InvertButtons", true))) {
      uimode::set_mode(cx.es, uimode::Mode::Full);  // ForceKid / ForceKiosk (command line in ES) are not cleared by it, as in ES
      return Transition::of(Transition::CloseMenus);
    }
    if (a == Action::Start) return Transition::pop();
    return Transition::none();
  }

 private:
  uimode::Unlock un_;
};

Transition choose_mode(Ctx& cx, uimode::Mode mode) {
  if (mode == uimode::Mode::Full) {
    uimode::set_mode(cx.es, mode);
    return Transition::of(Transition::CloseMenus);
  }
  // ES: the warning names the code before anything is locked
  std::string msg = tr("You are changing the user interface to a restricted mode:\nThis will hide most menu options to prevent changes to the system.\nTo unlock and return to the full user interface, enter this code:");
  msg += "\n\"" + uimode::passkey_text(uimode::passkey(cx.es)) + "\"\n\n";
  msg += tr("Do you want to proceed?");
  return ui::confirm(msg, [mode](Ctx& c) {
    uimode::set_mode(c.es, mode);
    return Transition::of(Transition::CloseMenus);
  });
}

std::unique_ptr<Screen> make_picker() {
  MenuSpec s;
  s.title = tr("USER INTERFACE MODE");
  s.title_key = N_("USER INTERFACE MODE");
  s.build = [](MenuBuilder& m, Ctx& cx) {
    const uimode::Mode cur = uimode::of(cx.es);
    m.action(tr("Full"), [](Ctx& c) { return choose_mode(c, uimode::Mode::Full); }).value(cur == uimode::Mode::Full ? "*" : "");
    m.action(tr("Kiosk"), [](Ctx& c) { return choose_mode(c, uimode::Mode::Kiosk); }).value(cur == uimode::Mode::Kiosk ? "*" : "");
    m.action(tr("Kid"), [](Ctx& c) { return choose_mode(c, uimode::Mode::Kid); }).value(cur == uimode::Mode::Kid ? "*" : "");
  };
  return make_menu(std::move(s));
}

// B is part of the code (ES: the code is typed with the pad), so it cannot be "back": only Start leaves.
const ui::Prompt kUnlockHelp[] = {{"start", N_("BACK")}};

std::unique_ptr<Screen> make_unlock(Ctx& cx) {
  MenuSpec s;
  s.title = tr("UNLOCK USER INTERFACE MODE");
  s.title_key = N_("UNLOCK USER INTERFACE MODE");
  s.build = [](MenuBuilder& m, Ctx&) { m.info(tr("ENTER THE CODE NOW TO UNLOCK")); };
  s.help = kUnlockHelp;
  s.help_n = 1;
  return std::make_unique<UnlockMenu>(std::move(s), uimode::passkey(cx.es));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.uimode", [](Ctx&, const std::string&) { return make_picker(); });
ZF_REGISTER_SCREEN("menu.uimode.unlock", [](Ctx& cx, const std::string&) { return make_unlock(cx); });

}  // namespace zf
