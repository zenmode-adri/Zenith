// PLAY TIME LIMIT, the device part (playtime.hpp): the process-wide tracker, the launch gate, the text for INFORMATION and the hooks that
// talk to the running game. No window: while a game runs zfront has none (the emulator owns the display), so the warning is
// RetroArch's own message and the end is the same kill as QUIT GAME.
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "homemenu.hpp"
#include "i18n.hpp"
#include "library_doctor.hpp"
#include "systems.hpp"
#include "oscontract.hpp"
#include "playtime.hpp"
#include "util.hpp"

namespace zf::playtime {

namespace {

std::unique_ptr<Store>& slot() {
  static std::unique_ptr<Store> s;
  return s;
}

/// RetroArch's UDP command port when the running game is RetroArch and its network commands are on (-1 otherwise).
int retroarch_port() {
  if (!homemenu::is_retroarch(oscontract::kill_names())) return -1;
  std::string cfg;
  if (!read_file(homemenu::kRetroarchCfg, cfg)) return -1;
  const homemenu::NetCmd n = homemenu::parse_retroarch_net(cfg);
  return n.enabled ? n.port : -1;
}

}  // namespace

std::string file_path() {
  const char* e = std::getenv("ZF_PLAYTIME_FILE");
  return e && *e ? e : "/storage/.config/zfront/playtime";
}

Store& store() {
  std::unique_ptr<Store>& s = slot();
  if (!s) {
    const std::string p = file_path();
    s.reset(new Store{load_file(p), Saver(p)});
  }
  return *s;
}

void reset_store() { slot().reset(); }

int limit_of(const SystemConf& conf) { return limit_minutes(conf.get_or(kKey, "")); }

bool counts_as_game(const System* sys) { return !sys || doctor::is_game_system(*sys); }

bool launch_blocked(const SystemConf& conf) {
  const int limit = limit_of(conf);
  if (limit <= 0) return false;  // Off: nothing is read, nothing is loaded
  Store& s = store();
  s.tracker.set_date(local_today());
  return s.tracker.exhausted(limit);
}

int minutes_left(const SystemConf& conf) {
  const int limit = limit_of(conf);
  if (limit <= 0) return -1;
  Store& s = store();
  s.tracker.set_date(local_today());
  return int((s.tracker.left_ms(limit) + 59999) / 60000);
}

std::string left_text(const SystemConf& conf) {
  const int m = minutes_left(conf);
  return m < 0 ? std::string() : std::to_string(m) + " min";
}

Hooks device_hooks() {
  Hooks h;
  h.warn = [](long left_ms) {
    const int port = retroarch_port();
    if (port <= 0) return;  // not RetroArch, or its network commands are off: no way to draw over the game
    // RetroArch's command table: `SHOW_MSG <text>` queues an on-screen message of about 3 seconds
    const unsigned long minutes = static_cast<unsigned long>(std::max(1L, (left_ms + 59999) / 60000));
    std::string text = i18n::ntr("Only %d minute of play time left today", "Only %d minutes of play time left today", minutes);
    const size_t at = text.find("%d");
    if (at != std::string::npos) text.replace(at, 2, std::to_string(minutes));
    for (char& c : text)
      if (c == '\n' || c == '\r') c = ' ';
    homemenu::send_udp_local(port, "SHOW_MSG " + text);
  };
  // RetroArch quits at the first SIGTERM through its normal shutdown (content closed, SRAM and the auto save written as on any quit);
  // the second one makes it exit at once without saving, which is why the retries of the watcher are 30 s apart
  h.expire = [](int attempt) {
    if (!oscontract::emukill())  // the launcher wrote no kill target, or its name is not a safe one: nothing to do but try again later
      std::fprintf(stderr, "zfront: play time limit: cannot close the game (attempt %d)\n", attempt);
  };
  return h;
}

std::unique_ptr<Watcher> begin_game(const SystemConf& conf) {
  const int limit = limit_of(conf);
  if (limit <= 0) return nullptr;
  Store& s = store();
  const std::string p = s.saver.path();
  const size_t slash = p.find_last_of('/');
  if (slash != std::string::npos && slash > 0) ::mkdir(p.substr(0, slash).c_str(), 0755);  // best effort: /storage/.config/zfront
  std::unique_ptr<Watcher> w(new Watcher(s.tracker, s.saver, limit, device_hooks()));
  w->start();
  return w;
}

}  // namespace zf::playtime
