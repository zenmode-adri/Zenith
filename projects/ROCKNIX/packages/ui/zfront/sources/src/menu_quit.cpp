// `menu.quit`: leave the frontend, restart or power off the console (ES's QUIT menu). The actions are the OS contract's
// `oscontract::quit(Mode)` / `oscontract::suspend()`: the system command itself runs after the window is gone (`finish`), and
// `Transition::quit()` is what ends the loop (device and harness alike).
//
//   RESTART THE FRONTEND  Mode::Quit      exit 0, systemd (Restart=always) starts the frontend again
//   RESTART SYSTEM            Mode::Reboot    `reboot.please` + shutdown -r now
//   SUSPEND SYSTEM            oscontract::suspend()  (systemctl suspend); only where ES offers it (pm-suspend exists)
//   SHUTDOWN SYSTEM           Mode::Shutdown  `shutdown.please` + shutdown -P -h now
//
// FAST SHUTDOWN ("shutdown without saving metadata") is not offered: zfront writes the gamelist in the background right
// after a game, so there is nothing to skip and it would be the same as SHUTDOWN (`kShowFastShutdown` brings it back).
#include "menu.hpp"
#include "menu_lang.hpp"
#include "oscontract.hpp"
#include "registry.hpp"

namespace zf {

namespace {

/// The user asked for a YES/NO question before every action of this menu. ES (Zenith patch 0053) dropped it for restart,
/// suspend and shutdown ("two deliberate steps already"); flip this to follow that.
constexpr bool kConfirmQuit = true;
constexpr bool kShowFastShutdown = false;

Transition leave(oscontract::Mode m) {
  oscontract::quit(m);  // fires the quit/reboot/shutdown events, remembers the mode, asks the loop to leave
  return Transition::quit();
}

Entry& power(MenuBuilder& m, std::string label, const char* icon, std::string question, Act act) {
  Entry& e = m.action(std::move(label), std::move(act)).icon(icon);
  if (kConfirmQuit) e.confirm(std::move(question));
  return e;
}

std::unique_ptr<Screen> make_quit_menu() {
  MenuSpec s;
  s.title = tr("QUIT");
  s.title_key = N_("QUIT");
  s.build = [](MenuBuilder& m, Ctx&) {
    power(m, tr("RESTART THE FRONTEND"), "restart", tr("REALLY RESTART THE FRONTEND?"), [](Ctx&) { return leave(oscontract::Mode::Quit); });
    power(m, tr("RESTART SYSTEM"), "restart", tr("REALLY RESTART?"), [](Ctx&) { return leave(oscontract::Mode::Reboot); });
    if (oscontract::suspend_supported())
      power(m, tr("SUSPEND SYSTEM"), "shutdown", tr("REALLY SUSPEND ?"), [](Ctx&) {
        oscontract::suspend();
        return Transition::none();
      });
    power(m, tr("SHUTDOWN SYSTEM"), "shutdown", tr("REALLY SHUTDOWN?"), [](Ctx&) { return leave(oscontract::Mode::Shutdown); });
    if (kShowFastShutdown)
      power(m, tr("FAST SHUTDOWN SYSTEM"), "shutdown", tr("REALLY SHUTDOWN WITHOUT SAVING METADATA?"), [](Ctx&) { return leave(oscontract::Mode::FastShutdown); })
          .hint(tr("Shutdown without saving metadata."));
  };
  return std::make_unique<LangMenu>(std::move(s));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.quit", [](Ctx&, const std::string&) { return make_quit_menu(); });

}  // namespace zf
