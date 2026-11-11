// IGDB protocol for the zfront scraper: Twitch client-credentials token, the apicalypse query and the answer mapping. Pure functions,
// no I/O: the tests feed them saved samples. ES's `IGDBScraper.cpp` is the reference (the platform table, the query fields, which
// image goes where).
//
// SECRETS: the client secret travels in the token request body and the token in every game request's header. Both live only in
// `HttpRequest`s that the engine writes to curl's 0600 config file.
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "scraper_src.hpp"
#include "scraper_ss.hpp"

namespace zf::scraper::igdb {

/// IGDB platform ids (several for "moonlight"/"windows") of the space separated `<platform>` text; unique, in order.
std::vector<std::string> platform_ids(std::string_view platforms);
bool platform_supported(std::string_view platforms);

/// POST https://id.twitch.tv/oauth2/token (ES `ensureToken`); the values are percent-encoded.
HttpRequest token_request(std::string_view client_id, std::string_view client_secret);
/// `{"access_token":"...","expires_in":N,...}`; false when there is no token. `expires_in` is 0 when missing.
bool parse_token(std::string_view json, std::string& token, long& expires_in);
/// ES keeps the token 30 minutes whatever the server says.
long token_ttl_s(long expires_in);

/// POST https://api.igdb.com/v4/games for the search `name` (ES `generateRequests`); `platform_id` empty = no platform filter.
HttpRequest game_request(std::string_view client_id, std::string_view token, std::string_view name, std::string_view platform_id);

/// What `wish` asks of IGDB: the cover (thumb), the first screenshot (image) and, with `wish.fanart`, the first artwork.
/// Reads the answer array and returns the first game with a name (ES gives every game to the caller, the first one wins).
/// False when the text is not a JSON array or has no game with a name.
/// `max_px` > 0 (ScraperResizeWidth): the cover and the screenshot come from IGDB's smaller size tokens (t_720p, t_screenshot_big...)
/// instead of the original, so a 1 GB console does not store multi-megabyte covers; 0 keeps ES's tokens.
bool parse_games(std::string_view json, const ss::Wish& wish, ss::Parsed& out, int max_px = 0);
/// The text is a JSON array (what the games endpoint answers): anything else is not an answer, whatever the HTTP code said.
bool valid_answer(std::string_view json);

}  // namespace zf::scraper::igdb
