// Launch audit (w1): what zfront executes for ROMs with awkward names, for each distinct command template of the real
// es_systems.cfg (runemu.sh template, `/usr/bin/run %ROM%`, `/bin/sh %ROM%`), compared with what a real `sh -c` makes of the
// same line (that is what ES runs). Also: placeholders are replaced in one pass, `#`/`~` inside a name keep the direct spawn
// and the idle preparation, and the emulator/core override rules of ES ("auto", invalid per-game value -> system-wide value).
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "launch.hpp"
#include "systems.hpp"

using namespace zf;
using std::string;
using std::vector;

namespace {

string tmpdir(const char* tag) {
  string d = string("/tmp/zf-launchaudit-") + tag + "-" + std::to_string(::getpid());
  string c = "rm -rf " + d + " && mkdir -p " + d;
  if (std::system(c.c_str()) != 0) std::abort();
  return d;
}

// Prints every argument followed by NUL into $1-file (path baked in), so the test sees the exact argv.
void write_printer(const string& path, const string& out) {
  std::ofstream(path) << "#!/bin/sh\nrm -f '" << out << "'\nfor a in \"$@\"; do printf '%s\\0' \"$a\" >> '" << out << "'; done\n: >> '" << out << "'\n";
  ::chmod(path.c_str(), 0755);
}

vector<string> read_argv(const string& path) {
  std::ifstream in(path, std::ios::binary);
  string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  vector<string> v;
  size_t i = 0;
  while (i < all.size()) {
    size_t z = all.find('\0', i);
    if (z == string::npos) break;
    v.push_back(all.substr(i, z - i));
    i = z + 1;
  }
  return v;
}

string first_word_replaced(const string& cmd, const string& prog) {
  size_t sp = cmd.find(' ');
  return sp == string::npos ? prog : prog + cmd.substr(sp);
}

}  // namespace

int main() {
  const string d = tmpdir("a");
  const string printer = d + "/argv.sh", out = d + "/out.bin";
  write_printer(printer, out);

  // The distinct templates of the fork's es_systems.cfg (130 systems share the first one).
  struct Tpl {
    const char* cmd;
    bool runemu;
  };
  const Tpl tpls[] = {
      {"/usr/bin/runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR% --controllers=\"%CONTROLLERSCONFIG%\"", true},
      {"/usr/bin/run %ROM%", false},
      {"/bin/sh %ROM%", false},
  };
  // Names that ROM sets really have, plus hostile ones. `simple` = split_simple must accept the line (direct spawn, no sh).
  struct Nm {
    string name;
    bool simple;
  };
  const vector<Nm> names = {
      {"plain", true},
      {"with space", true},
      {"it's (1) [x]", true},
      {"a&b $c ^d !e", true},
      {"{br} *star? <lt> ;semi", true},
      {"back`tick`|pipe", true},
      {"caf\xC3\xA9 \xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0 \xE2\x9D\xA4", true},
      {"q\"uote", true},
      {string(200, 'x'), true},
      {"Game -Part 2 --core=zzz", true},
      {"-dash", true},
      {"(USA) (Rev 1) [!]", true},
      {"tab\tname", true},
      {"trail ", true},
      {" lead", true},
      {"100% done", true},
      {"Game #1", true},                    // `#` inside a word is plain for sh
      {"Final Fantasy VII ~ Disc 1", true},  // `~` too
      {"a#b~c", true},
      {"a=b", true},
      {"a=~b", false},                      // `~` right after `=`: kept conservative (falls back to sh, still correct)
      {"Foo %CORE% %SYSTEM% %ROM%", true},  // placeholders inside a value must not be expanded again
      {"%EMULATOR%", true},
  };
  const char* exts[] = {".chd", ".gdi", ".nds", ".z64", ".iso", ".zip", ".m3u"};

  System sys;
  sys.name = "dreamcast";
  sys.fullname = "Dreamcast";
  sys.emulator = "flycast";
  sys.core = "flycast2022-sa";
  sys.emus = {{"flycast", {"flycast2022-sa", "flycast-sa"}}, {"retroarch", {"flycast2021"}}};

  int n = 0;
  for (const Tpl& t : tpls) {
    sys.command = t.cmd;
    for (const Nm& nm : names) {
      Game g;
      g.name = nm.name;
      g.path = "/storage/roms/dreamcast/sub dir/" + nm.name + exts[n++ % 7];
      string cmd = launch::build_command(sys, g, ConfMap(), {});
      string line = first_word_replaced(cmd, printer);

      vector<string> want = {g.path};
      if (t.runemu) {
        want.push_back("-Pdreamcast");
        want.push_back("--core=flycast2022-sa");
        want.push_back("--emulator=flycast");
        want.push_back("--controllers=");
      }

      // ground truth: what `sh -c` makes of the line
      string shf = d + "/line.sh";
      std::ofstream(shf) << line << "\n";
      CHECK_EQ(std::system(("sh " + shf).c_str()), 0);
      vector<string> via_sh = read_argv(out);
      if (via_sh != want) std::fprintf(stderr, "  sh argv differs for name '%s' template '%s'\n", nm.name.c_str(), t.cmd);
      CHECK(via_sh == want);

      // zfront's direct path (posix_spawn of the split words) must give the same argv, and be taken when the line is simple
      vector<string> words;
      bool simple = launch::split_simple(line, words);
      if (nm.simple && !simple) std::fprintf(stderr, "  not simple: '%s'\n", nm.name.c_str());
      CHECK_EQ(simple, nm.simple);
      CHECK_EQ(launch::run(line).exit_code, 0);
      vector<string> via_run = read_argv(out);
      if (via_run != want) std::fprintf(stderr, "  run argv differs for name '%s' template '%s'\n", nm.name.c_str(), t.cmd);
      CHECK(via_run == want);
      if (simple) {
        vector<string> rest(words.begin() + 1, words.end());
        CHECK(rest == want);
      }
    }
  }

  // %GAMENAME% / %SYSTEMNAME% with a placeholder in the value: one pass, the value is not expanded again
  {
    System s2;
    s2.name = "gb";
    s2.fullname = "Game %SYSTEM%";
    s2.emulator = "retroarch";
    s2.core = "gambatte";
    s2.emus = {{"retroarch", {"gambatte"}}};
    s2.command = printer + " %GAMENAME% %SYSTEMNAME% %BASENAME%";
    Game g;
    g.name = "x %CORE% y";
    g.path = "/r/gb/%SYSTEM%.gb";
    string cmd = launch::build_command(s2, g, ConfMap(), {});
    string shf = d + "/l2.sh";
    std::ofstream(shf) << cmd << "\n";
    CHECK_EQ(std::system(("sh " + shf).c_str()), 0);
    vector<string> want = {"x %CORE% y", "Game %SYSTEM%", "%SYSTEM%"};
    CHECK(read_argv(out) == want);
    // an unknown or unterminated placeholder stays as it is
    s2.command = "x %NOPE% 100% %ROM_RAW";
    CHECK_EQ(launch::build_command(s2, g, ConfMap(), {}), string("x %NOPE% 100% %ROM_RAW"));
  }

  // idle preparation of a ROM whose name has `#` and `~`: the --prepare line must be spawnable directly (no sh fallback there)
  {
    string runemu = d + "/runemu.sh";
    string pout = d + "/prep.bin";
    write_printer(runemu, pout);
    sys.command = runemu + " %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR% --controllers=\"%CONTROLLERSCONFIG%\"";
    Game g;
    g.name = "Mega Man #2 ~ Wily";
    g.path = "/storage/roms/dreamcast/Mega Man #2 ~ Wily.chd";
    string cmd = launch::build_command(sys, g, ConfMap(), {});
    int pid = launch::prepare(cmd, launch::idle_spawner());
    CHECK(pid > 0);
    if (pid > 0) {
      int st = 0;
      ::waitpid(pid, &st, 0);
      vector<string> v = read_argv(pout);
      CHECK_EQ(v.size(), size_t(6));
      if (v.size() == 6) {
        CHECK_EQ(v[0], string("--prepare"));
        CHECK_EQ(v[1], g.path);
      }
    }
  }

  // emulator / core: ES treats "auto" and an invalid per-game value as "not set" and uses the system-wide value
  {
    System s;
    s.name = "gb";
    s.emulator = "retroarch";
    s.core = "gambatte";
    s.emus = {{"retroarch", {"gambatte", "mgba"}}, {"standalone", {"sa-core", "sa-core2"}}};
    Game g;
    g.path = "/r/gb/a.gb";
    ConfMap c;
    c["gb.emulator"] = "standalone";
    c["gb[\"a.gb\"].emulator"] = "auto";
    auto r = resolve_emu(s, g, c);
    CHECK_EQ(r.first, string("standalone"));
    CHECK_EQ(r.second, string("sa-core"));
    c["gb[\"a.gb\"].emulator"] = "not-an-emulator";
    CHECK_EQ(resolve_emu(s, g, c).first, string("standalone"));
    c["gb[\"a.gb\"].emulator"] = "retroarch";
    CHECK_EQ(resolve_emu(s, g, c).first, string("retroarch"));
    CHECK_EQ(resolve_emu(s, g, c).second, string("gambatte"));
    c.clear();
    c["gb.core"] = "mgba";
    c["gb[\"a.gb\"].core"] = "bogus";  // invalid for the emulator that runs: the system-wide core still applies
    CHECK_EQ(resolve_emu(s, g, c).second, string("mgba"));
    c["gb[\"a.gb\"].core"] = "auto";
    CHECK_EQ(resolve_emu(s, g, c).second, string("mgba"));
    c["gb[\"a.gb\"].core"] = "gambatte";
    CHECK_EQ(resolve_emu(s, g, c).second, string("gambatte"));
    c.clear();
    c["gb[\"a.gb\"].emulator"] = "";
    CHECK_EQ(resolve_emu(s, g, c).first, string("retroarch"));
  }

  if (std::system(("rm -rf " + d).c_str()) != 0) return 1;
  return zt::done();
}
