// The Bezel Project installer: the output of `rocknix-es-thebezelproject list`, the command lines, when the entry is offered, and that a set
// that appears on disk is picked up by DECORATION SET (decoration_sets).
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "bezelproject.hpp"
#include "check.hpp"
#include "menu_game.hpp"

using namespace zf;
namespace fs = std::filesystem;

int main() {
  // ---- list output (ES getBatoceraBezelsList)
  const std::string out =
      "Checking systems...\n"
      "[A] snes - https://github.com/thebezelproject/bezelproject-SNES\n"
      "[I]\tgba  -  https://github.com/thebezelproject/bezelproject-GBA\r\n"
      "[X] nes - https://example.org/x\n"
      "[A] ? - https://example.org/unknown\n"
      "[A]\n"
      "\n"
      "[A] megadrive https://github.com/thebezelproject/bezelproject-MegaDrive\n";
  std::vector<bezel::Set> sets = bezel::parse_list(out);
  CHECK_EQ(sets.size(), size_t(3));
  if (sets.size() == 3) {
    CHECK(!sets[0].installed && sets[0].name == "snes" && sets[0].url == "https://github.com/thebezelproject/bezelproject-SNES");
    CHECK(sets[1].installed && sets[1].name == "gba" && sets[1].url == "https://github.com/thebezelproject/bezelproject-GBA");
    CHECK(!sets[2].installed && sets[2].name == "megadrive" && sets[2].url == "https://github.com/thebezelproject/bezelproject-MegaDrive");
  }
  CHECK(bezel::parse_list("").empty());
  CHECK(bezel::parse_list("Error : no network\n").empty());

  // ---- progress / error line
  CHECK_EQ(bezel::last_line("[snes] Installing bezels >>> 10%\n[snes] Installing bezels >>> 42%\n\n"), std::string("[snes] Installing bezels >>> 42%"));
  CHECK_EQ(bezel::last_line("Error : disk full"), std::string("Error : disk full"));
  CHECK_EQ(bezel::last_line("  \n"), std::string());

  // ---- commands
  ::unsetenv("ZF_BEZEL_CMD");
  CHECK_EQ(bezel::list_command(), std::string("rocknix-es-thebezelproject list"));
  CHECK_EQ(bezel::install_command("snes"), std::string("rocknix-es-thebezelproject install snes"));
  CHECK_EQ(bezel::remove_command("snes"), std::string("rocknix-es-thebezelproject remove snes"));
  ::setenv("ZF_BEZEL_CMD", "/tmp/fake bezel.sh", 1);
  CHECK_EQ(bezel::list_command(), std::string("'/tmp/fake bezel.sh' list"));
  ::unsetenv("ZF_BEZEL_CMD");
  CHECK(bezel::valid_name("snes") && bezel::valid_name("mame-libretro") && bezel::valid_name("a.b_c"));
  CHECK(!bezel::valid_name("") && !bezel::valid_name("a b") && !bezel::valid_name("x;reboot") && !bezel::valid_name("$(id)"));

  // ---- offered only with the script and a decorations folder that has something; the new set shows up in DECORATION SET
  {
    fs::path d = fs::temp_directory_path() / ("zf-bz-" + std::to_string(::getpid()));
    fs::remove_all(d);
    fs::create_directories(d);
    mg::Env saved = mg::env();
    mg::env().root = d.string();
    CHECK(!bezel::available());
    fs::create_directories(d / "usr/bin");
    std::ofstream(d / "usr/bin/rocknix-es-thebezelproject") << "#!/bin/sh\n";
    CHECK(!bezel::available());  // the script alone is not enough: there is nowhere to put the sets
    fs::create_directories(d / "storage/roms/bezels/default");
    CHECK(bezel::available());
    std::vector<std::string> before = mg::decoration_sets();
    CHECK(std::find(before.begin(), before.end(), "thebezelproject") == before.end());
    fs::create_directories(d / "storage/roms/bezels/thebezelproject/snes");  // what `install snes` leaves
    std::vector<std::string> after = mg::decoration_sets();
    CHECK(std::find(after.begin(), after.end(), "thebezelproject") != after.end());
    mg::env() = saved;
    fs::remove_all(d);
  }

  return zt::done();
}
