// `menu.quit`: leave the frontend, restart or power off the console (ES's QUIT menu). The actions are the OS contract's
// `oscontract::quit(Mode)` / `oscontract::suspend()`: the system command itself runs after the window is gone (`finish`), and
// `Transition::quit()` is what ends the loop (device and harness alike).
//
//   RESTART THE FRONTEND  Mode::Quit      exit 0, systemd (Restart=always) starts the frontend again
//   RESTART SYSTEM            Mode::Reboot    `reboot.please` + shutdown -r now
//   SUSPEND SYSTEM            oscontract::suspend()  (systemctl suspend -> sleep.sh, or rocknix-fake-suspend when suspendmode=off); only where the sleep.sh hook exists
//   SHUTDOWN SYSTEM           Mode::Shutdown  `shutdown.please` + shutdown -P -h now
//
// FAST SHUTDOWN ("shutdown without saving metadata") is ES's; zfront writes the gamelist in the background right after a game,
// so there is no metadata to skip and it would do exactly what SHUTDOWN does: hidden (`kShowFastShutdown`).
//
// Argument "quick" = ES's QUICK ACCESS menu (Select in the system carousel): LAUNCH SCREENSAVER, then the same QUIT entries.
// (ES also lists music, the manual and plane mode there; zfront has none of them.)
#include "menu.hpp"
#include "menu_lang.hpp"
#include "oscontract.hpp"
#include "registry.hpp"

namespace zf {

namespace {

/// The user asked for a YES/NO question before every action of this menu. ES (Zenith patch 0053) dropped it for restart,
/// suspend and shutdown ("two deliberate steps already"); flip this to follow that.
constexpr bool kConfirmQuit = true;
constexpr bool kShowFastShutdown = false;  // it did the same as SHUTDOWN: two identical rows, the second one lying

Transition leave(oscontract::Mode m) {
  oscontract::quit(m);  // fires the quit/reboot/shutdown events, remembers the mode, asks the loop to leave
  return Transition::quit();
}

Entry& power(MenuBuilder& m, std::string label, const char* icon, std::string question, Act act) {
  Entry& e = m.action(std::move(label), std::move(act)).icon(icon);
  if (kConfirmQuit) e.confirm(std::move(question));
  return e;
}

std::unique_ptr<Screen> make_quit_menu(bool quick) {
  MenuSpec s;
  s.title = quick ? tr("QUICK ACCESS") : tr("QUIT");
  s.title_key = quick ? N_("QUICK ACCESS") : N_("QUIT");
  s.build = [quick](MenuBuilder& m, Ctx&) {
    if (quick) {
      m.header(tr("QUICK ACCESS"));
      m.action(tr("LAUNCH SCREENSAVER"), [](Ctx& cx) {
        oscontract::screensaver_start(cx.es);
        return Transition::pop();
      }).icon("screen");
      m.header(tr("QUIT"));
    }
    // the usual first: suspend, power off, restart; restarting the frontend alone is the rare one
    if (oscontract::suspend_supported())
      power(m, tr("SUSPEND SYSTEM"), "shutdown", tr("REALLY SUSPEND ?"), [](Ctx&) {
        oscontract::suspend();
        return Transition::none();
      });
    power(m, tr("SHUTDOWN SYSTEM"), "shutdown", tr("REALLY SHUTDOWN?"), [](Ctx&) { return leave(oscontract::Mode::Shutdown); });
    power(m, tr("RESTART SYSTEM"), "restart", tr("REALLY RESTART?"), [](Ctx&) { return leave(oscontract::Mode::Reboot); });
    power(m, tr("RESTART THE FRONTEND"), "restart", tr("REALLY RESTART THE FRONTEND?"), [](Ctx&) { return leave(oscontract::Mode::Quit); });
    if (kShowFastShutdown)
      power(m, tr("FAST SHUTDOWN SYSTEM"), "shutdown", tr("REALLY SHUTDOWN WITHOUT SAVING METADATA?"), [](Ctx&) { return leave(oscontract::Mode::FastShutdown); })
          .hint(tr("Shutdown without saving metadata."));
  };
  return std::make_unique<LangMenu>(std::move(s));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.quit", [](Ctx&, const std::string& arg) { return make_quit_menu(arg == "quick"); });

}  // namespace zf
