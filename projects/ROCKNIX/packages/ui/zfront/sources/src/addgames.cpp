#include "addgames.hpp"

#include "i18n.hpp"
#include "menu_network.hpp"  // netmenu::local_ip_list()
#include "ui_dialogs.hpp"
#include "webtransfer.hpp"

namespace zf::addgames {

bool online(Ctx& cx) { return cx.conf.get_bool("wifi.enabled", false) && !netmenu::local_ip_list().empty(); }

Transition open_phone_page(Ctx& cx) {
  // Already on (TRANSFER FILES, stored, or a temporary one): as it is. Otherwise on for a while only: not stored, so a reboot does not
  // bring it back, and off by itself after 20 min with no request from the phone (webtransfer::idle_expired, App::poll_status).
  if (!cx.conf.get_bool("zfront.webtransfer", false) && !webtransfer::enabled()) {
    if (!webtransfer::apply(true)) {  // the port may be taken
      webtransfer::set_enabled(false);
      cx.sv->toasts.push(tr("Could not start the web transfer"), 3500);
      return Transition::none();
    }
    webtransfer::set_temporary(true);
  }
  return Transition::open("menu.webqr", "addgames");
}

Transition open(Ctx& cx) {
  if (online(cx)) return open_phone_page(cx);
  return ui::message(tr("No network. Put the microSD card in a PC and copy each game into the folder of its system, inside roms "
                        "(for example roms/snes). Or connect to WiFi and come back here to add games from your phone."),
                     nullptr, card_label());
}

std::string card_label() { return tr("Add games"); }
std::string card_hint() { return tr("From your phone or microSD"); }

}  // namespace zf::addgames
