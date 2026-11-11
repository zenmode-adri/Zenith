// ArcadeDB (adb.arcadeitalia.net) protocol for the zfront scraper: the query URL and the JSON answer mapping. No key, no account.
// Pure functions, no I/O. ES's `ArcadeDBJSONScraper.cpp` is the reference.
#pragma once
#include <string>
#include <string_view>

#include "scraper_src.hpp"
#include "scraper_ss.hpp"

namespace zf::scraper::adb {

/// ES `isSupportedPlatform`: arcade, neogeo or lcdgames, and never the "teknoparrot" system.
bool platform_supported(std::string_view platforms, std::string_view system_name);

/// The MAME short name ArcadeDB is asked for: ROM stem without "(..)"/"[..]", spaces and dots removed.
std::string game_name(std::string_view rom_file);
/// GET http://adb.arcadeitalia.net/service_scraper.php?ajax=query_mame&lang=en&use_parent=1&game_name=<name>
HttpRequest game_request(std::string_view rom_file);

/// Reads `{"release":4,"result":[{...}]}` (ES wants release >= 4) and returns the first game with a title.
/// `short_title` = ES's ScrapeShortTitle; `wish` picks the media (ScrapperImageSrc / ThumbSrc / LogoSrc / titleshot / video).
/// False when the answer is not JSON, has the wrong release or no game.
bool parse_games(std::string_view json, const ss::Wish& wish, bool short_title, ss::Parsed& out);
/// The text is a `{"release":>=4,"result":[...]}` answer: anything else (HTML error page, old release) is not an answer.
bool valid_answer(std::string_view json);

}  // namespace zf::scraper::adb
