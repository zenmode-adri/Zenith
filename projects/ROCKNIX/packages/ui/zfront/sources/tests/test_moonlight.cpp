// Moonlight tools: command lines, launch scripts, gamelist, the applist parser and UPDATE MOONLIGHT GAMES against a fake `curl`
// (a re-rooted tree: nothing outside the scratch folder is touched).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "check.hpp"
#include "moonlight.hpp"
#include "services.hpp"

using namespace zf;
namespace fs = std::filesystem;
namespace ml = zf::moonlight;

namespace {

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
void put(const fs::path& p, const std::string& t) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << t;
}

const char* kApplist =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?><root protocol_version=\"0.1\" status_code=\"200\">"
    "<App><IsHdrSupported>1</IsHdrSupported><AppTitle>Desktop</AppTitle><UniqueId>1</UniqueId><ID>881448767</ID></App>"
    "<App><AppTitle>Half-Life 2/Episode One</AppTitle><ID>1093255277</ID></App>"
    "<App><AppTitle>Evil \"$(touch x)\" `id`</AppTitle><ID>5</ID></App></root>";

}  // namespace

int main() {
  // ---- command lines
  CHECK_EQ(ml::quit_command("192.168.1.20"), std::string("moonlight quit 192.168.1.20"));
  CHECK_EQ(ml::pair_command("1234", "192.168.1.20", true), std::string("moonlight pair -pin 1234 192.168.1.20"));
  CHECK_EQ(ml::pair_command("1234", "192.168.1.20", false), std::string("QT_QPA_PLATFORM=wayland moonlight pair -pin 1234 192.168.1.20"));
  CHECK(ml::is_embedded("Moonlight Embedded 2.6.1") && !ml::is_embedded("moonlight-qt 5.0.1"));
  CHECK(ml::valid_host("192.168.1.20") && ml::valid_host("pc.local") && !ml::valid_host("") && !ml::valid_host("1.2.3.4; reboot") && !ml::valid_host("$(id)"));

  // ---- pairing output
  std::string ip;
  CHECK(ml::parse_server_ip("Connect to 10.1.10.217...", &ip) && ip == "10.1.10.217");
  CHECK(!ml::parse_server_ip("Connecting to 10.1.10.217...", &ip));
  CHECK(!ml::parse_server_ip("Connect to 10.1.10.217", &ip));
  CHECK(ml::paired_ok("Please enter the following PIN\nSuccesfully paired\n"));
  CHECK(!ml::paired_ok("Pairing failed\n"));
  CHECK(!ml::paired_ok("Succesfully paired and more"));  // ES compares the whole line

  // ---- applist
  std::vector<ml::App> apps;
  CHECK(ml::parse_applist(kApplist, apps));
  CHECK_EQ(apps.size(), size_t(3));
  CHECK_EQ(apps[0].title, std::string("Desktop"));
  CHECK_EQ(apps[0].id, std::string("881448767"));
  CHECK_EQ(apps[1].title, std::string("Half-Life 2/Episode One"));
  std::vector<ml::App> bad;  // a failed parse empties its output: not `apps`, which the gamelist part below uses
  CHECK(!ml::parse_applist("", bad));
  CHECK(!ml::parse_applist("<html>nope</html>", bad));

  // ---- scripts: embedded and Qt builds, and a hostile title
  CHECK_EQ(ml::script_name("Half-Life 2/Episode One"), std::string("Half-Life 2 Episode One"));
  std::string e = ml::script_text("10.0.0.5", "Desktop", false);
  CHECK(e.compare(0, 10, "#!/bin/sh\n") == 0);
  CHECK(e.find("/usr/bin/controller-layout moonlight \"Desktop.sh\" ") != std::string::npos);
  CHECK(e.find("moonlight stream -app \"Desktop\" -platform sdl 10.0.0.5\n") != std::string::npos);
  std::string q = ml::script_text("10.0.0.5", "Desktop", true);
  CHECK(q.find("QT_QPA_PLATFORM=wayland moonlight stream 10.0.0.5 \"Desktop\" --quit-after\n") != std::string::npos);
  std::string h = ml::script_text("10.0.0.5", "Evil \"$(touch x)\" `id`", false);
  CHECK(h.find("-app \"Evil \\\"\\$(touch x)\\\" \\`id\\`\"") != std::string::npos);
  CHECK(h.find("\n\"") == std::string::npos);

  // the script of a hostile title really runs as ONE argument, with its text, and nothing else runs
  {
    fs::path d = fs::temp_directory_path() / ("zf-ml-sh-" + std::to_string(::getpid()));
    fs::remove_all(d);
    fs::create_directories(d / "bin");
    put(d / "bin/moonlight", "#!/bin/sh\nfor a in \"$@\"; do printf '[%s]' \"$a\"; done > '" + (d / "args").string() + "'\n");
    put(d / "bin/controller-layout", "#!/bin/sh\nexit 0\n");
    ::chmod((d / "bin/moonlight").c_str(), 0755);
    ::chmod((d / "bin/controller-layout").c_str(), 0755);
    put(d / "t.sh", "PATH=" + (d / "bin").string() + ":$PATH\n" + std::string(h.begin() + h.find('\n') + 1, h.end()));
    // the controller-layout line points at /usr/bin: drop it for the run (the fake moonlight is the point)
    std::string body = slurp(d / "t.sh");
    size_t cl = body.find("/usr/bin/controller-layout");
    size_t cle = body.find('\n', cl);
    body.erase(cl, cle - cl + 1);
    put(d / "t.sh", body);
    std::string cmd = "cd " + d.string() + " && sh t.sh";
    CHECK_EQ(std::system(cmd.c_str()), 0);
    CHECK_EQ(slurp(d / "args"), std::string("[stream][-app][Evil \"$(touch x)\" `id`][-platform][sdl][10.0.0.5]"));
    CHECK(!fs::exists(d / "x"));
    fs::remove_all(d);
  }

  // ---- gamelist
  std::string gl = ml::gamelist_text(apps);
  CHECK(gl.find("<path>./Half-Life 2 Episode One.sh</path>") != std::string::npos);
  CHECK(gl.find("<name>Half-Life 2/Episode One</name>") != std::string::npos);
  CHECK(gl.find("<image>./images/Desktop.png</image>") != std::string::npos);
  CHECK(gl.find("favorite") == std::string::npos);
  // what the user set on a game that is still there survives the update; a game that is gone takes its entry with it
  {
    const std::string old = "<gameList><game><path>./Desktop.sh</path><name>Old name</name><image>./x.png</image><favorite>true</favorite>"
                            "<playcount>4</playcount></game><game><path>./Gone.sh</path><name>Gone</name><favorite>true</favorite></game></gameList>";
    const std::string g2 = ml::gamelist_text(apps, old);
    CHECK(g2.find("<name>Desktop</name>") != std::string::npos && g2.find("Old name") == std::string::npos && g2.find("./x.png") == std::string::npos);
    CHECK(g2.find("<favorite>true</favorite>") != std::string::npos && g2.find("<playcount>4</playcount>") != std::string::npos);
    CHECK(g2.find("Gone") == std::string::npos);
    CHECK_EQ(g2.find("<favorite>"), g2.rfind("<favorite>"));  // once: only Desktop's
    CHECK_EQ(ml::gamelist_text(apps, "not xml <"), gl);
  }

  // ---- certificates from Moonlight.conf and the request line
  CHECK_EQ(ml::extract_pem("a=1\ncertificate=\"@ByteArray(-----BEGIN CERTIFICATE-----\\nAAA\\n-----END CERTIFICATE-----\\n)\"\nkey=2\n", "certificate"),
           std::string("-----BEGIN CERTIFICATE-----\nAAA\n-----END CERTIFICATE-----\n"));
  CHECK_EQ(ml::extract_pem("a=1\n", "key"), std::string());
  std::string url = ml::make_url("10.0.0.5", "applist", "", "GUID");
  CHECK_EQ(url, std::string("https://10.0.0.5:47984/applist?uniqueid=0123456789ABCDEF&uuid=GUID"));
  CHECK_EQ(ml::make_url("10.0.0.5", "appasset", "appid=5", "G"), std::string("https://10.0.0.5:47984/appasset?appid=5&uniqueid=0123456789ABCDEF&uuid=G"));

  // ---- UPDATE MOONLIGHT GAMES on a fake curl, in a re-rooted tree
  {
    fs::path d = fs::temp_directory_path() / ("zf-ml-up-" + std::to_string(::getpid()));
    fs::remove_all(d);
    fs::create_directories(d / "tmp");
    ml::Paths saved = ml::paths();
    ml::paths().root = (d / "root").string();
    const fs::path roms = d / "root/storage/roms/moonlight";
    put(roms / "Old Game.sh", "old\n");
    put(roms / "images/Old Game.png", "png");
    // fake curl: writes the applist for /applist, a PNG for /appasset, fails when FAIL exists; logs the url
    put(d / "curl.sh",
        "#!/bin/sh\nout=''; url=''\nwhile [ $# -gt 0 ]; do case \"$1\" in -o) out=\"$2\"; shift;; http*) url=\"$1\";; esac; shift; done\n"
        "echo \"$url\" >> '" + (d / "urls").string() + "'\n[ -e '" + (d / "FAIL").string() + "' ] && exit 22\n"
        "case \"$url\" in *applist*) cat '" + (d / "applist.xml").string() + "' > \"$out\";; *) printf PNGDATA > \"$out\";; esac\n");
    put(d / "applist.xml", kApplist);
    ::chmod((d / "curl.sh").c_str(), 0755);
    ::setenv("ZF_CURL", (d / "curl.sh").c_str(), 1);
    ::setenv("ZF_ML_TMP", (d / "tmp").c_str(), 1);

    Services sv;
    sv.cmd.dry_run = false;
    // the server does not answer: nothing is removed
    put(d / "FAIL", "");
    ml::UpdateResult bad = ml::update_games(sv, "10.0.0.5");
    CHECK(!bad.ok);
    CHECK(fs::exists(roms / "Old Game.sh") && fs::exists(roms / "images/Old Game.png"));
    // a bad address never reaches a command
    CHECK(!ml::update_games(sv, "1.2.3.4; reboot").ok);
    fs::remove(d / "FAIL");
    // it answers: the old files go, one script per app, images, gamelist
    ml::UpdateResult ok = ml::update_games(sv, "10.0.0.5");
    CHECK(ok.ok);
    CHECK_EQ(ok.apps, size_t(3));
    CHECK(!fs::exists(roms / "Old Game.sh") && !fs::exists(roms / "images/Old Game.png"));
    CHECK(fs::exists(roms / "Desktop.sh") && fs::exists(roms / "Half-Life 2 Episode One.sh"));
    CHECK(slurp(roms / "Desktop.sh").find("-app \"Desktop\" -platform sdl 10.0.0.5") != std::string::npos);
    CHECK_EQ(slurp(roms / "images/Desktop.png"), std::string("PNGDATA"));
    CHECK(slurp(roms / "gamelist.xml").find("<name>Desktop</name>") != std::string::npos);
    CHECK(slurp(d / "urls").find("https://10.0.0.5:47984/appasset?appid=881448767&AssetType=2&AssetIdx=0&") != std::string::npos);

    ::unsetenv("ZF_CURL");
    ::unsetenv("ZF_ML_TMP");
    ml::paths() = saved;
    fs::remove_all(d);
  }

  // ---- unpair removes the folders of the build in use
  {
    fs::path d = fs::temp_directory_path() / ("zf-ml-un-" + std::to_string(::getpid()));
    fs::remove_all(d);
    ml::Paths saved = ml::paths();
    ml::paths().root = d.string();
    put(d / "storage/.cache/moonlight/client.pem", "x");
    CHECK(!ml::qt_build());
    CHECK(ml::unpair());
    CHECK(!fs::exists(d / "storage/.cache/moonlight"));
    ml::paths() = saved;
    fs::remove_all(d);
  }

  return zt::done();
}
