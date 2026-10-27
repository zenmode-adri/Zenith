// Menu groups (menu.g.*): the main menu and the group screens are organised by what the person wants to do, not by the
// order ES grew its menus in. Nothing here has logic of its own: a group screen is a few of the existing sections put
// together (the same rows, the same keys, the same scripts), so ES and zfront keep sharing every setting.
//
//   main menu     Games | Screen & sound | Battery & performance | Controller & Bluetooth | Network | Library | System | Quit
//                 (no quick settings: the volume and the brightness have hardware buttons)
//   menu.g.screen    brightness (+ panel adjustment one level down), audio output, LEDs, top bar (clock, battery)
//   menu.g.power     governors, cooling, then IDLE (suspend, screensaver), battery warning; "Advanced" = overclock
//   menu.g.library   collections, scraper, storage (SD card)
//   menu.g.system    information, language and time, updates, maintenance, "Advanced"
// The "switch confirm & cancel" toggle lives in CONTROLLER & BLUETOOTH. Nothing is left of USER INTERFACE as a group.
// The section screens (menu.system, menu.sound, menu.ui, ...) stay registered with all their rows.
#pragma once
#include <string>
#include <vector>

#include "app.hpp"
#include "menu.hpp"

namespace zf {

/// Parts of `menu.system` (bit mask). `kSecAll` = the complete screen, as before.
namespace sys_sec {
enum : unsigned {
  kBright = 1,    // SCREEN BRIGHTNESS
  kPanel = 2,     // PANEL ADJUSTMENT
  kLeds = 4,      // LEDS
  kLang = 8,      // LANGUAGE, KEYBOARD, TIMEZONE
  kStorage = 16,  // AUTODETECT GAMES CARD ... EJECT MICROSD CARD
  kPower = 32,    // COOLING PROFILE, CPU/GPU governors
  kTurbo = 64,    // CPU/GPU overclock, MangoHud
  kSysAdv = 128,  // touch keyboard, THIS DEVICE IS, GPU DRIVER, ROOT PASSWORD
  kInfo = 256,    // INFORMATION
  kReset = 512,   // SYSTEM MANAGEMENT AND RESET
  kSuspend = 1024,  // DEVICE SUSPEND MODE
  kNoHdr = 0x4000,  // leave out the section headers (a row of its own inside another menu)
  kAll = 0x3FFF,
};
}  // namespace sys_sec

/// Parts of `menu.sound`.
namespace snd_part {
enum : unsigned {
  kQuick = 1,  // the volume slider alone
  kNav = 32,   // ENABLE NAVIGATION SOUNDS alone
  kOverlay = 16,  // SHOW OVERLAY WHEN VOLUME CHANGES alone (the volume itself is the hardware buttons' job)
  kVol = 2,    // VOLUME: slider + overlay toggle
  kAudio = 4,  // AUDIO OUTPUT / AUDIO PROFILE
  kBatt = 8,   // audible battery warning + threshold
  kAll = kVol | kAudio | kBatt | kNav,
};
}  // namespace snd_part

/// Parts of `menu.ui`.
namespace ui_part {
enum : unsigned {
  kLang = 1,     // LANGUAGE (REGION)
  kControl = 2,  // SWITCH CONFIRM & CANCEL
  kBright = 4,   // SCREEN BRIGHTNESS
  kDisplay = 8,  // SHOW CLOCK, SHOW BATTERY STATUS
  kSaver = 16,   // SCREENSAVER
  kNoHdr = 64,   // leave out the SCREENSAVER SETTINGS header (the group puts its own)
  kTopBar = 32,  // SHOW CLOCK, SHOW BATTERY STATUS under their own header (kDisplay = the same rows under DISPLAY OPTIONS)
  kAll = 31,
};
}  // namespace ui_part

MenuSpec system_spec(Ctx& cx, unsigned sections);  // menu_system_main.cpp
MenuSpec sound_spec(Ctx& cx, unsigned parts);      // menu_sound.cpp
MenuSpec ui_spec(unsigned parts);                  // menu_ui.cpp
/// True when the console has something for `sys_sec::kTurbo` or `sys_sec::kSysAdv` (a console without them gets no "Advanced" row).
bool system_has_section(unsigned section);

/// One menu made of several: the rows of each in order; opens and closes run for all of them.
MenuSpec merge_specs(std::string title, const char* title_key, std::vector<MenuSpec> parts);

}  // namespace zf
