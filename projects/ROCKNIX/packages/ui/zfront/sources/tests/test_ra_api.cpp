// RetroAchievements user summary client, against fixtures (no network, no developer key). The JSON is shaped like the answer of
// API_GetUserSummary as ES reads it (RetroAchievements.cpp getUserSummary).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "app.hpp"
#include "check.hpp"
#include "ra_api.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

const char* kSummary = R"({
  "User":"ana","ID":4242,"MemberSince":"2021-03-04 10:11:12","Rank":"1500","TotalRanked":"90000",
  "TotalPoints":"1200","TotalSoftcorePoints":"300","TotalTruePoints":"2500","UserPic":"/UserPic/ana.png",
  "RecentlyPlayed":[
    {"GameID":"10","ConsoleID":"7","ConsoleName":"NES/Famicom","Title":"Super Mario Bros.","ImageIcon":"/Images/000001.png","LastPlayed":"2026-10-20 18:00:00"},
    {"GameID":"11","ConsoleID":"4","ConsoleName":"Game Boy","Title":"Tetris","ImageIcon":"","LastPlayed":"2026-10-19 12:00:00"},
    {"GameID":"12","ConsoleID":"4","ConsoleName":"Game Boy","Title":"Nothing Earned","ImageIcon":"/Images/9.png","LastPlayed":"2026-10-01 12:00:00"},
    {"GameID":"13","ConsoleID":"3","ConsoleName":"SNES","Title":"No Award Entry","ImageIcon":"/Images/8.png","LastPlayed":"2026-09-01 12:00:00"}
  ],
  "Awarded":{
    "10":{"NumPossibleAchievements":"20","PossibleScore":"400","NumAchieved":"12","ScoreAchieved":"240","NumAchievedHardcore":"5","ScoreAchievedHardcore":"100"},
    "11":{"NumPossibleAchievements":10,"PossibleScore":100,"NumAchieved":3,"ScoreAchieved":30,"NumAchievedHardcore":3,"ScoreAchievedHardcore":30},
    "12":{"NumPossibleAchievements":"8","PossibleScore":"80","NumAchieved":"0","ScoreAchieved":"0","NumAchievedHardcore":"0","ScoreAchievedHardcore":"0"}
  }
})";

}  // namespace

int main() {
  // ---- the request carries the key only inside the config, percent-encodes the user and caps the count
  {
    std::string c = ra::api::summary_config("z=zfront&y=KEYSECRET", "a b&c", 100);
    CHECK(c.find("https://retroachievements.org/API/API_GetUserSummary.php?z=zfront&y=KEYSECRET&u=a%20b%26c&g=100&a=100") != std::string::npos);
    CHECK(ra::api::summary_config("z=a&y=b", "u", 100000).find("&g=500&a=500") != std::string::npos);
    CHECK(ra::api::summary_config("z=a&y=b", "u", 0).find("&g=1&a=1") != std::string::npos);
  }

  // ---- parse (ES toRetroAchivementInfo)
  {
    ra::api::Profile p;
    CHECK(ra::api::parse_summary(kSummary, "ana", p));
    CHECK(p.error.empty());
    CHECK_EQ(p.username, std::string("ana"));
    CHECK_EQ(p.rank, std::string("1500 / 90000"));
    CHECK_EQ(p.points, std::string("1200"));
    CHECK_EQ(p.soft_points, std::string("300"));
    CHECK_EQ(p.true_points, std::string("2500"));
    CHECK_EQ(p.userpic_url, std::string("https://retroachievements.org/UserPic/ana.png"));
    CHECK_EQ(p.member_since, std::string("2021-03-04 10:11:12"));
    // "Nothing Earned" (an award entry with 0/0) is dropped; a game with no award entry stays (ES keeps it)
    CHECK_EQ(p.games.size(), size_t(3));
    CHECK_EQ(p.games[0].name, std::string("Super Mario Bros."));
    CHECK_EQ(p.games[0].badge_url, std::string("https://i.retroachievements.org/Images/000001.png"));
    CHECK(p.games[0].won_soft == 12 && p.games[0].won_hard == 5 && p.games[0].total == 20);
    CHECK(p.games[0].score_soft == 240 && p.games[0].score_hard == 100 && p.games[0].possible == 400);
    CHECK_EQ(p.games[1].name, std::string("Tetris"));
    CHECK(p.games[1].badge_url.empty());
    CHECK(p.games[1].won_soft == 3 && p.games[1].total == 10);  // numbers as numbers too
    CHECK_EQ(p.games[2].name, std::string("No Award Entry"));
    CHECK(p.games[2].total == 0);
  }
  // soft points when the site does not send them: true points minus points (ES)
  {
    ra::api::Profile p;
    CHECK(ra::api::parse_summary(R"({"ID":1,"Rank":"7","TotalPoints":"100","TotalTruePoints":"350"})", "bob", p));
    CHECK_EQ(p.soft_points, std::string("250"));
    CHECK_EQ(p.rank, std::string("7"));  // no TotalRanked: just the rank
    CHECK(p.games.empty());
  }
  // errors: not JSON, an error object, hostile text
  {
    ra::api::Profile p;
    CHECK(!ra::api::parse_summary("<html>502</html>", "ana", p) && !p.error.empty());
    CHECK(!ra::api::parse_summary(R"({"Error":"User not found\r\nsecretname"})", "secretname", p));
    CHECK(p.error.find("secretname") == std::string::npos && p.error.find('\n') == std::string::npos);
    CHECK(p.error.find("User not found") != std::string::npos);
    CHECK(!ra::api::parse_summary(R"({"message":"Invalid API key"})", "ana", p));
    CHECK_EQ(p.error, std::string("Invalid API key"));
    CHECK(!ra::api::parse_summary("[]", "ana", p));
  }

  // ---- fetch with the fake curl: needs the developer login; the key is in a 0600 file, never in an argument
  {
    char tmpl[] = "/tmp/zf-ra-api-XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    CHECK(d != nullptr);
    if (!d) return 1;
    const std::string root = d, fake = root + "/fake", scratch = root + "/tmp";
    fs::create_directories(fake);
    fs::create_directories(scratch);
    {
      std::ofstream f(fake + "/summary.json");
      f << kSummary;
      std::ofstream c(fake + "/curl");
      c << R"SH(#!/bin/bash
D="$ZF_FAKE_DIR"
echo "$*" >> "$D/argv.log"
[ "$1" = "-K" ] || exit 2
stat -c %a "$2" > "$D/cfg.mode"
cat "$D/summary.json"; printf '\n@@0|200\n'
)SH";
    }
    ::chmod((fake + "/curl").c_str(), 0755);
    ::setenv("ZF_CURL", (fake + "/curl").c_str(), 1);
    ::setenv("ZF_FAKE_DIR", fake.c_str(), 1);
    ::setenv("ZF_RA_TMP", scratch.c_str(), 1);
    ::unsetenv("ZF_RA_DEVLOGIN");

    Ctx cx;
    cx.sv->cmd.dry_run = false;
    cx.sv->cmd.keep_log = true;
    ra::api::Profile p = ra::api::fetch_summary(cx, "ana");
    if (!ra::api::available()) {  // a build without the developer key: nothing is asked
      CHECK(!p.error.empty());
      CHECK(!fs::exists(fake + "/argv.log"));
      ::setenv("ZF_RA_DEVLOGIN", "z=zfront&y=KEYSECRET", 1);
    }
    CHECK(ra::api::available());
    p = ra::api::fetch_summary(cx, "");
    CHECK(!p.error.empty());  // no user name configured
    p = ra::api::fetch_summary(cx, "ana");
    CHECK(p.error.empty() && p.games.size() == 3);
    std::ifstream m(fake + "/cfg.mode");
    std::string mode;
    std::getline(m, mode);
    CHECK_EQ(mode, std::string("600"));
    std::ifstream a(fake + "/argv.log");
    std::stringstream ss;
    ss << a.rdbuf();
    CHECK(ss.str().find("KEYSECRET") == std::string::npos);
    for (const std::string& line : cx.sv->cmd.log) CHECK(line.find("KEYSECRET") == std::string::npos);
    CHECK(fs::is_empty(scratch));
    for (const char* k : {"ZF_CURL", "ZF_FAKE_DIR", "ZF_RA_TMP", "ZF_RA_DEVLOGIN"}) ::unsetenv(k);
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  return zt::done();
}
