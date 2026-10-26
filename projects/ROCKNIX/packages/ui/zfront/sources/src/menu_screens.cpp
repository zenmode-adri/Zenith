// `menu.demo` (every widget of the framework, for review and as a worked example). The main menu is `menu_main.cpp`; it opens
// the screens the section menus register (ZF_REGISTER_SCREEN):
//   menu.game  menu.controllers  menu.ui  menu.collections  menu.sound  menu.network  menu.scraper  menu.updates
//   menu.system  menu.quit
#include <memory>

#include "menu.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "ui_osd.hpp"

namespace zf {

namespace {

struct DemoState {
  bool clock = true, help = false;
  int volume = 62, brightness = 60;
  std::string battery = "text", zone = "Europe/Madrid", wifi_key = "hunter22", host = "zenith";
};

std::vector<Option> zones() {
  const char* z[] = {"Africa/Cairo",     "America/Bogota",     "America/Mexico_City", "America/New_York", "America/Sao_Paulo", "Asia/Dubai",
                     "Asia/Kolkata",     "Asia/Tokyo",         "Atlantic/Canary",     "Australia/Sydney", "Europe/Berlin",    "Europe/London",
                     "Europe/Madrid",    "Europe/Moscow",      "Europe/Paris",        "Pacific/Auckland"};
  std::vector<Option> v;
  for (const char* s : z) v.push_back({s, s});
  return v;
}

std::unique_ptr<Screen> make_demo() {
  auto st = std::make_shared<DemoState>();
  MenuSpec s;
  s.title = tr("WIDGET DEMO");
  s.title_key = N_("WIDGET DEMO");
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    m.header(tr("SWITCHES AND SLIDERS"));
    m.toggle(tr("SHOW CLOCK"), [st] { return st->clock; }, [st](bool b) { st->clock = b; }).hint(tr("The clock shows in the top right corner."));
    m.toggle(tr("ON-SCREEN HELP"), [st] { return st->help; }, [st](bool b) { st->help = b; });
    m.slider(tr("SYSTEM VOLUME"), 0, 100, 1, [st] { return st->volume; }, [st, &cx](int v) {
      st->volume = v;
      osd_show_level(cx, "volume", float(v) / 100.0f);
    }).unit("%").commit([&cx](int v) { cx.sv->toasts.push(tr("Volume saved") + std::string(": ") + std::to_string(v)); });
    m.slider(tr("SCREEN BRIGHTNESS"), 5, 100, 5, [st] { return st->brightness; }, [st, &cx](int v) {
      st->brightness = v;
      osd_show_level(cx, "brightness", float(v) / 100.0f);
    }).unit("%");
    m.header(tr("LISTS AND TEXT"));
    m.choice(tr("SHOW BATTERY STATUS"), {{"no", tr("NO")}, {"icon", tr("ICON")}, {"text", tr("TEXT")}}, [st] { return st->battery; },
             [st](const std::string& v) { st->battery = v; });
    m.choice(tr("TIME ZONE"), zones(), [st] { return st->zone; }, [st](const std::string& v) { st->zone = v; }).no_cycle();
    m.text(tr("WI-FI PASSWORD"), [st] { return st->wifi_key; }, [st](const std::string& v) { st->wifi_key = v; }).secret();
    m.text(tr("HOSTNAME"), [st] { return st->host; }, [st](const std::string& v) { st->host = v; });
    m.header(tr("ACTIONS"));
    m.submenu(tr("SOUND SETTINGS"), "menu.sound");
    m.action(tr("SHOW A TOAST"), [&cx] { cx.sv->toasts.push(tr("Wi-Fi connected: Casa 5G"), 4000); });
    m.action(tr("SHOW THE VOLUME BAR"), [&cx] { osd_show_level(cx, "volume", 0.62f, 4000); });
    m.action(tr("SHOW THE BRIGHTNESS BAR"), [&cx] { osd_show_level(cx, "brightness", 0.4f, 4000); });
    m.action(tr("ASK A QUESTION"), [] { return ui::confirm(tr("Delete all saved states of this game?"), nullptr); });
    m.action(tr("SHOW A MESSAGE"), [] { return ui::message(tr("The update was installed. Restart the console to use it."), nullptr, tr("UPDATE")); });
    m.action(tr("RESTART SYSTEM"), [&cx] { cx.sv->toasts.push(tr("Restarting...")); }).danger().confirm(tr("Restart the system now?"));
    m.header(tr("INFORMATION"));
    m.info(tr("IP ADDRESS"), "192.168.1.42");
    m.info(tr("VERSION"), "Zenith 1.0");
    m.separator();
    m.action(tr("DISABLED ENTRY"), [] {}).disabled();
  };
  return make_menu(std::move(s));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.demo", [](Ctx&, const std::string&) { return make_demo(); });

}  // namespace zf
