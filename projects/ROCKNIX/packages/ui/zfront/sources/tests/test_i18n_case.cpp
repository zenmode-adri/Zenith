// Sentence case of untranslated all-caps labels (i18n_case.cpp) and its hook in tr().
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "check.hpp"
#include "alloc_nothrow.hpp"
#include "i18n.hpp"
#include "services.hpp"
#include "settings.hpp"

using namespace zf;

static size_t g_allocs = 0;
void* operator new(size_t n) {
  ++g_allocs;
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static std::string sc(const char* s) { return i18n::sentence_case(s); }

int main() {
  // plain words
  CHECK_EQ(sc("GAME SETTINGS"), std::string("Game settings"));
  CHECK_EQ(sc("SWITCHES AND SLIDERS"), std::string("Switches and sliders"));
  CHECK_EQ(sc("YES"), std::string("Yes"));
  CHECK_EQ(sc("NO"), std::string("No"));
  CHECK_EQ(sc("SHOW BATTERY STATUS"), std::string("Show battery status"));
  // acronyms and brands keep their spelling, also at the start
  CHECK_EQ(sc("WI-FI PASSWORD"), std::string("Wi-Fi password"));
  CHECK_EQ(sc("WIFI"), std::string("Wi-Fi"));
  CHECK_EQ(sc("IP ADDRESS"), std::string("IP address"));
  CHECK_EQ(sc("SD CARD"), std::string("SD card"));
  CHECK_EQ(sc("CONTROLLER & BLUETOOTH"), std::string("Controller & Bluetooth"));
  CHECK_EQ(sc("USB, HDMI AND LED"), std::string("USB, HDMI and LED"));
  CHECK_EQ(sc("CPU GPU RGB BIOS DNS SSH OTA"), std::string("CPU GPU RGB BIOS DNS SSH OTA"));
  CHECK_EQ(sc("TRADUCCIÓN DE JUEGO POR IA"), std::string("Traducción de juego por IA"));
  CHECK_EQ(sc("PSP GBA NES SNES PS1 PS2 N64 NDS 3DS"), std::string("PSP GBA NES SNES PS1 PS2 N64 NDS 3DS"));
  CHECK_EQ(sc("RAM, FPS, CRT, LCD, ES, OS, ID, VM"), std::string("RAM, FPS, CRT, LCD, ES, OS, ID, VM"));
  CHECK_EQ(sc("MAME AND FBNEO WITH RETROARCH"), std::string("MAME and FBNeo with RetroArch"));
  CHECK_EQ(sc("OK"), std::string("OK"));
  CHECK_EQ(sc("SELECT ES THEME"), std::string("Select ES theme"));
  CHECK_EQ(sc("PS1/PS2 BIOS"), std::string("PS1/PS2 BIOS"));
  CHECK_EQ(sc("SD-CARD SPEED"), std::string("SD-card speed"));
  // hyphens, apostrophes, numbers, punctuation
  CHECK_EQ(sc("ON-SCREEN HELP"), std::string("On-screen help"));
  CHECK_EQ(sc("DON'T ASK AGAIN"), std::string("Don't ask again"));
  CHECK_EQ(sc("5GHZ NETWORK"), std::string("5GHz network"));
  CHECK_EQ(sc("1080P MODE"), std::string("1080p mode"));
  CHECK_EQ(sc("WIDTH: 640"), std::string("Width: 640"));
  CHECK_EQ(sc("(OPTIONAL) NAME"), std::string("(Optional) name"));
  CHECK_EQ(sc("DONE. RESTART NOW"), std::string("Done. Restart now"));
  CHECK_EQ(sc("LOADING..."), std::string("Loading..."));
  CHECK_EQ(sc("A"), std::string("A"));
  CHECK_EQ(sc("  SPACED   OUT "), std::string("  Spaced   out "));
  // accents (UTF-8, Latin-1 and Latin Extended-A)
  CHECK_EQ(sc("CONFIGURACI\xc3\x93N DE RED"), std::string("Configuraci\xc3\xb3n de red"));
  CHECK_EQ(sc("\xc3\x81LBUM \xc3\x91O\xc3\x91O"), std::string("\xc3\x81lbum \xc3\xb1o\xc3\xb1o"));
  CHECK_EQ(sc("\xc3\x89TICA"), std::string("\xc3\x89tica"));
  CHECK_EQ(sc("\xc5\x81\xc3\x93" "D\xc5\xb9"), std::string("\xc5\x81\xc3\xb3" "d\xc5\xba"));  // ŁÓDŹ -> Łódź
  CHECK_EQ(sc("WI-FI \xc3\x91"), std::string("Wi-Fi \xc3\xb1"));
  // strings that must not change
  CHECK_EQ(sc(""), std::string(""));
  CHECK_EQ(sc("1234"), std::string("1234"));
  CHECK_EQ(sc("---"), std::string("---"));
  CHECK_EQ(sc("Wi-Fi connected: Casa 5G"), std::string("Wi-Fi connected: Casa 5G"));
  CHECK_EQ(sc("Show clock"), std::string("Show clock"));
  CHECK_EQ(sc("SHOW the Clock"), std::string("SHOW the Clock"));  // already mixed: never touched
  CHECK_EQ(sc("%d DAYS ago"), std::string("%d DAYS ago"));
  CHECK_EQ(sc("RetroArch"), std::string("RetroArch"));
  CHECK_EQ(sc("\xd0\x94\xd0\x90"), std::string("\xd0\x94\xd0\x90"));  // Cyrillic: not our business
  CHECK_EQ(sc("\xe8\xae\xbe\xe7\xbd\xae"), std::string("\xe8\xae\xbe\xe7\xbd\xae"));
  CHECK_EQ(sc("%S IS READY"), std::string("%S is ready"));
  CHECK(i18n::is_all_caps("GAME 2"));
  CHECK(!i18n::is_all_caps("Game 2"));
  CHECK(!i18n::is_all_caps("123"));
  CHECK(!i18n::is_all_caps(""));
  // idempotent
  CHECK_EQ(sc(sc("WI-FI PASSWORD").c_str()), std::string("Wi-Fi password"));
  CHECK_EQ(sc("%d DAYS AGO"), std::string("%d days ago"));
  CHECK(i18n::is_all_caps("%d DAYS AGO"));
  CHECK(!i18n::is_all_caps("%d"));

  // display_text: interned, stable pointer, no allocation on a repeated call; mixed case comes back as the same pointer
  const char* a = i18n::display_text("GAME COLLECTION SETTINGS");
  CHECK_EQ(std::string(a), std::string("Game collection settings"));
  size_t before = g_allocs;
  const char* b = i18n::display_text("GAME COLLECTION SETTINGS");
  const char* mixed = "Already mixed";
  const char* c = i18n::display_text(mixed);
  CHECK(a == b);
  CHECK(c == mixed);
  CHECK_EQ(g_allocs - before, size_t(0));
  CHECK(i18n::display_text("") != nullptr);

  // tr(): without a translator it is the identity (no init), with the i18n translator and no catalog the sentence case
  CHECK_EQ(std::string(tr("SHOW CLOCK")), std::string("SHOW CLOCK"));
  CHECK(i18n::set_language(""));
  CHECK_EQ(std::string(tr("SHOW CLOCK")), std::string("Show clock"));
  CHECK_EQ(std::string(tr("Show clock")), std::string("Show clock"));
  CHECK_EQ(std::string(tr("WI-FI")), std::string("Wi-Fi"));
  CHECK_EQ(std::string(i18n::ctr("ctx", "DELETE")), std::string("Delete"));
  CHECK_EQ(std::string(i18n::ntr("%d GAME", "%d GAMES", 2)), std::string("%d games"));
  before = g_allocs;
  const char* t1 = tr("SHOW CLOCK");
  const char* t2 = tr("SHOW CLOCK");
  CHECK(t1 == t2);
  CHECK_EQ(g_allocs - before, size_t(0));
  return zt::done();
}
