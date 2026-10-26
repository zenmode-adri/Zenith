#include <cstdio>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

#include "check.hpp"
#include "systems.hpp"
#include "util.hpp"

using namespace zf;

static void touch(const std::string& p) {
  FILE* f = std::fopen(p.c_str(), "w");
  if (f) std::fclose(f);
}

int main() {
  std::string root = "/tmp/zf_test_roms_" + std::to_string(getpid());
  mkdir(root.c_str(), 0755);
  mkdir((root + "/gbc").c_str(), 0755);
  mkdir((root + "/empty").c_str(), 0755);
  for (const char* n : {"The Zelda.gbc", "A Bug.gbc", "Mario.gbc", "Hidden.gbc", "notes.txt"}) touch(root + "/gbc/" + n);
  FILE* gl = std::fopen((root + "/gbc/gamelist.xml").c_str(), "w");
  std::fputs("<gameList><game><path>./Hidden.gbc</path><name>Hidden</name><hidden>true</hidden></game>"
             "<game><path>./Mario.gbc</path><name>Mario</name><sortname>Aardvark Mario</sortname></game></gameList>",
             gl);
  std::fclose(gl);

  const std::string xml =
      "<systemList>"
      "<system><name>gbc</name><fullname>Game Boy Color</fullname><path>/storage/roms/gbc</path><extension>.gbc</extension>"
      "<command>run %ROM% %EMULATOR% %CORE%</command>"
      "<emulators><emulator name=\"retroarch\"><cores><core default=\"true\">gambatte</core><core>mgba</core></cores></emulator></emulators></system>"
      "<system><name>empty</name><fullname>Empty</fullname><path>/storage/roms/empty</path><extension>.x</extension><command>c</command></system>"
      "</systemList>";
  std::vector<System> v;
  std::string err;
  CHECK(load_systems_text(xml, root, true, v, err));
  CHECK_EQ(v.size(), size_t(1));  // systems without games are dropped
  if (v.size() == 1) {
    const System& s = v[0];
    CHECK_EQ(s.games.size(), size_t(3));  // hidden removed, notes.txt ignored
    // ES order: sortname/name case-insensitive, leading article ignored: "Aardvark Mario" < "Bug" < "Zelda"
    CHECK(s.games[0].name == "Mario");
    CHECK(s.games[1].name == "A Bug");
    CHECK(s.games[2].name == "The Zelda");
    CHECK(s.emulator == "retroarch" && s.core == "gambatte");

    ConfMap conf;
    CHECK(resolve_emu(s, s.games[0], conf).second == "gambatte");
    conf["gbc.core"] = "mgba";
    CHECK(resolve_emu(s, s.games[0], conf).second == "mgba");
    conf["gbc.core"] = "bogus";  // not one of the system's cores: ignored
    CHECK(resolve_emu(s, s.games[0], conf).second == "gambatte");
    conf["gbc[\"Mario.gbc\"].core"] = "mgba";
    CHECK(resolve_emu(s, s.games[0], conf).second == "mgba");
    // ROM names with '=' or '#': ES stores the per-game keys without those characters
    Game odd;
    odd.path = root + "/gbc/Mario #1 = Land.gbc";
    conf["gbc[\"Mario 1  Land.gbc\"].core"] = "mgba";
    CHECK(resolve_emu(s, odd, conf).second == "mgba");
  }

  // index mode: counts files without reading the gamelist, open() fills the games
  std::vector<System> idx;
  CHECK(load_systems_text(xml, root, false, idx, err));
  CHECK_EQ(idx.size(), size_t(1));
  if (idx.size() == 1) {
    CHECK(idx[0].games.empty());
    CHECK_EQ(idx[0].count, size_t(4));  // hidden games are counted until the list is opened
    idx[0].open();
    CHECK_EQ(idx[0].games.size(), size_t(3));
    CHECK_EQ(idx[0].count, size_t(3));
  }

  CHECK(sort_key("The Legend") == sort_key("Legend"));
  CHECK(sort_key("ABC") == sort_key("abc"));

  std::string cmd = "rm -rf " + root;
  int rc = std::system(cmd.c_str());
  (void)rc;
  return zt::done();
}
