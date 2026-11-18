// The "Add games" card of the carousel and the phone row of the welcome's last step share this.
//
// The card is a carousel item that is not a system (`SystemItem::sys == kItemAddGames`, scene.hpp). `make_items` puts it first only
// on a console with no game at all: no game system (`doctor::is_game_system`: not Tools, Music, the image viewer) has a game. With
// one game anywhere it does not exist, and the carousel is exactly what it was. Its count is `kCountPending`, so no theme draws a
// "0 games" line or a "continue playing" row for it. When games arrive (the web transfer reloads the list) the card goes away.
#pragma once
#include <string>

#include "app.hpp"

namespace zf::addgames {

/// The console is on a network: WiFi on and an address. The phone page can only work then.
bool online(Ctx& cx);
/// Turns the web transfer on (as the TRANSFER FILES switch does; it stays on, PIN-protected) and opens its QR code. If the port is
/// taken: the setting goes back to off, a toast, and nothing opens.
Transition open_phone_page(Ctx& cx);
/// A on the card: the phone page when online; otherwise a message on how to copy games with the microSD card in a PC.
Transition open(Ctx& cx);
/// The card's name, in the current language.
std::string card_label();
/// The line under the carousel where a system has its game count (Zenith, Zenith Next): how games get here.
std::string card_hint();

}  // namespace zf::addgames
