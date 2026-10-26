// scraper engine end to end WITHOUT network: a fake `curl` (a shell script written at run time, selected with $ZF_CURL) answers
// the API and the media from files of a scratch directory. Covers: found / not found, gamelist round trip (untouched bytes
// identical), media on disk, cancel and resume, network down, quota, bad login, rate limit retry, NOMEDIA texts, and that no
// credential ever reaches a command line, a toast or the error list.
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
#include "scraper_fake.hpp"
#include "scraper_run.hpp"

using namespace zf;
using namespace zf::scraper;
using namespace scrfake;
namespace fs = std::filesystem;

int main() {
  // ---- found / not found, round trip, media, secrets
  {
    Rig t;
    t.cx.es.set_string(key::kUser, "USERSECRET");
    t.cx.es.set_string(key::kPass, "PASSSECRET");
    t.rom("a.sfc");
    t.rom("b.sfc");
    t.rom("c.sfc");
    t.known("a");
    t.known("b");
    const std::string before =
        "<?xml version=\"1.0\"?>\n<gameList>\n\t<!-- my comment -->\n\t<game id=\"7\" source=\"x\">\n\t\t<path>./a.sfc</path>\n\t\t<name>Old A</name>\n"
        "\t\t<favorite>true</favorite>\n\t\t<mystuff attr=\"1\">keep me</mystuff>\n\t</game>\n\t<game>\n\t\t<path>./b.sfc</path>\n\t\t<playcount>3</playcount>\n\t</game>\n"
        "\t<folder><path>./sub</path></folder>\n</gameList>\n";
    put(t.roms() + "/gamelist.xml", before);
    t.systems();
    CHECK(available());
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK(r->phase() == Phase::Finished);
    CHECK_EQ(r->stats().total, 3u);
    CHECK_EQ(r->stats().found, 2u);
    CHECK_EQ(r->stats().not_found, 1u);
    CHECK_EQ(r->stats().errors, 0u);
    CHECK(r->stats().media >= 6);
    std::string gl = t.gamelist();
    CHECK(gl.find("Game a") != std::string::npos);
    CHECK(gl.find("Game b") != std::string::npos);
    CHECK(gl.find("<!-- my comment -->") != std::string::npos);
    CHECK(gl.find("<mystuff attr=\"1\">keep me</mystuff>") != std::string::npos);
    CHECK(gl.find("<favorite>true</favorite>") != std::string::npos);
    CHECK(gl.find("<playcount>3</playcount>") != std::string::npos);
    CHECK(gl.find("<folder><path>./sub</path></folder>") != std::string::npos);
    CHECK(gl.find("id=\"7\" source=\"x\"") != std::string::npos);
    CHECK(gl.find("./images/a-image.png") != std::string::npos);
    CHECK(gl.find("./images/a-thumb.jpg") != std::string::npos);
    CHECK(gl.find("./videos/a-video.mp4") != std::string::npos);
    CHECK(gl.find("./manuals/a-manual.pdf") != std::string::npos);
    CHECK(gl.find("<scrap name=\"ScreenScraper\"") != std::string::npos);
    CHECK(fs::exists(t.roms() + "/images/a-image.png"));
    CHECK(fs::exists(t.roms() + "/videos/b-video.mp4"));
    CHECK(!fs::exists(t.roms() + "/images/c-image.png"));
    CHECK(count_of(gl, "<game") >= 3);  // c.sfc got an entry (its hashes and the stamp) so the next run can skip it
    // the in-memory list was refreshed in place
    bool seen = false;
    for (const Game& g : t.cx.systems[0].games)
      if (g.path == t.roms() + "/a.sfc") seen = (g.name == "Game a (USA)");
    CHECK(seen);
    // the server saw the hash of the ROM
    CHECK(t.requests().find("romnom=a.sfc") != std::string::npos);
    CHECK(t.requests().find("md5=") != std::string::npos);
    // secrets: the fake saw them, nothing of ours shows them
    CHECK(has_secret(t.requests()));
    for (const std::string& c : t.cx.sv->cmd.log) CHECK(!has_secret(c));
    for (const Toast& ts : t.cx.sv->toasts.items()) CHECK(!has_secret(ts.text));
    for (const ErrLine& e : r->errors()) CHECK(!has_secret(e.game));
    CHECK(!has_secret(r->game_name()) && !has_secret(r->system_name()));
    // second run: nothing changes except the stamps; same bytes for everything the scraper does not own
    std::string once = gl;
    auto r2 = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r2));
    std::string twice = t.gamelist();
    CHECK_EQ(count_of(twice, "<mystuff attr=\"1\">keep me</mystuff>"), 1u);
    CHECK_EQ(count_of(twice, "<name>Game a (USA)</name>"), 1u);
    r.reset();
    r2.reset();
    CHECK(fs::is_empty(t.root + "/tmp"));  // the private directory of credentials is gone
  }

  // ---- cancel in the middle, nothing lost, then resume skipping what is done
  {
    Rig t;
    for (int i = 0; i < 10; ++i) {
      std::string n = "g" + std::to_string(i);
      t.rom(n + ".sfc");
      t.known(n);
    }
    t.systems();
    Options o = t.opts();
    o.skip_days = 15;
    o.flush_every = 100;  // the final write on cancel is what saves the work
    auto r = Run::create(t.cx, o);
    r->start();
    CHECK(t.pump(*r, [&] { return r->stats().done >= 3; }));
    r->cancel();
    CHECK(t.pump(*r, [&] { return r->finished(); }));
    CHECK(r->phase() == Phase::Cancelled);
    size_t done = r->stats().done;
    CHECK(done >= 3 && done < 10);
    CHECK_EQ(count_of(t.gamelist(), "<scrap name=\"ScreenScraper\""), done);
    auto r2 = Run::create(t.cx, o);
    CHECK(t.run_to_end(*r2));
    CHECK(r2->phase() == Phase::Finished);
    CHECK_EQ(r2->stats().total, 10 - done);
    CHECK_EQ(count_of(t.gamelist(), "<scrap name=\"ScreenScraper\""), 10u);
  }

  // ---- network down: three failures in a row stop the run, nothing is written
  {
    Rig t;
    for (int i = 0; i < 6; ++i) t.rom("n" + std::to_string(i) + ".sfc");
    t.systems();
    put(t.fake + "/down", "");
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK(r->phase() == Phase::Aborted);
    CHECK(r->abort_reason() == Abort::NoNetwork);
    CHECK(!fs::exists(t.roms() + "/gamelist.xml"));
  }
  // ... and it one game on a healthy network is simply scraped
  {
    Rig t;
    t.rom("m0.sfc");
    t.known("m0");
    t.systems();
    Options o = t.opts();
    auto r = Run::create(t.cx, o);
    CHECK(t.run_to_end(*r));
    CHECK_EQ(r->stats().found, 1u);
  }

  // ---- bad login (403 on the account check): no game request is made
  {
    Rig t;
    t.rom("a.sfc");
    t.known("a");
    t.systems();
    put(t.fake + "/user.http", "403");
    put(t.fake + "/user.xml", "Erreur de login : Vérifier vos identifiants développeur !");
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK(r->phase() == Phase::Aborted);
    CHECK(r->abort_reason() == Abort::BadLogin);
    CHECK(t.requests().find("jeuInfos") == std::string::npos);
    CHECK(!fs::exists(t.roms() + "/gamelist.xml"));
  }

  // ---- daily quota already used up (from the account info), and 430 in the middle of a run
  {
    Rig t;
    t.rom("a.sfc");
    t.known("a");
    t.systems();
    put(t.fake + "/user.xml",
        "<Data><ssuser><id>u</id><requeststoday>20000</requeststoday><maxrequestspermin>20</maxrequestspermin><maxrequestsperday>20000</maxrequestsperday></ssuser></Data>");
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK(r->abort_reason() == Abort::Quota);
    CHECK(t.requests().find("jeuInfos") == std::string::npos);
  }
  {
    Rig t;
    for (int i = 0; i < 5; ++i) {
      std::string n = "q" + std::to_string(i);
      t.rom(n + ".sfc");
      t.known(n);
    }
    t.systems();
    Options o = t.opts();
    o.flush_every = 100;
    auto r = Run::create(t.cx, o);
    r->start();
    CHECK(t.pump(*r, [&] { return r->stats().done >= 2; }));
    put(t.fake + "/game.http", "430");  // the quota runs out now
    CHECK(t.pump(*r, [&] { return r->finished(); }));
    CHECK(r->phase() == Phase::Aborted);
    CHECK(r->abort_reason() == Abort::Quota);
    CHECK(count_of(t.gamelist(), "<scrap name=\"ScreenScraper\"") >= 2);  // what was scraped before is kept
  }

  // ---- the network drops in the middle: three failures in a row stop the run, what was done is kept, the next run resumes
  {
    Rig t;
    for (int i = 0; i < 8; ++i) {
      std::string n = "w" + std::to_string(i);
      t.rom(n + ".sfc");
      t.known(n);
    }
    t.systems();
    Options o = t.opts();
    o.skip_days = 15;
    o.flush_every = 100;
    auto r = Run::create(t.cx, o);
    r->start();
    CHECK(t.pump(*r, [&] { return r->stats().done >= 2; }));
    put(t.fake + "/down", "");
    CHECK(t.pump(*r, [&] { return r->finished(); }));
    CHECK(r->phase() == Phase::Aborted);
    CHECK(r->abort_reason() == Abort::NoNetwork);
    CHECK_EQ(count_of(t.gamelist(), "<scrap name=\"ScreenScraper\""), r->stats().found + r->stats().not_found);
    size_t kept = r->stats().found + r->stats().not_found;
    CHECK(kept >= 2 && kept < 8);
    CHECK(r->stats().errors >= 3);
    fs::remove(t.fake + "/down");
    auto r2 = Run::create(t.cx, o);
    CHECK(t.run_to_end(*r2));
    CHECK(r2->phase() == Phase::Finished);
    CHECK_EQ(r2->stats().total, 8 - kept);  // the games that failed are retried, the ones done are not
    CHECK_EQ(count_of(t.gamelist(), "<scrap name=\"ScreenScraper\""), 8u);
  }

  // ---- too many requests: waits and retries, then goes on
  {
    Rig t;
    t.rom("a.sfc");
    t.known("a");
    t.systems();
    put(t.fake + "/once429", "");
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK(r->phase() == Phase::Finished);
    CHECK_EQ(r->stats().found, 1u);
    CHECK_EQ(count_of(t.requests(), "jeuInfos"), 2u);
  }

  // ---- ScreenScraper answers with a text instead of a media file: the game is scraped, no media, no error
  {
    Rig t;
    t.rom("a.sfc");
    t.known("a");
    t.systems();
    put(t.fake + "/nomedia", "");
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK_EQ(r->stats().found, 1u);
    CHECK_EQ(r->stats().media, 0u);
    CHECK_EQ(r->stats().errors, 0u);
    CHECK_EQ(r->stats().warnings, 0u);
    CHECK(t.gamelist().find("<name>Game a (USA)</name>") != std::string::npos);
    CHECK(t.gamelist().find("<image>") == std::string::npos);
    CHECK(!fs::exists(t.roms() + "/images/a-image.png"));
    CHECK(!fs::exists(t.roms() + "/images/a-image.png.part"));
  }

  // ---- existing media is kept when the filter says "only missing"; names are kept when the option is off
  {
    Rig t;
    t.rom("a.sfc");
    t.known("a");
    put(t.roms() + "/images/a-image.png", "\x89PNG\r\n\x1a\nMINE");
    put(t.roms() + "/gamelist.xml",
        "<gameList>\n<game><path>./a.sfc</path><name>My Name</name><image>./images/a-image.png</image></game>\n</gameList>\n");
    t.systems();
    Options o = t.opts();
    o.filter = Options::Filter::MissingAny;
    o.overwrite_names = false;
    auto r = Run::create(t.cx, o);
    CHECK(t.run_to_end(*r));
    CHECK_EQ(r->stats().found, 1u);
    CHECK(slurp(t.roms() + "/images/a-image.png").find("MINE") != std::string::npos);
    CHECK(t.gamelist().find("<name>My Name</name>") != std::string::npos);
    CHECK(t.gamelist().find("Game a") == std::string::npos);
    CHECK(fs::exists(t.roms() + "/videos/a-video.mp4"));
  }

  // ---- dry-run (harness): nothing runs, the run ends as "no network" without writing
  {
    Rig t;
    t.rom("a.sfc");
    t.systems();
    t.cx.sv->cmd.dry_run = true;
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK(r->abort_reason() == Abort::NoNetwork);
    CHECK(!fs::exists(t.roms() + "/gamelist.xml"));
  }

  // ---- options come from ES's keys with ES's defaults
  {
    Rig t;
    Options o = Options::load(t.cx);
    CHECK_EQ(o.wish.image_src, std::string("ss"));
    CHECK_EQ(o.resize_px, 640);
    CHECK(o.wish.video && o.wish.manual && !o.wish.fanart);
    t.cx.es.set_bool(key::kVideos, false);
    t.cx.es.set_string(key::kImage, "mixrbv2");
    t.cx.es.set_int(key::kResizeW, 0);
    o = Options::load(t.cx);
    CHECK(!o.wish.video);
    CHECK_EQ(o.wish.image_src, std::string("mixrbv2"));
    CHECK_EQ(o.resize_px, 0);
  }
  return zt::done();
}
