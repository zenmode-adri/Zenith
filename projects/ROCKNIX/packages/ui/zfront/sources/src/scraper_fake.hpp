// Shared by the scraper tests: a fake `curl` (shell script written at run time, picked with $ZF_CURL) that answers the API and
// the media from files of a scratch directory, and a rig (scratch ROM folder + Ctx) around it.
#pragma once
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "scraper_run.hpp"

namespace scrfake {
using namespace zf;
using namespace zf::scraper;
namespace fs = std::filesystem;


inline std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
inline void put(const std::string& path, const std::string& text) {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream f(path, std::ios::binary);
  f << text;
}
inline size_t count_of(const std::string& hay, const std::string& needle) {
  size_t n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
  return n;
}
inline std::string replace_all(std::string s, const std::string& a, const std::string& b) {
  for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
  return s;
}

inline const char* kFakeCurl = R"SH(#!/bin/bash
D="$ZF_FAKE_DIR"
[ "$1" = "-K" ] || exit 2
cfg="$2"
url=""; out=""
emit() {
  [ -z "$url" ] && return
  echo "$url" >> "$D/requests.log"
  if [ -f "$D/down" ]; then printf '@@6|000|0||\n'; return; fi
  code=200; ctype="text/xml"; retry=""
  case "$url" in
    *ssuserInfos.php*)
      code=$(cat "$D/user.http" 2>/dev/null || echo 200)
      cat "$D/user.xml" > "$out" ;;
    *jeuInfos.php*)
      rom=$(printf '%s' "$url" | sed -n 's/.*romnom=\([^&]*\).*/\1/p')
      if [ -f "$D/once429" ]; then rm -f "$D/once429"; code=429; retry=1; echo "too fast" > "$out"
      elif [ -f "$D/game/$rom.http" ]; then code=$(cat "$D/game/$rom.http"); echo "error" > "$out"
      elif [ -f "$D/game.http" ]; then code=$(cat "$D/game.http"); echo "error" > "$out"
      elif [ -f "$D/game/$rom.xml" ]; then cat "$D/game/$rom.xml" > "$out"
      elif [ -f "$D/game/default.xml" ]; then cat "$D/game/default.xml" > "$out"
      else code=404; echo "Erreur : Rom/Iso/Dossier non trouvee !" > "$out"; fi ;;
    *media*)
      m=$(printf '%s' "$url" | sed -n 's/.*media=\([^&(]*\).*/\1/p')
      if [ -f "$D/media.http" ]; then code=$(cat "$D/media.http"); echo "error" > "$out"
      elif [ -f "$D/nomedia" ]; then printf 'NOMEDIA' > "$out"; ctype="text/plain"
      else
        case "$m" in
          ss|sstitle|wheel|wheel-hd|box-3D) printf '\x89PNG\r\n\x1a\nFAKEPNG' > "$out"; ctype="image/png" ;;
          box-2D|maps|fanart) printf '\xff\xd8\xff\xe0FAKEJPG' > "$out"; ctype="image/jpeg" ;;
          video-normalized) printf '\x00\x00\x00\x18ftypmp42FAKEMP4' > "$out"; ctype="video/mp4" ;;
          manuel) printf '%%PDF-1.4 FAKE' > "$out"; ctype="application/pdf" ;;
          *) code=404; echo "x" > "$out" ;;
        esac
      fi ;;
    *) code=404; echo "x" > "$out" ;;
  esac
  size=$(stat -c %s "$out" 2>/dev/null || echo 0)
  printf '@@0|%s|%s|%s|%s\n' "$code" "$size" "$retry" "$ctype"
}
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    next) emit; url=""; out="" ;;
    'url = "'*) url="${line#url = \"}"; url="${url%\"}" ;;
    'output = "'*) out="${line#output = \"}"; out="${out%\"}" ;;
  esac
done < "$cfg"
emit
exit 0
)SH";

inline const char* kUserOk =
    "<Data><ssuser><id>testuser</id><maxthreads>1</maxthreads><requeststoday>12</requeststoday><requestskotoday>1</requestskotoday>"
    "<maxrequestspermin>20</maxrequestspermin><maxrequestsperday>20000</maxrequestsperday><maxrequestskoperday>2000</maxrequestskoperday></ssuser></Data>";

struct Rig {
  std::string root, fake;
  Ctx cx;
  Rig() {
    char dir[] = "/tmp/zf-scr-XXXXXX";
    root = ::mkdtemp(dir);
    fake = root + "/fake";
    fs::create_directories(fake + "/game");
    put(fake + "/curl", kFakeCurl);
    ::chmod((fake + "/curl").c_str(), 0755);
    put(fake + "/user.xml", kUserOk);
    ::setenv("ZF_CURL", (fake + "/curl").c_str(), 1);
    ::setenv("ZF_FAKE_DIR", fake.c_str(), 1);
    ::setenv("ZF_SS_DEVLOGIN", "devid=DEVIDSECRET&devpassword=DEVPWSECRET", 1);
    ::setenv("ZF_SCRAPER_TMP", (root + "/tmp").c_str(), 1);
    fs::create_directories(root + "/tmp");
    cx.sv->cmd.dry_run = false;
    cx.sv->cmd.keep_log = true;
    cx.es = EsSettings::from_text("<config/>");
  }
  ~Rig() {
    for (const char* k : {"ZF_CURL", "ZF_FAKE_DIR", "ZF_SS_DEVLOGIN", "ZF_SCRAPER_TMP"}) ::unsetenv(k);
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  std::string roms() const { return root + "/roms/snes"; }
  void systems() {
    std::string xml =
        "<systemList><system><name>snes</name><fullname>Super Nintendo</fullname><path>/storage/roms/snes</path><extension>.sfc .zip</extension>"
        "<platform>snes</platform><command>run</command></system></systemList>";
    std::string err;
    CHECK(load_systems_text(xml, root + "/roms", true, cx.systems, err));
  }
  // three systems: snes and gba (ScreenScraper knows them) and ports (it does not)
  void systems3() {
    std::string xml =
        "<systemList>"
        "<system><name>snes</name><fullname>Super Nintendo</fullname><path>/storage/roms/snes</path><extension>.sfc</extension><platform>snes</platform><command>r</command></system>"
        "<system><name>gba</name><fullname>Game Boy Advance</fullname><path>/storage/roms/gba</path><extension>.gba</extension><platform>gba</platform><command>r</command></system>"
        "<system><name>ports</name><fullname>Ports</fullname><path>/storage/roms/ports</path><extension>.sh</extension><platform>ports</platform><command>r</command></system>"
        "</systemList>";
    put(root + "/roms/gba/x.gba", "x");
    put(root + "/roms/ports/p.sh", "x");
    std::string err;
    CHECK(load_systems_text(xml, root + "/roms", true, cx.systems, err));
  }
  void rom(const std::string& name, const std::string& body = "ROM") { put(roms() + "/" + name, body + name); }
  // a ScreenScraper answer for `stem.sfc`
  void known(const std::string& stem) {
    std::string x = slurp(std::string(ZF_TEST_DATA) + "/scraper/jeu_sfc.xml");
    put(fake + "/game/" + stem + ".sfc.xml", replace_all(x, "Super Mario World", "Game " + stem));
  }
  Options opts() {
    Options o = Options::load(cx);
    o.systems = {0};
    o.min_interval_ms = 0;
    o.follow_account_rate = false;
    o.busy_retry_ms = 20;
    o.skip_days = 0;
    o.filter = Options::Filter::All;
    o.flush_every = 2;
    return o;
  }
  // drives the main thread until the run ends (or `stop` says so); the real loop does the same from App::tick
  template <class F>
  bool pump(Run& r, F stop, int ms = 20000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
      cx.sv->jobs.drain();
      r.tick();
      if (stop()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }
  bool run_to_end(Run& r) {
    r.start();
    return pump(r, [&] { return r.finished(); });
  }
  std::string requests() { return slurp(fake + "/requests.log"); }
  std::string gamelist() { return slurp(roms() + "/gamelist.xml"); }
};

inline bool has_secret(const std::string& s) {
  for (const char* k : {"DEVIDSECRET", "DEVPWSECRET", "USERSECRET", "PASSSECRET"})
    if (s.find(k) != std::string::npos) return true;
  return false;
}


}  // namespace scrfake
