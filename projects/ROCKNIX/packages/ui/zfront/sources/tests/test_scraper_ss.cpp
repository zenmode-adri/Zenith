// ScreenScraper protocol (scraper_ss): URL building, answer parsing (fixture), media choice, region/language, error
// classification, and that secrets never survive `scrub`.
#include <cstdlib>
#include <string>

#include "check.hpp"
#include "scraper_ss.hpp"
#include "util.hpp"

using namespace zf;
using namespace zf::scraper::ss;

static std::string fixture(const char* name) {
  std::string t;
  CHECK(read_file(std::string(ZF_TEST_DATA) + "/scraper/" + name, t));
  return t;
}
static const Pick* pick(const Parsed& p, const char* kind) {
  for (const Pick& k : p.picks)
    if (k.kind == kind) return &k;
  return nullptr;
}

int main() {
  // --- platforms
  CHECK(system_ids("snes") == std::vector<int>{4});
  CHECK(system_ids("nes") == std::vector<int>{3});
  CHECK(system_ids("atomiswave arcade") == (std::vector<int>{53, 75}));
  CHECK(system_ids("music tools").empty());
  CHECK(platform_supported("gba"));
  CHECK(!platform_supported("ebook"));

  // --- URLs: encoding, optional fields, login only when both parts are present
  Login l;
  l.dev = "devid=D&devpassword=P";
  l.soft = "zfront 1";
  GameQuery q;
  q.rom_name = "Sonic & Knuckles (Europe) é.md";
  q.system_id = 1;
  std::string u = game_url(l, q);
  CHECK(u.find("https://api.screenscraper.fr/api2/jeuInfos.php?devid=D&devpassword=P&softname=zfront%201&output=xml") == 0);
  CHECK(u.find("&romnom=Sonic%20%26%20Knuckles%20%28Europe%29%20%C3%A9.md&romtype=rom") != std::string::npos);
  CHECK(u.find("&systemeid=1") != std::string::npos);
  CHECK(u.find("md5=") == std::string::npos && u.find("romtaille") == std::string::npos && u.find("ssid") == std::string::npos);
  q.md5 = "abc";
  q.crc = "1f";
  q.size = 4096;
  l.user = "me";
  l.pass = "p@ss w/rd";
  u = game_url(l, q);
  CHECK(u.find("&md5=abc&crc=1f&romtaille=4096&systemeid=1&ssid=me&sspassword=p%40ss%20w%2Frd") != std::string::npos);
  l.pass.clear();
  CHECK(game_url(l, q).find("ssid") == std::string::npos);

  // --- scrub removes every credential value
  std::string s = scrub(u + " and 'sspassword=zzz' (ssid=aa) devid=1&x=2");
  CHECK(s.find("p%40ss") == std::string::npos);
  CHECK(s.find("devid=***&devpassword=***") != std::string::npos);
  CHECK(s.find("ssid=***&sspassword=***") != std::string::npos);
  CHECK(s.find("sspassword=***'") != std::string::npos);
  CHECK(s.find("zzz") == std::string::npos);
  CHECK(s.find("&x=2") != std::string::npos);
  CHECK_EQ(scrub("grassid=5"), std::string("grassid=5"));  // not a key (word boundary)

  // --- ROM region / language (ES LangParser)
  RomLang r = rom_lang("Super Mario World (USA).sfc", "snes", "snes", "es_ES");
  CHECK_EQ(r.region, std::string("us"));
  CHECK(r.languages == std::vector<std::string>{"en"});
  r = rom_lang("Zelda (Europe) (En,Fr,De).sfc", "snes", "snes", "en_US");
  CHECK_EQ(r.region, std::string("eu"));
  CHECK_EQ(r.languages.size(), 3u);
  r = rom_lang("Dragon Quest (Japan).sfc", "sfc", "snes", "en_US");
  CHECK_EQ(r.region, std::string("jp"));
  CHECK(r.languages == std::vector<std::string>{"jp"});
  r = rom_lang("rom.pce", "pcenginecd", "pcenginecd", "en_US");  // no tags: Japanese defaults of those systems
  CHECK_EQ(r.region, std::string("jp"));
  r = rom_lang("rom.pce", "pc", "pc", "en_US");  // "pc" is not "pc88"
  CHECK(r.region.empty());
  CHECK(r.languages == std::vector<std::string>{"en"});
  r = rom_lang("sf2.zip", "arcade", "arcade", "en_US");
  CHECK_EQ(r.region, std::string("us"));
  r = rom_lang("sf2j.zip", "arcade", "arcade", "en_US");
  CHECK_EQ(r.region, std::string("jp"));

  LangPick lp = pick_lang(rom_lang("Mario.sfc", "snes", "snes", "es_ES"), "es_ES");  // no tags: the locale decides
  CHECK_EQ(lp.language, std::string("es"));
  CHECK_EQ(lp.region, std::string("eu"));
  lp = pick_lang(rom_lang("Mario (USA).sfc", "snes", "snes", "es_ES"), "es_ES");
  CHECK_EQ(lp.region, std::string("us"));
  CHECK_EQ(lp.romlang, std::string("en"));  // the ROM is English only
  lp = pick_lang(rom_lang("Mario.sfc", "snes", "snes", ""), "");
  CHECK_EQ(lp.language, std::string("en"));
  CHECK_EQ(lp.region, std::string("us"));
  lp = pick_lang(rom_lang("Mario.sfc", "snes", "snes", "pt_BR"), "pt_BR");
  CHECK_EQ(lp.region, std::string("br"));

  // --- classification
  CHECK(classify(200, "<Data/>") == Reply::Ok);
  CHECK(classify(200, "Maximum threads per minute reached") == Reply::TooFast);
  CHECK(classify(200, "<html><body>oops</body></html>") == Reply::Transient);
  CHECK(classify(404, "Erreur : Rom/Iso/Dossier non trouvée !") == Reply::NotFound);
  CHECK(classify(400, "") == Reply::NotFound);
  CHECK(classify(401, "") == Reply::ApiClosed);
  CHECK(classify(403, "") == Reply::BadLogin);
  CHECK(classify(423, "") == Reply::Maintenance);
  CHECK(classify(426, "") == Reply::Blacklisted);
  CHECK(classify(429, "") == Reply::TooFast);
  CHECK(classify(430, "") == Reply::Quota);
  CHECK(classify(431, "") == Reply::TooManyFails);
  CHECK(classify(503, "") == Reply::Transient);
  CHECK(classify(0, "") == Reply::Transient);
  CHECK(is_fatal(Reply::Quota) && is_fatal(Reply::BadLogin) && !is_fatal(Reply::TooFast) && !is_fatal(Reply::NotFound) && !is_fatal(Reply::Transient));

  // --- answer: English, Spanish
  std::string xml = fixture("jeu_sfc.xml");
  Wish w;
  w.fanart = true;
  Parsed p;
  CHECK(parse_game(xml, pick_lang(rom_lang("Super Mario World (USA).sfc", "snes", "snes", "en_US"), "en_US"), w, p));
  CHECK_EQ(p.id, std::string("3"));
  CHECK_EQ(p.name, std::string("Super Mario World (USA)"));
  CHECK_EQ(p.desc, std::string("Mario & Luigi save Dinosaur Land."));  // double-escaped text is decoded once more, like ES
  CHECK_EQ(p.developer, std::string("Nintendo EAD"));
  CHECK_EQ(p.publisher, std::string("Nintendo"));
  CHECK_EQ(p.players, std::string("1-2"));
  CHECK_EQ(p.rating, std::string("0.9"));
  CHECK_EQ(p.date, std::string("19910823T000000"));
  CHECK_EQ(p.genre, std::string("Platform / Collect"));
  CHECK_EQ(p.family, std::string("Mario"));
  const Pick* im = pick(p, "image");
  CHECK(im && im->ext == ".png" && im->url.find("media=ss(wor)") != std::string::npos);
  const Pick* th = pick(p, "thumb");
  CHECK(th && th->ext == ".jpg" && th->url.find("media=box-2D(us)") != std::string::npos);  // USA ROM -> the US box, not the EU one
  const Pick* lg = pick(p, "marquee");
  CHECK(lg && lg->url.find("media=wheel(us)") != std::string::npos);  // "wheel" asked: the plain wheel of the region
  const Pick* vid = pick(p, "video");
  CHECK(vid && vid->ext == ".mp4" && vid->url.find("video-normalized") != std::string::npos);
  CHECK(pick(p, "manual") && pick(p, "map") && pick(p, "fanart"));
  CHECK(!pick(p, "boxback") && !pick(p, "titleshot"));  // switched off

  CHECK(parse_game(xml, pick_lang(rom_lang("Super Mario World (Europe).sfc", "snes", "snes", "es_ES"), "es_ES"), w, p));
  CHECK_EQ(p.name, std::string("Super Mario World (Europe)"));
  CHECK_EQ(p.desc, std::string("Mario y Luigi salvan la Tierra de los Dinosaurios."));
  CHECK_EQ(p.date, std::string("19920101T000000"));  // year only
  CHECK_EQ(p.genre, std::string("Plataformas / Coleccionar"));
  th = pick(p, "thumb");
  CHECK(th && th->url.find("media=box-2D(eu)") != std::string::npos);

  CHECK(parse_game(xml, pick_lang(rom_lang("Super Mario World (Japan).sfc", "snes", "snes", "es_ES"), "es_ES"), w, p));
  CHECK_EQ(p.name, std::string("Super Mario World JP"));
  CHECK_EQ(p.date, std::string("19901121T000000"));

  // options: no image source, no thumb, no logo, ScraperRegion, ratings off, same source for thumb as for image
  Wish w2;
  w2.image_src = "box-3D";
  w2.thumb_src = "box-3D";  // same as the image: not asked twice
  w2.logo_src.clear();
  w2.ratings = false;
  w2.video = w2.manual = w2.map = false;
  w2.region = "eu";
  CHECK(parse_game(xml, pick_lang(rom_lang("Mario (USA).sfc", "snes", "snes", "en_US"), "en_US"), w2, p));
  CHECK(p.rating.empty());
  CHECK_EQ(p.picks.size(), 1u);
  CHECK(pick(p, "image") && pick(p, "image")->ext == ".png");  // box-3D only exists for the US: falls back through the regions
  w2.image_src = "box-2D";
  w2.thumb_src.clear();
  CHECK(parse_game(xml, pick_lang(rom_lang("Mario (USA).sfc", "snes", "snes", "en_US"), "en_US"), w2, p));
  CHECK(pick(p, "image")->url.find("media=box-2D(eu)") != std::string::npos);  // the media region override wins over the ROM's

  // not XML / no game
  CHECK(!parse_game("Erreur : Jeu non trouvée !", lp, w, p));
  CHECK(!parse_game("<?xml version=\"1.0\"?><Data><serveurs/></Data>", lp, w, p));
  CHECK(!parse_game("", lp, w, p));

  // search answers have the game under <jeux>
  CHECK(parse_game("<Data><jeux><jeu id=\"9\"><noms><nom region=\"wor\">Found</nom></noms></jeu></jeux></Data>", lp, w, p));
  CHECK_EQ(p.name, std::string("Found"));
  CHECK(p.picks.empty());

  // --- user info
  UserInfo ui;
  CHECK(parse_user(xml, ui));
  CHECK_EQ(ui.maxthreads, 1);
  CHECK_EQ(ui.requests_today, 12);
  CHECK_EQ(ui.max_per_min, 20);
  CHECK_EQ(ui.max_per_day, 20000);
  CHECK(!parse_user("<Data/>", ui));

  // --- media urls
  CHECK_EQ(media_url("https://x/a b#screenscraperserveur#c", 0, true), std::string("https://x/a%20bhttps://www.screenscraper.fr/c"));
  CHECK_EQ(media_url("https://x/m.php?a=1&media=ss(wor)", 640, true), std::string("https://x/m.php?a=1&media=ss(wor)&maxwidth=640&maxheight=640"));
  CHECK_EQ(media_url("https://x/m.php?a=1&maxwidth=100&media=ss", 320, true), std::string("https://x/m.php?a=1&media=ss&maxwidth=320&maxheight=320"));
  CHECK_EQ(media_url("https://x/m.php?a=1&media=maps(wor)", 640, true), std::string("https://x/m.php?a=1&media=maps(wor)"));  // maps keep their size
  CHECK_EQ(media_url("https://x/v.php?a=1", 640, false), std::string("https://x/v.php?a=1"));

  // --- the developer login: from the environment, never printed by the tests
  ::setenv("ZF_SS_DEVLOGIN", "devid=a&devpassword=b", 1);
  CHECK_EQ(dev_login(), std::string("devid=a&devpassword=b"));
  ::unsetenv("ZF_SS_DEVLOGIN");
  return zt::done();
}
