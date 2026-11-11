// RetroAchievements web API client for the user summary (ES: RetroAchievements::getUserSummary + toRetroAchivementInfo), the data of
// ES's GuiRetroAchievements viewer. The client is complete and tested against fixtures; THERE IS NO SCREEN YET: the call needs a
// developer login (`z=<name>&y=<web API key>`) that ES gets from the build environment ($CHEEVOS_DEV_LOGIN) and that this tree does not
// have. Without it `available()` is false and nothing is asked.
//
// SECRETS: the key travels in the URL, so the request is a 0600 curl config like the login (ra.hpp); no text of the answer or the URL
// is kept in an error.
#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "app.hpp"

namespace zf::ra::api {

/// "z=<name>&y=<key>": compiled in from $CHEEVOS_DEV_LOGIN (CMake -> ZF_RA_DEV_LOGIN, like ES's package) or, for tests and debugging
/// over SSH, $ZF_RA_DEVLOGIN at run time. Empty when this build has none.
std::string dev_login();
inline bool available() { return !dev_login().empty(); }

struct Game {
  std::string id, name, console, badge_url, last_played;
  int won_soft = 0, won_hard = 0, total = 0;
  int score_soft = 0, score_hard = 0, possible = 0;
};

struct Profile {
  std::string error;  // non-empty: nothing else is valid (the site's own `Status`/error text, cleaned)
  std::string username, member_since, rank, points, soft_points, true_points, userpic_url;
  std::vector<Game> games;  // recently played with at least one achievement, in the site's order
};

/// The curl config (`curl -K`) for API_GetUserSummary. Secret (carries the key).
std::string summary_config(std::string_view dev, std::string_view user, int count);
/// Reads the JSON answer. False (and `out.error` set) when it is not a summary. Mirrors ES's toRetroAchivementInfo.
bool parse_summary(std::string_view body, std::string_view user, Profile& out);
/// Blocking (job thread): asks for the summary of `user` (the account of `menu.retroachievements` when empty).
Profile fetch_summary(Ctx& cx, const std::string& user, int count = 100);

}  // namespace zf::ra::api
