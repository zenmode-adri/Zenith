// gamelist_edit_fields: batch edit of gamelist.xml (scraper). Untouched bytes stay identical, unknown children survive,
// <scrap> stamps are matched by scraper name, new games are appended, bad files are not touched.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "check.hpp"
#include "gamelist.hpp"
#include "util.hpp"

using namespace zf;
namespace fs = std::filesystem;

static std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream o;
  o << f.rdbuf();
  return o.str();
}
static void spit(const std::string& p, const std::string& t) {
  std::ofstream f(p, std::ios::binary);
  f << t;
}

static const char* kDoc =
    "<?xml version=\"1.0\"?>\n"
    "<gameList>\n"
    "\t<provider><System>snes</System></provider>\n"
    "\t<game id=\"77\" source=\"x\">\n"
    "\t\t<path>./a.sfc</path>\n"
    "\t\t<name>Old A</name>\n"
    "\t\t<custom attr=\"k\">keep me &amp; mine</custom>\n"
    "\t\t<favorite>true</favorite>\n"
    "\t\t<scrap name=\"TheGamesDB\" date=\"20250101T000000\"/>\n"
    "\t</game>\n"
    "\t<!-- a comment -->\n"
    "\t<folder><path>./sub</path><name>Sub</name></folder>\n"
    "\t<game>\n"
    "\t\t<path>./b.sfc</path>\n"
    "\t\t<name>B</name>\n"
    "\t</game>\n"
    "\t<game>\n"
    "\t\t<path>./c.sfc</path>\n"
    "\t\t<name>C</name>\n"
    "\t\t<desc>untouched</desc>\n"
    "\t</game>\n"
    "</gameList>\n";

int main() {
  char dir[] = "/tmp/zf-gled-XXXXXX";
  std::string root = ::mkdtemp(dir);
  std::string xml = root + "/gamelist.xml";
  std::string err;

  // --- edit two games, create one, leave one alone
  spit(xml, kDoc);
  std::vector<GameFieldEdit> ed(3);
  ed[0].rom = root + "/a.sfc";
  ed[0].fields = {{"name", "Game <A> & Co"}, {"desc", "Line 1\nLine 2"}, {"rating", "0.800000"}};
  ed[0].scraper = "ScreenScraper";
  ed[0].scrape_date = "20261025T120000";
  ed[1].rom = root + "/c.sfc";
  ed[1].fields = {{"name", "C new"}};
  ed[2].rom = root + "/z.sfc";
  ed[2].fields = {{"name", "Zed"}, {"image", "./images/z-image.png"}};
  ed[2].scraper = "ScreenScraper";
  ed[2].scrape_date = "20261025T120001";
  GamelistBatchResult r;
  CHECK(gamelist_edit_fields(xml, root, ed, err, &r));
  CHECK_EQ(r.edited, 2u);
  CHECK_EQ(r.created, 1u);
  CHECK_EQ(r.failed, 0u);
  std::string out = slurp(xml);
  // the untouched parts are byte identical
  CHECK(out.find("\t<provider><System>snes</System></provider>\n") != std::string::npos);
  CHECK(out.find("\t<!-- a comment -->\n\t<folder><path>./sub</path><name>Sub</name></folder>\n") != std::string::npos);
  CHECK(out.find("\t<game>\n\t\t<path>./b.sfc</path>\n\t\t<name>B</name>\n\t</game>\n") != std::string::npos);
  // unknown child + attribute of <game id="77"> survive; the other scraper's stamp stays; ours is added
  CHECK(out.find("<game id=\"77\" source=\"x\">") != std::string::npos);
  CHECK(out.find("<custom attr=\"k\">keep me &amp; mine</custom>") != std::string::npos);
  CHECK(out.find("<scrap name=\"TheGamesDB\" date=\"20250101T000000\"/>") != std::string::npos);
  CHECK(out.find("<scrap name=\"ScreenScraper\" date=\"20261025T120000\"/>") != std::string::npos);
  CHECK(out.find("<favorite>true</favorite>") != std::string::npos);
  CHECK(out.find("<name>Game &lt;A&gt; &amp; Co</name>") != std::string::npos);
  CHECK(out.find("<desc>untouched</desc>") != std::string::npos);
  CHECK(out.find("<desc>Line 1\nLine 2</desc>") != std::string::npos);
  CHECK(out.find("<name>C new</name>") != std::string::npos);
  CHECK(out.find("<path>./z.sfc</path>") != std::string::npos);
  CHECK(out.find("<image>./images/z-image.png</image>") != std::string::npos);
  CHECK(out.rfind("</game>\n</gameList>\n") == out.size() - std::string("</game>\n</gameList>\n").size());
  // the result is still readable by the streaming reader
  size_t n = 0;
  std::string name_a;
  auto cb = [&](const GameEntry& e) {
    ++n;
    if (e.path == "./a.sfc") name_a = std::string(e.name);
  };
  GamelistInfo gi = gamelist_scan_file(xml, EntryFn::of(cb));
  CHECK(gi.ok);
  CHECK_EQ(n, 4u);
  CHECK_EQ(name_a, std::string("Game <A> & Co"));

  // --- the same scrape again: the stamp of our scraper is replaced, not duplicated
  ed.resize(1);
  ed[0].scrape_date = "20261026T080000";
  CHECK(gamelist_edit_fields(xml, root, ed, err, &r));
  out = slurp(xml);
  CHECK(out.find("20261025T120000") == std::string::npos);
  size_t cnt = 0;
  for (size_t p = 0; (p = out.find("name=\"ScreenScraper\"", p)) != std::string::npos; ++p) ++cnt;
  CHECK_EQ(cnt, 2u);  // a.sfc + z.sfc
  CHECK(out.find("<scrap name=\"ScreenScraper\" date=\"20261026T080000\"/>") != std::string::npos);

  // --- idempotent: an edit that changes nothing leaves the file byte identical
  std::string before = slurp(xml);
  CHECK(gamelist_edit_fields(xml, root, ed, err, &r));
  CHECK(slurp(xml) == before);

  // --- the last edit of the same ROM wins
  ed.assign(2, GameFieldEdit());
  ed[0].rom = ed[1].rom = root + "/b.sfc";
  ed[0].fields = {{"name", "first"}};
  ed[1].fields = {{"name", "second"}};
  CHECK(gamelist_edit_fields(xml, root, ed, err, &r));
  out = slurp(xml);
  CHECK(out.find("<name>second</name>") != std::string::npos);
  CHECK(out.find("first") == std::string::npos);

  // --- no gamelist yet: a document is created
  std::string xml2 = root + "/new/gamelist.xml";
  fs::create_directories(root + "/new");
  ed.assign(1, GameFieldEdit());
  ed[0].rom = root + "/new/q.gb";
  ed[0].fields = {{"name", "Q"}};
  ed[0].scraper = "ScreenScraper";
  ed[0].scrape_date = "20261025T000000";
  CHECK(gamelist_edit_fields(xml2, root + "/new", ed, err, &r));
  CHECK_EQ(r.created, 1u);
  out = slurp(xml2);
  CHECK(out.find("<gameList>") != std::string::npos);
  CHECK(out.find("<path>./q.gb</path>") != std::string::npos);
  CHECK(out.find("<scrap name=\"ScreenScraper\" date=\"20261025T000000\"/>") != std::string::npos);

  // --- self-closing root and blank file
  spit(xml2, "<?xml version=\"1.0\"?>\n<gameList/>\n");
  CHECK(gamelist_edit_fields(xml2, root + "/new", ed, err, &r));
  CHECK(slurp(xml2).find("<path>./q.gb</path>") != std::string::npos);
  spit(xml2, "  \n");
  CHECK(gamelist_edit_fields(xml2, root + "/new", ed, err, &r));
  CHECK(slurp(xml2).find("<path>./q.gb</path>") != std::string::npos);

  // --- malformed file is not touched
  spit(xml2, "<gameList><game><path>./x</path>");
  std::string bad = slurp(xml2);
  CHECK(!gamelist_edit_fields(xml2, root + "/new", ed, err, &r));
  CHECK(slurp(xml2) == bad);
  CHECK(!err.empty());
  CHECK(!fs::exists(xml2 + ".zfront-tmp"));

  // --- a stamp-only edit (not found in the file): the game is created with just the stamp
  spit(xml, kDoc);
  ed.assign(1, GameFieldEdit());
  ed[0].rom = root + "/nope.sfc";
  ed[0].scraper = "ScreenScraper";
  ed[0].scrape_date = "20261025T010203";
  CHECK(gamelist_edit_fields(xml, root, ed, err, &r));
  out = slurp(xml);
  CHECK(out.find("<path>./nope.sfc</path>") != std::string::npos);
  CHECK(out.find("date=\"20261025T010203\"") != std::string::npos);

  std::error_code ec;
  fs::remove_all(root, ec);
  return zt::done();
}
