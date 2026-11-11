// `menu.retroachievements` (ES: RETROACHIEVEMENTS SETTINGS, GuiRetroAchievementsSettings): the account and the options RetroArch and the
// standalone emulators read from system.cfg. Keys, values and the login-on-close behaviour are in ra.hpp.
//
// Not ported from ES: ENCORE MODE (nothing reads `retroachievements.encore`) and GAME INDEXES (the hasher feeds ES's own viewer and
// collection; zfront's collection reads the gamelist).
#include <cctype>
#include <memory>

#include "menu_game_int.hpp"
#include "ra.hpp"
#include "registry.hpp"
#include "settings.hpp"

namespace zf::ra {
namespace {

using namespace zf::mg;
using namespace zf::mg::detail;

struct State {
  Snapshot before;     // what ES's save compares against; moves forward after a good TEST ACCOUNT
  bool testing = false;
  bool have = false;   // a TEST ACCOUNT answered
  LoginResult result;
  std::string shown_user;
};

Entry& flag(MenuBuilder& m, Ctx& cx, std::string label, const char* k) { return conf_switch(m, cx, std::move(label), k, "1", "0"); }

void build(MenuBuilder& m, Ctx& cx, const std::shared_ptr<State>& st) {
  Refresh rf = m.refresh;
  m.header(tr("SETTINGS"));
  flag(m, cx, tr("RETROACHIEVEMENTS"), key::kEnabled);
  conf_text(m, cx, tr("USERNAME"), key::kUser, false);
  conf_text(m, cx, tr("PASSWORD"), key::kPass, true);
  m.action(tr("TEST ACCOUNT"), [&cx, st, rf] {
    if (st->testing) return;
    st->testing = true;
    st->have = false;
    rf();
    std::shared_ptr<Services> sv = cx.sv;
    std::string user = conf_get(cx, key::kUser), pass = conf_get(cx, key::kPass);
    Ctx* c = &cx;
    auto res = std::make_shared<LoginResult>();
    sv->jobs.post_slow([sv, res, user, pass] { *res = login(*sv, user, pass); },
                  [c, st, res, user, rf] {
                    st->testing = false;
                    if (res->kind != LoginResult::Kind::Dry) {
                      st->have = true;
                      st->result = *res;
                      st->shown_user = user;
                      // a good login is what ES's save would have done: keep the token and do not ask again on leaving
                      if (res->ok() && conf_get(*c, key::kEnabled) == "1") {
                        conf_put(*c, key::kToken, res->token);
                        st->before = snapshot(*c);
                      }
                    }
                    rf();
                  });
  }).disabled(st->testing).hint(tr("Checks the user name and password with retroachievements.org."));
  if (st->testing) m.info(tr("CONNECTING TO RETROACHIEVEMENTS"));
  if (st->have) {
    if (st->result.ok()) m.info(tr("LOGGED IN"), st->shown_user);
    else m.info(describe(st->result)).danger();
  }

  m.header(tr("OPTIONS"));
  flag(m, cx, tr("HARDCORE MODE"), key::kHardcore).hint(tr("Disable loading states, rewind and cheats for more points."));
  flag(m, cx, tr("LEADERBOARDS"), key::kLeaderboards).hint(tr("Compete in high-score and best time leaderboards (requires hardcore)."));
  flag(m, cx, tr("VERBOSE MODE"), key::kVerbose).hint(tr("Show achievement progression on game launch and other notifications."));
  flag(m, cx, tr("RICH PRESENCE"), key::kRich);
  flag(m, cx, tr("AUTOMATIC SCREENSHOT"), key::kScreenshot).hint(tr("Automatically take a screenshot when an achievement is earned."));
  flag(m, cx, tr("CHALLENGE INDICATORS"), key::kChallenge).hint(tr("Shows icons in the bottom right corner when eligible achievements can be earned."));
  flag(m, cx, tr("UNOFFICIAL ACHIEVEMENTS"), key::kUnofficial).hint(tr("Enable unlocking of unofficial achievements."));

  std::vector<std::string> sounds = sound_names();
  if (!sounds.empty()) {
    std::vector<Option> o = {{"none", tr("NONE")}};
    for (const std::string& s : sounds) {
      std::string up = s;
      for (char& ch : up) ch = char(std::toupper((unsigned char)ch));
      o.push_back({s, up});
    }
    Ctx* c = &cx;
    m.choice(tr("UNLOCK SOUND"), o,
             [c, sounds] {
               std::string v = conf_get(*c, key::kSound);
               if (v.empty()) return std::string("none");
               for (const std::string& s : sounds)
                 if (s == v) return v;
               return std::string("none");  // the file is gone: ES shows the first entry too
             },
             [c](const std::string& v) { conf_put(*c, key::kSound, v); })
        .no_cycle();
  }
  if (has_stick_leds()) flag(m, cx, tr("LED NOTIFICATIONS"), key::kLeds);
  // ES: the switch of the main menu's RETROACHIEVEMENTS row (menu_main.cpp, ra::in_main_menu); that row opens these settings
  {
    Ctx* c = &cx;
    m.toggle(tr("SHOW RETROACHIEVEMENTS ENTRY IN MAIN MENU"), [c] { return c->es.get_bool(key::kMenuItem, true); },
             [c](bool on) {
               c->es.set_bool(key::kMenuItem, on);
               std::string err;
               if (!c->es.save(&err)) c->sv->toasts.push(tr("Could not save the setting"), 3000);
             })
        .hint(tr("Show the RetroAchievements row in the main menu."));
  }
}

std::unique_ptr<Screen> make_ra(Ctx& cx) {
  auto st = std::make_shared<State>();
  st->before = snapshot(cx);
  MenuSpec s;
  s.title = tr("RETROACHIEVEMENTS SETTINGS");
  s.title_key = N_("RETROACHIEVEMENTS SETTINGS");
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    build(m, cx, st);
  };
  s.on_close = [st](Ctx& cx) { apply_login(cx, st->before); };
  return make_menu(std::move(s));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.retroachievements", [](Ctx& cx, const std::string&) { return make_ra(cx); });

}  // namespace zf::ra
