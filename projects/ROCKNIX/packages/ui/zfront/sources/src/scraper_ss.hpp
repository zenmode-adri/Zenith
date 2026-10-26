// ScreenScraper (api2) protocol for the zfront scraper: request URLs, answer parsing, media choice, error classification.
// Pure functions, no I/O and no Ctx, so the tests feed them fixtures. ES's `ScreenScraper.cpp` is the reference for what is
// read from an answer and in which order of preference (region/language fallbacks, rip lists).
//
// SECRETS: the developer login, the user name and the password travel in every API URL, and ScreenScraper echoes them inside
// the media URLs it returns. A URL must therefore never reach a log, a toast, an error list or a summary: `scrub()` removes the
// four credential keys from any text that leaves the engine.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zf::scraper::ss {

/// Developer login ("devid=..&devpassword=.."): compiled in from $SCREENSCRAPER_DEV_LOGIN (like ES's package) or, for tests and
/// debugging over SSH, $ZF_SS_DEVLOGIN at run time. Empty when this build has none: then ScreenScraper cannot be used.
std::string dev_login();
std::string soft_name();

struct Login {
  std::string dev;        // dev_login()
  std::string soft;       // soft_name()
  std::string user, pass; // optional (anonymous works with low limits)
};

/// ScreenScraper system ids (can be several) for the space separated `<platform>` text of es_systems.cfg; sorted, unique.
std::vector<int> system_ids(std::string_view platforms);
bool platform_supported(std::string_view platforms);

// ---- request URLs --------------------------------------------------------------------------------------------------
/// RFC 3986 percent-encoding (UTF-8 bytes), unreserved characters kept.
std::string url_encode(std::string_view s);

struct GameQuery {
  std::string rom_name;      // file name with extension (`romnom`)
  std::string md5, crc;      // lower case; empty when unknown
  uint64_t size = 0;         // 0: not sent
  int system_id = 0;
};
std::string game_url(const Login& l, const GameQuery& q);
std::string user_url(const Login& l);

/// Replaces the values of devid / devpassword / ssid / sspassword / softname in any text with "***".
std::string scrub(std::string_view s);

// ---- ROM region / language (ES LangParser) ---------------------------------------------------------------------------
struct RomLang {
  std::string region;                  // "us", "eu", "jp", "br"... or empty
  std::vector<std::string> languages;  // "en", "fr"...
};
/// `file` is the ROM file name; `system_name`/`platforms` as in es_systems.cfg; `locale` is `system.language` ("es_ES").
RomLang rom_lang(std::string_view file, std::string_view system_name, std::string_view platforms, std::string_view locale);
/// ES's choice of preferred language and region for the answer: language = locale language, region from the ROM or the locale.
struct LangPick {
  std::string language, region, romlang;
};
LangPick pick_lang(const RomLang& rl, std::string_view locale);

// ---- answers -------------------------------------------------------------------------------------------------------
enum class Reply {
  Ok,           // 2xx
  NotFound,     // 404 / 400: the game is not in the database (not an error)
  BadLogin,     // 403
  ApiClosed,    // 401 (closed to anonymous / inactive members)
  Maintenance,  // 423 / 5xx maintenance text
  Blacklisted,  // 426
  Quota,        // 430: daily scrape quota
  TooManyFails, // 431: too many unrecognised ROMs
  TooFast,      // 429 / "maximum threads per minute reached": wait and retry
  Transient     // anything else (HTTP 5xx, HTML pages, odd answers)
};
Reply classify(int http, std::string_view body);
/// True for the replies after which the run must stop (nothing more can succeed now).
bool is_fatal(Reply r);

/// What the user asked to download (es_settings, ES names).
struct Wish {
  std::string image_src = "ss", thumb_src = "box-2D", logo_src = "wheel";  // "" = not wanted
  std::string region;                                                        // ScraperRegion override ("" = auto)
  bool ratings = true, video = true, fanart = false, boxback = false, manual = true, map = true, titleshot = false;
};

struct Pick {
  std::string kind;  // "image" "thumb" "marquee" "video" "fanart" "boxback" "manual" "map" "titleshot"
  std::string url;   // normalised, may carry credentials: never print
  std::string ext;   // ".png" ".mp4"... from the media's `format` attribute (may be empty: the download decides)
};

struct Parsed {
  std::string id, name, desc, genre, family, developer, publisher, players, date, rating;  // date: YYYYMMDDT000000; rating: "0.xxxxxx"
  std::vector<Pick> picks;
};

/// Reads a `jeuInfos.php` (or the first game of a `jeuRecherche.php`) XML answer. False when it is not XML or has no game.
bool parse_game(std::string_view xml, const LangPick& lp, const Wish& w, Parsed& out);

struct UserInfo {
  int maxthreads = 0, requests_today = 0, requests_ko_today = 0, max_per_min = 0, max_per_day = 0, max_ko_per_day = 0;
  std::string id;
};
bool parse_user(std::string_view xml, UserInfo& out);

/// Types tried, in order, for a source name of the settings ("ss" -> ss, sstitle...).
std::vector<std::string> rip_list(const std::string& src);

/// `url` with " " escaped and the ScreenScraper server placeholder resolved; for images `maxwidth`/`maxheight` are set to
/// `max_px` when > 0 (the server resizes: this build has no image encoder).
std::string media_url(std::string_view url, int max_px, bool is_image);

}  // namespace zf::scraper::ss
