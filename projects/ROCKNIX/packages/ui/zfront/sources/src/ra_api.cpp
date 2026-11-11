#include "ra_api.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <map>

#include "json.hpp"
#include "menu_game.hpp"
#include "ra.hpp"
#include "scraper_ss.hpp"
#include "util.hpp"

namespace zf::ra::api {

std::string dev_login() {
  if (const char* e = std::getenv("ZF_RA_DEVLOGIN"); e && *e) return e;
#ifdef ZF_RA_DEV_LOGIN
  return ZF_RA_DEV_LOGIN;
#else
  return {};
#endif
}

std::string summary_config(std::string_view dev, std::string_view user, int count) {
  namespace ss = zf::scraper::ss;
  std::string n = std::to_string(std::max(1, std::min(count, 500)));
  std::string c = "url = \"https://retroachievements.org/API/API_GetUserSummary.php?" + std::string(dev) + "&u=" + ss::url_encode(user) + "&g=" + n + "&a=" + n + "\"\n";
  c += "silent\nconnect-timeout = 15\nmax-time = 30\nmax-filesize = 4194304\nuser-agent = \"zfront\"\n";
  c += "write-out = \"\\n@@%{exitcode}|%{http_code}\\n\"\n";
  return c;
}

namespace {

int to_int(const json::Value* v) {
  if (!v) return 0;
  if (v->type == json::Value::Type::Number) return int(v->num);
  if (v->type == json::Value::Type::String) {
    long r = 0;
    return parse_int(v->str, r) ? int(r) : 0;
  }
  return 0;
}

}  // namespace

bool parse_summary(std::string_view body, std::string_view user, Profile& out) {
  out = Profile();
  json::Value v;
  if (!json::parse(body, v) || !v.is_object()) {
    out.error = tr("UNEXPECTED ANSWER FROM RETROACHIEVEMENTS");
    return false;
  }
  // an error answer: {"Status":..., "Error":...} or {"message":...} and no ID
  if (!v.find("ID") && !v.find("TotalPoints") && !v.find("Rank")) {
    std::string e = v.text("Error");
    if (e.empty()) e = v.text("message");
    if (e.empty()) e = v.text("Status");
    out.error = ra::clean_error(e, user, "");
    if (out.error.empty()) out.error = tr("UNEXPECTED ANSWER FROM RETROACHIEVEMENTS");
    return false;
  }
  out.username = std::string(user);
  out.member_since = v.text("MemberSince");
  out.userpic_url = v.find("UserPic") ? "https://retroachievements.org" + v.text("UserPic") : std::string();
  out.points = v.text("TotalPoints");
  out.true_points = v.text("TotalTruePoints");
  out.rank = v.text("Rank");
  const std::string ranked = v.text("TotalRanked");
  if (!ranked.empty() && !out.rank.empty()) out.rank += " / " + ranked;
  const std::string soft = v.text("TotalSoftcorePoints");
  long tp = 0, pt = 0;
  parse_int(out.true_points, tp);
  parse_int(out.points, pt);
  long sv = 0;
  out.soft_points = !soft.empty() && parse_int(soft, sv) ? std::to_string(sv) : std::to_string(tp - pt);  // ES: true points minus points

  const json::Value* awarded = v.find("Awarded");
  const json::Value* recent = v.find("RecentlyPlayed");
  if (recent && recent->is_array()) {
    for (const json::Value& r : recent->arr) {
      Game g;
      g.id = r.text("GameID");
      g.name = r.text("Title");
      g.console = r.text("ConsoleName");
      g.last_played = r.text("LastPlayed");
      const std::string icon = r.text("ImageIcon");
      if (!icon.empty()) g.badge_url = "https://i.retroachievements.org" + icon;
      const json::Value* aw = awarded ? awarded->find(g.id) : nullptr;
      if (aw) {
        if (to_int(aw->find("NumAchieved")) == 0 && to_int(aw->find("ScoreAchieved")) == 0) continue;  // ES hides games with nothing earned
        g.won_soft = to_int(aw->find("NumAchieved"));
        g.won_hard = to_int(aw->find("NumAchievedHardcore"));
        g.total = to_int(aw->find("NumPossibleAchievements"));
        g.score_soft = to_int(aw->find("ScoreAchieved"));
        g.score_hard = to_int(aw->find("ScoreAchievedHardcore"));
        g.possible = to_int(aw->find("PossibleScore"));
      }
      out.games.push_back(std::move(g));
    }
  }
  return true;
}

Profile fetch_summary(Ctx& cx, const std::string& user_in, int count) {
  Profile p;
  const std::string dev = dev_login();
  std::string user = user_in.empty() ? mg::conf_get(cx, key::kUser) : user_in;
  if (dev.empty() || user.empty()) {
    p.error = user.empty() ? tr("A valid account is required. Please register an account on https://retroachievements.org") : tr("NOT AVAILABLE IN THIS BUILD");
    return p;
  }
  const char* zc = std::getenv("ZF_CURL");
  std::string curl = (zc && *zc && ::access(zc, X_OK) == 0) ? zc : "curl";
  const char* base = std::getenv("ZF_RA_TMP");
  std::string tmpl = std::string(base && *base ? base : "/tmp") + "/zf-ra-XXXXXX";
  std::vector<char> t(tmpl.begin(), tmpl.end());
  t.push_back('\0');
  if (!::mkdtemp(t.data())) {
    p.error = tr("NO NETWORK CONNECTION");
    return p;
  }
  const std::string dir = t.data(), path = dir + "/c";
  std::string cfg = summary_config(dev, user, count);
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  bool ok = fd >= 0 && ::write(fd, cfg.data(), cfg.size()) == ssize_t(cfg.size());
  if (fd >= 0) ::close(fd);
  std::fill(cfg.begin(), cfg.end(), '\0');
  if (!ok) {
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
    p.error = tr("NO NETWORK CONNECTION");
    return p;
  }
  CmdResult c = cx.sv->cmd.run(shell_quote(curl) + " -K " + shell_quote(path) + " 2>/dev/null", 45000);
  ::unlink(path.c_str());
  ::rmdir(dir.c_str());
  if (c.dry) return p;  // harness: nothing asked, empty profile with no error
  size_t at = c.out.rfind("\n@@");
  long code = -1, http = 0;
  std::string body;
  if (at != std::string::npos) {
    body = c.out.substr(0, at);
    std::string_view tail(c.out);
    tail.remove_prefix(at + 3);
    size_t bar = tail.find('|');
    if (bar != std::string_view::npos) {
      parse_int(tail.substr(0, bar), code);
      parse_int(trim(tail.substr(bar + 1)), http);
    }
  }
  if (code != 0 || http == 0) {
    p.error = tr("NO NETWORK CONNECTION");
    return p;
  }
  parse_summary(body, user, p);
  return p;
}

}  // namespace zf::ra::api
