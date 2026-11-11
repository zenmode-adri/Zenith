// Which database the scraper asks (ES's `Scraper` setting, es-app/src/scrapers/Scraper.cpp:23-40): ScreenScraper (scraper_ss), IGDB
// (scraper_igdb) or ArcadeDB (scraper_adb). Pure functions: the engine (scraper_run.cpp) switches on `Source`, the menus list them.
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "settings.hpp"

namespace zf::scraper {

enum class Source : unsigned char { ScreenScraper, IGDB, ArcadeDB };

/// ES's name of the source, as written in es_settings.cfg and in `<scrap name=...>` ("ScreenScraper", "IGDB", "ArcadeDB").
const char* source_name(Source s);
/// Reverse of `source_name`; anything else (empty, the unknown TheGamesDB/HfsDB that this ES build does not have) is ScreenScraper,
/// the ES default (Settings.cpp:255).
Source source_from_name(std::string_view name);
/// `Scraper` of es_settings.cfg, default "ScreenScraper".
Source source_of(const EsSettings& es);
constexpr const char* kKeySource = "Scraper";

/// One HTTP request the engine sends with curl: GET when `post` is false. The headers and the body can carry the client secret
/// or a token, so they only ever go into curl's private 0600 config file (never a command line, log or toast).
struct HttpRequest {
  std::string url;
  std::vector<std::string> headers;  // "Name: value"
  std::string body;
  bool post = false;
};

/// ES's `Utils::String::removeParenthesis(getStem(path))`: the file name without extension and without "(...)" and "[...]" parts,
/// trimmed. This is what IGDB and ArcadeDB are searched with.
std::string search_stem(std::string_view rom_file);

/// The sources in ES's list order.
std::vector<Source> sources();

/// This build can use the source: ScreenScraper needs the developer login compiled in, the others need nothing (IGDB needs the
/// user's own client id and secret, which the run checks).
bool available(Source s);

/// The source knows the system: `platforms` is the space separated `<platform>` of es_systems.cfg, `system_name` its `<name>`.
/// ScreenScraper: its system id table; IGDB: ES's igdb_platformids (IGDBScraper.cpp:23-120); ArcadeDB: arcade, neogeo or lcdgames
/// platforms and not the "teknoparrot" system (ArcadeDBJSONScraper.cpp:isSupportedPlatform).
bool platform_supported(Source s, std::string_view platforms, std::string_view system_name);

}  // namespace zf::scraper
