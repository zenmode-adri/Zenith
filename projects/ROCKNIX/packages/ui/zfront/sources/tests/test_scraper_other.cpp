// IGDB and ArcadeDB next to ScreenScraper: the source chosen by ES's `Scraper` key, which systems each one serves, the requests (URL,
// headers, body) and the answer mapping from saved sample JSON (never the network), and the engine end to end with a fake `curl`
// (the same trick as test_scraper_run): token cached once per run, secrets only in curl's config file, wrong credentials stop the run.
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "scraper_adb.hpp"
#include "scraper_fake.hpp"
#include "oscontract.hpp"
#include "scraper_hash.hpp"
#include "scraper_igdb.hpp"
#include "scraper_run.hpp"
#include "scraper_src.hpp"
#include "systems.hpp"

using namespace zf;
using namespace zf::scraper;
using namespace scrfake;
namespace fs = std::filesystem;

// ---- saved samples (shapes as the real servers send them) --------------------------------------------------------------------
static const char* kTokenJson = R"J({"access_token":"TOKENVALUE123","expires_in":5587808,"token_type":"bearer"})J";

static const char* kIgdbJson = R"J([
  {"id":99},
  {"id":1074,"aggregated_rating":93.5,
   "artworks":[{"id":5,"height":1080,"image_id":"ar1abc","url":"//images.igdb.com/igdb/image/upload/t_thumb/ar1abc.jpg","width":1920},{"id":6,"image_id":"ar2def"}],
   "cover":{"id":88,"image_id":"co1xyz","url":"//images.igdb.com/igdb/image/upload/t_thumb/co1xyz.jpg"},
   "involved_companies":[{"id":1,"company":{"id":1,"name":"Nintendo EAD"},"developer":true,"publisher":false},
                         {"id":2,"company":{"id":2,"name":"Nintendo"},"developer":false,"publisher":true}],
   "name":"Super Mario World",
   "screenshots":[{"id":1,"image_id":"sc1aaa"},{"id":2,"image_id":"sc2bbb"}],
   "summary":"Mario and Luigi run and jump through Dinosaur Land.",
   "url":"https://www.igdb.com/games/super-mario-world"}
])J";

static const char* kAdbJson = R"J({"release":4,"result":[
  {"game_name":"pacman","title":"Pac-Man (Midway)","short_title":"Pac-Man","description":"Pac-Man (Midway)",
   "history":"Pac-Man is a maze game.","year":"1980","manufacturer":"Namco","genre":"Maze","players":1,
   "url_image_ingame":"http://adb.arcadeitalia.net/media/mame.current/ingames/pacman.png",
   "url_image_title":"http://adb.arcadeitalia.net/media/mame.current/titles/pacman.png",
   "url_image_marquee":"http://adb.arcadeitalia.net/media/mame.current/marquees/pacman.png",
   "url_image_flyer":"http://adb.arcadeitalia.net/media/mame.current/flyers/pacman.png",
   "url_video_shortplay":"http://adb.arcadeitalia.net/media/mame.current/videos/pacman.mp4"}
]})J";

// fake curl that also logs the headers and the POST body of each request (one tab separated line per request)
static const char* kFake2 = R"SH(#!/bin/bash
D="$ZF_FAKE_DIR"
[ "$1" = "-K" ] || exit 2
[ -f "$D/slow" ] && exec sleep 30
cfg="$2"
url=""; out=""; hdrs=""; data=""; pr=""
emit() {
  [ -z "$url" ] && return
  printf '%s\t%s\t%s\t%s\n' "$url" "$hdrs" "$data" "$pr" >> "$D/requests.log"
  if [ -f "$D/once28" ]; then rm -f "$D/once28"; printf '@@28|000|0||\n'; return; fi
  if [ -f "$D/code23" ]; then printf '@@23|000|0||\n'; return; fi
  code=200; ctype="application/json"
  case "$url" in
    *ssuserInfos.php*) cat "$D/user.xml" > "$out"; ctype="text/xml" ;;
    *jeuInfos.php*) cat "$D/game.xml" > "$out"; ctype="text/xml" ;;
    */media/*|*images.igdb.com*)
      printf '\x89PNG\r\n\x1a\nFAKEPNG' > "$out"; ctype="image/png" ;;
    *id.twitch.tv/oauth2/token*)
      if [ -f "$D/tok.http" ]; then code=$(cat "$D/tok.http"); echo '{"status":400,"message":"invalid client"}' > "$out"
      else cat "$D/token.json" > "$out"; fi ;;
    *api.igdb.com/v4/games*)
      cat "$D/igdb.json" > "$out" ;;
    *adb.arcadeitalia.net*)
      cat "$D/adb.json" > "$out" ;;
    *) code=404; echo "x" > "$out" ;;
  esac
  size=$(stat -c %s "$out" 2>/dev/null || echo 0)
  printf '@@0|%s|%s||%s\n' "$code" "$size" "$ctype"
}
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    next) emit; url=""; out=""; hdrs=""; data=""; pr="" ;;
    # as the real curl: a write-out that starts with @ names a file to read the format from (found on the device, 9-nov: every
    # scrape ended in "Network error")
    'write-out = "@'*) echo "curl: option -K: error encountered when reading a file" >&2; exit 26 ;;
    'proto = "'*|'proto-redir = "'*) pr="$pr[$line]" ;;
    'url = "'*) url="${line#url = \"}"; url="${url%\"}" ;;
    'output = "'*) out="${line#output = \"}"; out="${out%\"}" ;;
    'header = "'*) h="${line#header = \"}"; h="${h%\"}"; hdrs="$hdrs[$h]" ;;
    'data-raw = "'*) data="${line#data-raw = \"}"; data="${data%\"}" ;;
  esac
done < "$cfg"
emit
exit 0
)SH";

static bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
static bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
static const ss::Pick* pick(const ss::Parsed& p, const char* kind) {
  for (const ss::Pick& k : p.picks)
    if (k.kind == kind) return &k;
  return nullptr;
}

// one system `snes` whose platform and extensions the test chooses, ROMs in Rig::roms()
static void one_system(Rig& t, const char* ext, const char* platform) {
  std::string xml = std::string("<systemList><system><name>snes</name><fullname>Test</fullname><path>/storage/roms/snes</path><extension>") + ext +
                    "</extension><platform>" + platform + "</platform><command>r</command></system></systemList>";
  std::string err;
  CHECK(load_systems_text(xml, t.root + "/roms", true, t.cx.systems, err));
}

static void use_fake2(Rig& t) {
  put(t.fake + "/curl", kFake2);
  ::chmod((t.fake + "/curl").c_str(), 0755);
  put(t.fake + "/token.json", kTokenJson);
  put(t.fake + "/igdb.json", kIgdbJson);
  put(t.fake + "/adb.json", kAdbJson);
}

int main() {
  // ---- the source is chosen by ES's `Scraper` key; ScreenScraper stays the default
  {
    EsSettings es = EsSettings::from_text("<config/>");
    CHECK(source_of(es) == Source::ScreenScraper);
    es.set_string("Scraper", "IGDB");
    CHECK(source_of(es) == Source::IGDB);
    es.set_string("Scraper", "ArcadeDB");
    CHECK(source_of(es) == Source::ArcadeDB);
    es.set_string("Scraper", "ScreenScraper");
    CHECK(source_of(es) == Source::ScreenScraper);
    es.set_string("Scraper", "TheGamesDB");  // not in this ES build
    CHECK(source_of(es) == Source::ScreenScraper);
    es.set_string("Scraper", "");
    CHECK(source_of(es) == Source::ScreenScraper);
    CHECK_EQ(std::string(kKeySource), std::string("Scraper"));
    CHECK_EQ(std::string(source_name(Source::IGDB)), std::string("IGDB"));
    CHECK_EQ(std::string(source_name(Source::ArcadeDB)), std::string("ArcadeDB"));
    CHECK_EQ(std::string(source_name(Source::ScreenScraper)), std::string("ScreenScraper"));
    std::vector<Source> all = sources();
    CHECK_EQ(all.size(), 3u);
    CHECK(all[0] == Source::ScreenScraper && all[1] == Source::IGDB && all[2] == Source::ArcadeDB);
    CHECK(available(Source::IGDB) && available(Source::ArcadeDB));
  }

  // ---- options carry the source, the IGDB credentials and ScrapeShortTitle (ES defaults: none, false)
  {
    Ctx cx;
    cx.es = EsSettings::from_text("<config/>");
    Options o = Options::load(cx);
    CHECK(o.source == Source::ScreenScraper);
    CHECK(o.igdb_id.empty() && o.igdb_secret.empty() && !o.short_title);
    cx.es.set_string("Scraper", "IGDB");
    cx.es.set_string("IGDBClientID", "cid");
    cx.es.set_string("IGDBSecret", "sec");
    cx.es.set_bool("ScrapeShortTitle", true);
    o = Options::load(cx);
    CHECK(o.source == Source::IGDB);
    CHECK_EQ(o.igdb_id, std::string("cid"));
    CHECK_EQ(o.igdb_secret, std::string("sec"));
    CHECK(o.short_title);
  }

  // ---- which systems each source serves (as ES's isSupportedPlatform)
  CHECK(igdb::platform_ids("snes") == std::vector<std::string>{"19"});
  CHECK(igdb::platform_ids("pc windows") == (std::vector<std::string>{"13", "6"}));
  CHECK(igdb::platform_ids("moonlight") == (std::vector<std::string>{"13", "6"}));
  CHECK(igdb::platform_ids("arcade atomiswave") == std::vector<std::string>{"52"});
  CHECK(igdb::platform_ids("ebook").empty());
  CHECK(igdb::platform_supported("gba") && igdb::platform_supported("psx") && !igdb::platform_supported("ports"));
  CHECK(adb::platform_supported("arcade", "fbneo"));
  CHECK(adb::platform_supported("neogeo", "neogeo"));
  CHECK(adb::platform_supported("lcdgames", "lcdgames"));
  CHECK(!adb::platform_supported("snes", "snes"));
  CHECK(!adb::platform_supported("teknoparrot", "teknoparrot"));
  CHECK(!adb::platform_supported("arcade", "teknoparrot"));
  CHECK(platform_supported(Source::ScreenScraper, "snes", "snes") && !platform_supported(Source::ScreenScraper, "ports", "ports"));
  {
    Rig t;
    Ctx& cx = t.cx;
    for (const char* d : {"snes/a.sfc", "fbneo/a.zip", "teknoparrot/a.txt", "ports/a.sh"}) put(t.root + "/roms/" + d, "x");
    std::string xml =
        "<systemList>"
        "<system><name>snes</name><fullname>Super Nintendo</fullname><path>/storage/roms/snes</path><extension>.sfc</extension><platform>snes</platform><command>r</command></system>"
        "<system><name>fbneo</name><fullname>FBNeo</fullname><path>/storage/roms/fbneo</path><extension>.zip</extension><platform>arcade</platform><command>r</command></system>"
        "<system><name>teknoparrot</name><fullname>TeknoParrot</fullname><path>/storage/roms/teknoparrot</path><extension>.txt</extension><platform>teknoparrot</platform><command>r</command></system>"
        "<system><name>ports</name><fullname>Ports</fullname><path>/storage/roms/ports</path><extension>.sh</extension><platform>ports</platform><command>r</command></system>"
        "</systemList>";
    std::string err;
    CHECK(load_systems_text(xml, t.root + "/roms", true, cx.systems, err, true));
    CHECK_EQ(cx.systems.size(), 4u);
    auto names = [&cx](const std::vector<size_t>& v) {  // the list may be sorted: compare by name
      std::vector<std::string> n;
      for (size_t i : v) n.push_back(cx.systems[i].name);
      std::sort(n.begin(), n.end());
      return n;
    };
    auto at = [&cx](const char* name) {
      for (size_t i = 0; i < cx.systems.size(); ++i)
        if (cx.systems[i].name == name) return i;
      return size_t(99);
    };
    std::vector<size_t> ss_def = supported_systems(cx);  // no key: ScreenScraper, as before
    CHECK(ss_def == supported_systems(cx, Source::ScreenScraper));
    CHECK(has(names(ss_def), "snes") && !has(names(ss_def), "ports"));
    CHECK(names(supported_systems(cx, Source::IGDB)) == (std::vector<std::string>{"fbneo", "snes", "teknoparrot"}));
    CHECK(names(supported_systems(cx, Source::ArcadeDB)) == std::vector<std::string>{"fbneo"});
    cx.es.set_string("Scraper", "ArcadeDB");
    CHECK(names(supported_systems(cx)) == std::vector<std::string>{"fbneo"});
    CHECK(system_supported(cx, at("fbneo")) && !system_supported(cx, at("snes")) && !system_supported(cx, at("teknoparrot")) && !system_supported(cx, 99));
    cx.es.set_string("Scraper", "IGDB");
    CHECK(names(supported_systems(cx)) == (std::vector<std::string>{"fbneo", "snes", "teknoparrot"}));
    CHECK(system_supported(cx, at("snes")) && !system_supported(cx, at("ports")));
  }

  // ---- the name every source is searched with: ES removeParenthesis(stem)
  CHECK_EQ(search_stem("Super Mario World (USA) [!].sfc"), std::string("Super Mario World"));
  CHECK_EQ(search_stem("/roms/megadrive/Sonic The Hedgehog (Europe).md"), std::string("Sonic The Hedgehog"));
  CHECK_EQ(search_stem("pacman.zip"), std::string("pacman"));
  CHECK_EQ(search_stem("Game (a (b) c) end.gb"), std::string("Game  end"));
  CHECK_EQ(adb::game_name("Street Fighter II (World 910522).zip"), std::string("StreetFighterII"));
  CHECK_EQ(adb::game_name("Pac-Man v1.0.zip"), std::string("Pac-Manv10"));

  // ---- IGDB requests
  {
    HttpRequest t = igdb::token_request("my id", "s&e=cret");
    CHECK_EQ(t.url, std::string("https://id.twitch.tv/oauth2/token"));
    CHECK(t.post);
    CHECK_EQ(t.body, std::string("client_id=my%20id&client_secret=s%26e%3Dcret&grant_type=client_credentials"));
    CHECK(t.headers == std::vector<std::string>{"Content-Type: application/x-www-form-urlencoded"});

    HttpRequest g = igdb::game_request("CID", "TOK", "Super Mario World", "19");
    CHECK_EQ(g.url, std::string("https://api.igdb.com/v4/games"));
    CHECK(g.post);
    CHECK(has(g.headers, "Client-ID: CID"));
    CHECK(has(g.headers, "Authorization: Bearer TOK"));
    CHECK(has(g.headers, "Accept: application/json"));
    CHECK(g.body.find("fields id, name, platforms.name, genres.name,") == 0);
    CHECK(contains(g.body, "cover.*, screenshots.*, artworks.*"));
    CHECK(contains(g.body, "involved_companies.company.name, involved_companies.developer, involved_companies.publisher ;"));
    CHECK(contains(g.body, " search \"Super Mario World\"; limit 10; where release_dates.platform = 19;"));
    HttpRequest n = igdb::game_request("CID", "TOK", "He said \"hi\"\\", "");
    CHECK(contains(n.body, "search \"He said \\\"hi\\\"\\\\\";"));
    CHECK(!contains(n.body, "where"));
    // the secrets are never in the URL
    CHECK(!contains(g.url, "TOK") && !contains(g.url, "CID") && !contains(t.url, "cret"));
  }

  // ---- IGDB answers
  {
    std::string tok;
    long exp = 7;
    CHECK(igdb::parse_token(kTokenJson, tok, exp));
    CHECK_EQ(tok, std::string("TOKENVALUE123"));
    CHECK_EQ(exp, 5587808L);
    CHECK_EQ(igdb::token_ttl_s(exp), 1800L);  // ES keeps it 30 minutes
    CHECK_EQ(igdb::token_ttl_s(60), 60L);
    CHECK_EQ(igdb::token_ttl_s(0), 1800L);
    CHECK(!igdb::parse_token("{\"status\":400,\"message\":\"invalid client\"}", tok, exp) && tok.empty());
    CHECK(!igdb::parse_token("not json", tok, exp));

    ss::Wish w;
    ss::Parsed p;
    CHECK(igdb::parse_games(kIgdbJson, w, p));  // the first entry has no name and is skipped
    CHECK_EQ(p.name, std::string("Super Mario World"));
    CHECK_EQ(p.desc, std::string("Mario and Luigi run and jump through Dinosaur Land."));
    CHECK_EQ(p.developer, std::string("Nintendo EAD"));
    CHECK_EQ(p.publisher, std::string("Nintendo"));
    CHECK(p.rating.empty() && p.genre.empty() && p.date.empty());  // ES maps none of these for IGDB (rating -1)
    const ss::Pick* th = pick(p, "thumb");
    const ss::Pick* im = pick(p, "image");
    CHECK(th && im);
    if (th) CHECK_EQ(th->url, std::string("https://images.igdb.com/igdb/image/upload/t_original/co1xyz.jpg"));
    if (im) CHECK_EQ(im->url, std::string("https://images.igdb.com/igdb/image/upload/t_screenshot_huge/sc1aaa.jpg"));
    CHECK(!pick(p, "fanart"));
    w.fanart = true;
    ss::Parsed p2;
    CHECK(igdb::parse_games(kIgdbJson, w, p2));
    const ss::Pick* fa = pick(p2, "fanart");
    CHECK(fa);
    if (fa) CHECK_EQ(fa->url, std::string("https://images.igdb.com/igdb/image/upload/t_1080p/ar1abc.jpg"));
    CHECK_EQ(p2.picks.size(), 3u);
    ss::Parsed none;
    CHECK(!igdb::parse_games("[]", w, none));
    CHECK(!igdb::parse_games("not json", w, none));
    CHECK(!igdb::parse_games("{\"name\":\"x\"}", w, none));
    CHECK(!igdb::parse_games("[{\"title\":\"Syntax Error\",\"status\":400}]", w, none));  // an IGDB error body
  }

  // ---- ArcadeDB request and answers
  {
    HttpRequest r = adb::game_request("Pac-Man (World).zip");
    CHECK(!r.post && r.body.empty() && r.headers.empty());
    CHECK_EQ(r.url, std::string("https://adb.arcadeitalia.net/service_scraper.php?ajax=query_mame&lang=en&use_parent=1&game_name=Pac-Man"));
    CHECK(contains(adb::game_request("Space Invaders Part II.zip").url, "&game_name=SpaceInvadersPartII"));

    ss::Wish w;  // ES defaults: image ss, thumb box-2D, logo wheel, video on
    ss::Parsed p;
    CHECK(adb::parse_games(kAdbJson, w, false, p));
    CHECK_EQ(p.name, std::string("Pac-Man (Midway)"));
    CHECK_EQ(p.desc, std::string("Pac-Man is a maze game."));
    CHECK_EQ(p.date, std::string("19800101T000000"));
    CHECK_EQ(p.developer, std::string("Namco"));
    CHECK_EQ(p.publisher, std::string("Namco"));
    CHECK_EQ(p.genre, std::string("Maze"));
    CHECK_EQ(p.players, std::string("1"));
    CHECK(p.rating.empty());
    CHECK_EQ(p.picks.size(), 4u);
    CHECK(pick(p, "image") && pick(p, "image")->url.find("/ingames/pacman.png") != std::string::npos);
    CHECK(pick(p, "thumb") && pick(p, "thumb")->url.find("/flyers/pacman.png") != std::string::npos);
    CHECK(pick(p, "marquee") && pick(p, "marquee")->url.find("/marquees/pacman.png") != std::string::npos);
    CHECK(pick(p, "video") && pick(p, "video")->url.find("/videos/pacman.mp4") != std::string::npos);
    CHECK(!pick(p, "titleshot"));
    ss::Parsed ps;
    CHECK(adb::parse_games(kAdbJson, w, true, ps));  // ScrapeShortTitle
    CHECK_EQ(ps.name, std::string("Pac-Man"));
    w.titleshot = true;
    w.video = false;
    w.thumb_src = "ss";  // same as the image: no thumbnail (ES)
    w.logo_src = "";
    ss::Parsed pw;
    CHECK(adb::parse_games(kAdbJson, w, false, pw));
    CHECK_EQ(pw.picks.size(), 2u);
    CHECK(pick(pw, "titleshot") && pick(pw, "titleshot")->url.find("/titles/pacman.png") != std::string::npos);
    CHECK(!pick(pw, "thumb") && !pick(pw, "marquee") && !pick(pw, "video"));
    w.image_src = "sstitle";
    ss::Parsed pt;
    CHECK(adb::parse_games(kAdbJson, w, false, pt));
    CHECK(pick(pt, "image") && pick(pt, "image")->url.find("/titles/pacman.png") != std::string::npos);
    ss::Parsed none;
    CHECK(!adb::parse_games("{\"release\":3,\"result\":[{\"title\":\"x\"}]}", w, false, none));  // old release
    CHECK(!adb::parse_games("{\"release\":4,\"result\":[]}", w, false, none));
    CHECK(!adb::parse_games("{\"release\":4}", w, false, none));
    CHECK(!adb::parse_games("<html>", w, false, none));
    CHECK(!adb::parse_games("{\"release\":4,\"result\":[{\"short_title\":\"x\"}]}", w, false, none));  // no title: ES throws and skips it
  }

  // ---- engine, ArcadeDB: no account, searched by name, media downloaded, stamp "ArcadeDB"
  {
    Rig t;
    use_fake2(t);
    t.cx.es.set_string("Scraper", "ArcadeDB");
    t.rom("pacman.zip");
    one_system(t, ".zip", "arcade");
    auto r = Run::create(t.cx, t.opts());
    CHECK(r->source() == Source::ArcadeDB);
    CHECK(t.run_to_end(*r));
    CHECK(r->phase() == Phase::Finished);
    CHECK_EQ(r->stats().found, 1u);
    std::string req = t.requests();
    CHECK(contains(req, "service_scraper.php?ajax=query_mame&lang=en&use_parent=1&game_name=pacman\t"));
    CHECK(!contains(req, "twitch") && !contains(req, "screenscraper.fr") && !contains(req, "igdb"));
    // ArcadeDB answers over https (its http address only redirects there, 9-nov) and gives media as http:// links that also redirect: an
    // http URL may move to https (with "=http" alone curl refused the redirect: exit 1, "Network error" on the device), never file:// or ftp://
    CHECK(contains(req, "https://adb.arcadeitalia.net/service_scraper.php"));
    CHECK(contains(req, "[proto = \"=http,https\"][proto-redir = \"=http,https\"]"));
    std::string gl = t.gamelist();
    CHECK(contains(gl, "<name>Pac-Man (Midway)</name>"));
    CHECK(contains(gl, "<releasedate>19800101T000000</releasedate>"));
    CHECK(contains(gl, "<developer>Namco</developer>"));
    CHECK(contains(gl, "<scrap name=\"ArcadeDB\""));
    CHECK(!contains(gl, "ScreenScraper"));
    CHECK(fs::exists(t.roms() + "/images/pacman-image.png"));
    CHECK(fs::exists(t.roms() + "/images/pacman-thumb.png"));
    CHECK(fs::exists(t.roms() + "/images/pacman-marquee.png"));
    // the "missing media" filter only counts what ArcadeDB can give: with image, thumb, marquee and video there nothing is missing
    Options o = t.opts();
    o.filter = Options::Filter::MissingAny;
    o.wish.video = false;
    auto r2 = Run::create(t.cx, o);
    CHECK(t.run_to_end(*r2));
    CHECK_EQ(r2->stats().total, 0u);
  }

  // ---- engine, IGDB: token once per run, POST with Client-ID and Bearer, secrets only in the curl config file
  {
    Rig t;
    use_fake2(t);
    t.cx.es.set_string("Scraper", "IGDB");
    t.cx.es.set_string("IGDBClientID", "CLIENTIDVALUE");
    t.cx.es.set_string("IGDBSecret", "IGDBSECRETVALUE");
    t.rom("Super Mario World (USA).sfc");
    t.rom("Another Game (Europe).sfc");
    one_system(t, ".sfc", "snes");
    auto r = Run::create(t.cx, t.opts());
    CHECK(r->source() == Source::IGDB);
    CHECK(t.run_to_end(*r));
    CHECK(r->phase() == Phase::Finished);
    CHECK_EQ(r->stats().found, 2u);
    std::string req = t.requests();
    CHECK_EQ(count_of(req, "https://id.twitch.tv/oauth2/token\t"), 1u);  // the token is kept for the whole run
    CHECK(contains(req, "\t[Content-Type: application/x-www-form-urlencoded]\tclient_id=CLIENTIDVALUE&client_secret=IGDBSECRETVALUE&grant_type=client_credentials"));
    CHECK_EQ(count_of(req, "https://api.igdb.com/v4/games\t"), 2u);
    CHECK(contains(req, "[Client-ID: CLIENTIDVALUE][Authorization: Bearer TOKENVALUE123][Content-Type: text/plain][Accept: application/json]"));
    CHECK(contains(req, "search \\\"Super Mario World\\\"; limit 10; where release_dates.platform = 19;"));
    CHECK(contains(req, "search \\\"Another Game\\\"; limit 10; where release_dates.platform = 19;"));
    CHECK(!contains(req, "screenscraper.fr"));
    CHECK(contains(req, "[proto = \"=https\"][proto-redir = \"=https\"]"));  // https only, also across redirects
    CHECK(!contains(req, "=http\""));
    for (const std::string& c : t.cx.sv->cmd.log) CHECK(!contains(c, "IGDBSECRETVALUE") && !contains(c, "TOKENVALUE123") && !contains(c, "CLIENTIDVALUE"));
    std::string gl = t.gamelist();
    CHECK(contains(gl, "<name>Super Mario World</name>"));
    CHECK(contains(gl, "<developer>Nintendo EAD</developer>"));
    CHECK(contains(gl, "<publisher>Nintendo</publisher>"));
    CHECK(contains(gl, "<desc>Mario and Luigi run and jump through Dinosaur Land.</desc>"));
    CHECK(contains(gl, "<scrap name=\"IGDB\""));
    CHECK(!contains(gl, "ScreenScraper") && !contains(gl, "SECRETVALUE") && !contains(gl, "TOKENVALUE"));
    CHECK(fs::exists(t.roms() + "/images/Super Mario World (USA)-image.png"));  // the bytes decide the extension, not the .jpg of the URL
    CHECK(fs::exists(t.roms() + "/images/Super Mario World (USA)-thumb.png"));
    CHECK(!fs::exists(t.roms() + "/images/Super Mario World (USA)-fanart.png"));  // ScrapeFanart is off
  }

  // ---- engine, IGDB: wrong or missing credentials end the run at once
  {
    Rig t;
    use_fake2(t);
    t.cx.es.set_string("Scraper", "IGDB");
    t.cx.es.set_string("IGDBClientID", "CLIENTIDVALUE");
    t.cx.es.set_string("IGDBSecret", "WRONGSECRET");
    t.rom("a.sfc");
    one_system(t, ".sfc", "snes");
    put(t.fake + "/tok.http", "400");
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK(r->phase() == Phase::Aborted);
    CHECK(r->abort_reason() == Abort::InvalidCredentials);
    CHECK(!contains(t.requests(), "api.igdb.com"));
    CHECK_EQ(std::string(describe(Abort::InvalidCredentials)), std::string(tr("INVALID CREDENTIALS")));

    Rig u;
    use_fake2(u);
    u.cx.es.set_string("Scraper", "IGDB");  // no client id / secret at all
    u.rom("a.sfc");
    one_system(u, ".sfc", "snes");
    auto r2 = Run::create(u.cx, u.opts());
    CHECK(u.run_to_end(*r2));
    CHECK(r2->abort_reason() == Abort::NoCredentials);  // not "invalid": nothing was typed yet (seen on the device, 9-nov)
    CHECK(std::string(describe(Abort::NoCredentials)) != std::string(describe(Abort::InvalidCredentials)));
    CHECK(u.requests().empty());  // nothing is sent without credentials
  }

  // ---- finding 13: the CRC table is built once, thread-safely (8 threads start together on a cold table)
  {
    std::vector<std::thread> th;
    std::vector<uint32_t> got(8, 0);
    for (size_t i = 0; i < got.size(); ++i) th.emplace_back([&got, i] { got[i] = crc32_update(0, "123456789", 9); });
    for (std::thread& x : th) x.join();
    for (uint32_t g : got) CHECK_EQ(g, 0xCBF43926u);
  }

  // ---- answers that are not answers, IGDB size tokens
  CHECK(igdb::valid_answer("[]") && igdb::valid_answer(kIgdbJson) && !igdb::valid_answer("{}") && !igdb::valid_answer("<html>") && !igdb::valid_answer(""));
  CHECK(adb::valid_answer(kAdbJson) && !adb::valid_answer("{\"release\":3,\"result\":[]}") && !adb::valid_answer("<html>x</html>") && !adb::valid_answer("{\"release\":4}"));
  {
    ss::Wish w;
    ss::Parsed p;
    CHECK(igdb::parse_games(kIgdbJson, w, p, 640));  // ScraperResizeWidth 640: no original-size covers
    CHECK(pick(p, "thumb") && pick(p, "thumb")->url.find("/t_720p/co1xyz.jpg") != std::string::npos);
    CHECK(pick(p, "image") && pick(p, "image")->url.find("/t_screenshot_big/sc1aaa.jpg") != std::string::npos);
    ss::Parsed q;
    CHECK(igdb::parse_games(kIgdbJson, w, q, 200));
    CHECK(pick(q, "thumb") && pick(q, "thumb")->url.find("/t_cover_big/") != std::string::npos);
    CHECK(pick(q, "image") && pick(q, "image")->url.find("/t_screenshot_med/") != std::string::npos);
    ss::Parsed z;
    CHECK(igdb::parse_games(kIgdbJson, w, z, 0));  // full size: ES's tokens
    CHECK(pick(z, "thumb") && pick(z, "thumb")->url.find("/t_original/") != std::string::npos);
  }

  // ---- finding 1: B kills the curl in flight (a 30 s transfer must not hold the run), and the run holds oscontract::Busy meanwhile
  {
    Rig t;
    use_fake2(t);
    t.cx.es.set_string("Scraper", "ArcadeDB");
    t.rom("pacman.zip");
    one_system(t, ".zip", "arcade");
    put(t.fake + "/slow", "1");
    CHECK(!oscontract::busy());
    auto r = Run::create(t.cx, t.opts());
    r->start();
    CHECK(oscontract::busy());  // finding 9: /isIdle says false while a scrape runs
    CHECK(t.pump(*r, [&] { return r->phase() == Phase::Running; }));
    auto t0 = std::chrono::steady_clock::now();
    t.pump(*r, [&] { return std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(500); });
    CHECK(!r->finished());
    r->cancel();
    CHECK(t.pump(*r, [&] { return r->finished(); }, 8000));
    CHECK(r->phase() == Phase::Cancelled);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8));
    CHECK(!oscontract::busy());
    CHECK(!contains(t.gamelist(), "<scrap"));  // the interrupted game is not stamped as scraped
    // nor counted: the request B killed is not a network error of that game (the summary said "1 error" after a plain stop)
    CHECK_EQ(r->stats().done, 0u);
    CHECK_EQ(r->stats().errors, 0u);
    CHECK(r->errors().empty());
  }

  // ---- finding 4: a 2xx that cannot be read is an error, not "not found" (which is stamped for 15 days)
  {
    Rig t;
    use_fake2(t);
    t.cx.es.set_string("Scraper", "ArcadeDB");
    t.rom("pacman.zip");
    one_system(t, ".zip", "arcade");
    put(t.fake + "/adb.json", "<html><body>Bad gateway</body></html>");
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK_EQ(r->stats().errors, 1u);
    CHECK_EQ(r->stats().not_found, 0u);
    CHECK(!contains(t.gamelist(), "<scrap"));

    Rig u;
    use_fake2(u);
    u.cx.es.set_string("Scraper", "IGDB");
    u.cx.es.set_string("IGDBClientID", "c");
    u.cx.es.set_string("IGDBSecret", "s");
    u.rom("a.sfc");
    one_system(u, ".sfc", "snes");
    put(u.fake + "/igdb.json", "nope");
    auto r2 = Run::create(u.cx, u.opts());
    CHECK(u.run_to_end(*r2));
    CHECK_EQ(r2->stats().errors, 1u);
    CHECK_EQ(r2->stats().not_found, 0u);
    CHECK(!contains(u.gamelist(), "<scrap"));

    Rig v;  // ScreenScraper
    use_fake2(v);
    v.rom("a.sfc");
    one_system(v, ".sfc", "snes");
    put(v.fake + "/game.xml", "<Data><jeu id=\"1\"><noms>");
    auto r3 = Run::create(v.cx, v.opts());
    CHECK(v.run_to_end(*r3));
    CHECK_EQ(r3->stats().errors, 1u);
    CHECK_EQ(r3->stats().not_found, 0u);
    CHECK(!contains(v.gamelist(), "<scrap"));
  }

  // ---- finding 3: ScreenScraper is not asked for the next system id at once
  {
    Rig t;  // the default fake: 404 for every game
    t.rom("x.sfc");
    one_system(t, ".sfc", "atomiswave arcade");  // two ScreenScraper ids (53, 75)
    Options o = t.opts();
    o.min_interval_ms = 400;
    auto r = Run::create(t.cx, o);
    auto t0 = std::chrono::steady_clock::now();
    CHECK(t.run_to_end(*r));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK_EQ(count_of(t.requests(), "jeuInfos.php"), 2u);
    CHECK(ms >= 350);
  }

  // ---- finding 7: one timeout is retried before it counts as "no network"
  {
    Rig t;
    use_fake2(t);
    t.cx.es.set_string("Scraper", "ArcadeDB");
    t.rom("pacman.zip");
    one_system(t, ".zip", "arcade");
    put(t.fake + "/once28", "1");
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK(r->phase() == Phase::Finished);
    CHECK_EQ(r->stats().found, 1u);
    CHECK_EQ(r->stats().errors, 0u);
  }

  // ---- finding 6: curl 23 is "no space" only when the disk is nearly full
  {
    Rig t;
    use_fake2(t);
    t.cx.es.set_string("Scraper", "ArcadeDB");
    t.rom("pacman.zip");
    one_system(t, ".zip", "arcade");
    put(t.fake + "/code23", "1");
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK(r->phase() == Phase::Finished);  // plenty of room: a plain error of that game
    CHECK_EQ(r->stats().errors, 1u);
    ::setenv("ZF_SCRAPER_MINFREE_MB", "1000000000", 1);  // pretend the disk is full
    auto r2 = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r2));
    ::unsetenv("ZF_SCRAPER_MINFREE_MB");
    CHECK(r2->phase() == Phase::Aborted);
    CHECK(r2->abort_reason() == Abort::DiskFull);
  }

  // ---- finding 5: a media folder that cannot be made is reported (here `images` is a file)
  {
    Rig t;
    use_fake2(t);
    t.cx.es.set_string("Scraper", "ArcadeDB");
    t.rom("pacman.zip");
    one_system(t, ".zip", "arcade");
    put(t.roms() + "/images", "not a folder");
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK_EQ(r->stats().found, 1u);
    CHECK_EQ(r->stats().warnings, 1u);
  }

  // ---- finding 12: the file the gamelist named, replaced by one with another extension, does not stay behind
  {
    Rig t;
    use_fake2(t);
    t.cx.es.set_string("Scraper", "ArcadeDB");
    t.rom("pacman.zip");
    put(t.roms() + "/images/pacman-image.jpg", "OLDJPG");
    put(t.roms() + "/gamelist.xml", "<?xml version=\"1.0\"?>\n<gameList>\n<game><path>./pacman.zip</path><name>old</name><image>./images/pacman-image.jpg</image></game>\n</gameList>\n");
    one_system(t, ".zip", "arcade");
    auto r = Run::create(t.cx, t.opts());
    CHECK(t.run_to_end(*r));
    CHECK_EQ(r->stats().found, 1u);
    CHECK(fs::exists(t.roms() + "/images/pacman-image.png"));
    CHECK(!fs::exists(t.roms() + "/images/pacman-image.jpg"));
  }

  return zt::done();
}
