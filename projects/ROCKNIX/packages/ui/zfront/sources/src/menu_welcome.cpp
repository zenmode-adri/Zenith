// `menu.welcome`: the first-boot welcome, 4 steps (language, WiFi, time zone, all set). It is a `Menu` per step, so EVERYTHING about its
// layout comes from the framework's grid (`ui::Metrics`: cells `u`, letter sizes `em` / `small_px`, `lines` = rows that fit the height
// left by the status bar and the help bar, scrolling, ellipsis, TEXT SIZE, the 1:1 and 16:9 shapes). This file has NO pixel constant.
//
//   title (title area)   the heading of the step ("Choose your language"...)
//   first row (header)   "Welcome · N of 4"
//   rows                 the cyan-framed list: languages / WiFi actions / zones with their clock / the three hints + "Start playing"
//   hint under the list  the one-line explanation of the step
//   help bar             per step (static arrays below)
//
// Steps are replaced one by another (`Transition::replace`); the flow state lives in a shared `Session`. Start = skip all
// (`Transition::close_menus`, `on_dismiss` stores `zfront.welcome_done`). The logic and its tests are in welcome.cpp / test_welcome.cpp.
#include "menu_welcome.hpp"

#include <time.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "i18n.hpp"
#include "menu.hpp"
#include "menu_network.hpp"  // netmenu::has_wifi_hw(), set_wifi_enabled(), local_ip_list()
#include "menu_system.hpp"   // msys::set_timezone
#include "menu_ui.hpp"       // ui_language_options(), language_names()
#include "registry.hpp"
#include "uimode.hpp"
#include "util.hpp"
#include "addgames.hpp"  // the phone row of step 4: web transfer on + QR, as the carousel's "Add games" card
#include "welcome.hpp"

namespace zf {

namespace {

namespace w = zf::welcome;

constexpr const char* kDoneKey = "zfront.welcome_done";

// ---- environment hooks for the harness (tests, test_layout): like ZF_NET_ROOT in menu_network.cpp. Without them nothing changes. ----

std::vector<std::string> split_commas(const char* s) {
  std::vector<std::string> v;
  std::string cur;
  for (const char* p = s; *p; ++p) {
    if (*p == ',' || *p == '\n') {
      if (!trim(cur).empty()) v.emplace_back(trim(cur));
      cur.clear();
    } else {
      cur += *p;
    }
  }
  if (!trim(cur).empty()) v.emplace_back(trim(cur));
  return v;
}

/// zfront's OWN catalogs: `<dir>/<lang>/LC_MESSAGES/zfront.mo`. `available_languages` looks at `opts.domains.back()`, so a copy of the options with
/// only the `zfront` domain lists exactly those. A language that ES translates but zfront does not is therefore never offered.
std::vector<w::LangName> languages_of(const std::vector<std::string>& catalogs) {
  std::vector<w::LangName> known;
  for (const LanguageName& l : language_names()) known.push_back({l.code, l.name});
  return w::complete_languages(catalogs, known);
}

std::vector<w::LangName> offered_languages() {
  if (const char* env = std::getenv("ZF_WELCOME_LANGS")) return languages_of(split_commas(env));  // harness only
  return welcome_languages(ui_language_options());
}

bool wifi_present() {
  if (const char* env = std::getenv("ZF_WELCOME_WIFI")) return env[0] == '1';
  return netmenu::has_wifi_hw();
}

/// Offset of a zone from the C library's zone database (`TZ` is switched for the call and put back). Main thread only.
bool system_offset(const std::string& zone, long long utc, int& off) {
  if (zone.empty() || zone.find("..") != std::string::npos || !file_exists("/usr/share/zoneinfo/" + zone)) return false;
  const char* old = std::getenv("TZ");
  const bool had = old != nullptr;
  const std::string saved = had ? old : "";
  setenv("TZ", zone.c_str(), 1);
  tzset();
  time_t t = time_t(utc);
  struct tm lt = {};
  const bool ok = localtime_r(&t, &lt) != nullptr;
  if (ok) off = int(lt.tm_gmtoff);
  if (had) setenv("TZ", saved.c_str(), 1);
  else unsetenv("TZ");
  tzset();
  return ok;
}

std::vector<std::string> parse_zone_list(const std::string& out) {  // `timeinfo timezones`: names separated by commas / newlines
  std::vector<std::string> v;
  std::string cur;
  auto flush = [&] {
    std::string z(trim(cur));
    if (!z.empty()) v.push_back(z);
    cur.clear();
  };
  for (char c : out) {
    if (c == ',' || c == '\n') flush();
    else cur += c;
  }
  flush();
  return v;
}

// ---- the shared state of the four steps ----

struct Session {
  w::Flow flow;
  bool forced = false;
  std::vector<w::LangName> langs;
  std::vector<std::string> zones;  // all the zones the OS knows
  bool zones_loaded = false, zones_started = false;
  std::string region;              // region shown in the zone step
  bool keep_first = false;         // no suggestion: the "Keep UTC" row goes first (visible without scrolling)
  std::string want_id;             // entry the cursor must go to once it exists ("lang:es_ES", "zone:Europe/Madrid")
  bool place_pending = false;
  bool done_stored = false;
  bool wifi_list_open = false;   // the WiFi step opened the network list on top (it closes itself after a join)
  std::string wifi_ssid_before;  // wifi.ssid when it opened it: a different one now means a new network was joined
  bool wifi_online_before = false;
  explicit Session(bool wifi) : flow(wifi) {}
};

void store_done(Ctx& cx, Session& s) {
  if (s.done_stored) return;
  s.done_stored = true;
  std::string err;
  if (!cx.conf.persist(kDoneKey, "1", &err)) cx.sv->toasts.push(tr("Could not save the settings"));
}

// ---- static help bars: one per step (MenuSpec::help wants static storage); "R" is the same icon as in manual.cpp and the keyboard ----
const ui::Prompt kHelpLang[] = {{"A", N_("SELECT")}, {"start", N_("SKIP ALL")}};
const ui::Prompt kHelpWifi[] = {{"A", N_("SELECT")}, {"B", N_("BACK")}, {"start", N_("SKIP ALL")}};
const ui::Prompt kHelpZone[] = {{"A", N_("SELECT")}, {"B", N_("BACK")}, {"R", N_("REGION")}, {"start", N_("SKIP ALL")}};
const ui::Prompt kHelpDone[] = {{"A", N_("SELECT")}, {"B", N_("BACK")}};

std::unique_ptr<Screen> make_step(Ctx& cx, const std::shared_ptr<Session>& s);

/// "Welcome · 2 of 4"
std::string counter(const Session& s) {
  return std::string(tr("Welcome")) + " \xC2\xB7 " + std::to_string(s.flow.number()) + " " + tr("of") + " " + std::to_string(s.flow.total());
}

/// Moves the flow and returns the screen that follows (the next step, or the end of the welcome).
Transition advance(Ctx& cx, const std::shared_ptr<Session>& s, w::Ev e) {
  s->flow.apply(e);
  if (s->flow.finished()) {
    store_done(cx, *s);
    // "Run welcome again" (System): back on the page it was opened from. On first boot: the carousel.
    return s->forced ? Transition::pop() : Transition::close_menus();
  }
  return Transition::replace(make_step(cx, s));
}

// ---- the screen: a Menu that owns Start / B / L / R and the timers of its step ----

class WelcomeMenu : public Menu {
 public:
  WelcomeMenu(MenuSpec spec, std::shared_ptr<Session> s) : Menu(std::move(spec)), s_(std::move(s)), step_(s_->flow.step()) {}

  Transition handle(Action a, Ctx& cx) override {
    switch (a) {
      case Action::Start:
        s_->flow.apply(w::Ev::Skip);
        store_done(cx, *s_);
        if (s_->forced) return Transition::pop();  // opened from System: back there
        return Transition::close_menus();  // also runs on_dismiss of every menu below
      case Action::B:
        if (s_->flow.step() == w::Step::Language) return Transition::none();  // nothing before the first step
        return advance(cx, s_, w::Ev::Back);
      case Action::L:
      case Action::R:
        if (step_ == w::Step::Zone && s_->zones_loaded) {
          s_->region = w::step_region(w::regions(s_->zones), s_->region, a == Action::R ? +1 : -1);
          std::vector<std::string> in = w::zones_in_region(s_->zones, s_->region);
          s_->want_id = in.empty() ? std::string("zone:UTC") : "zone:" + in.front();
          s_->place_pending = true;
          rebuild(cx);
          return Transition::none();
        }
        return Menu::handle(a, cx);  // elsewhere L / R page the list as in every menu
      default:
        return Menu::handle(a, cx);
    }
  }

  // Back on top after the WiFi list it opened: a network was joined there (a new SSID, or online now and not before) -> the next step,
  // no "Continue" to press (the list closed itself after the join). Backing out of the list without joining stays on this step.
  Transition take_transition(Ctx& cx) override {
    if (step_ == w::Step::Wifi && s_->wifi_list_open) {
      s_->wifi_list_open = false;
      const bool online = cx.conf.get_bool("wifi.enabled", false) && !netmenu::local_ip_list().empty();
      const std::string ssid = cx.conf.get_or("wifi.ssid", "");
      if ((!ssid.empty() && ssid != s_->wifi_ssid_before) || (online && !s_->wifi_online_before)) return advance(cx, s_, w::Ev::Next);
    }
    return Menu::take_transition(cx);
  }

  bool tick(float dt, Ctx& cx) override {
    bool changed = Menu::tick(dt, cx);  // builds the entries before the first draw (test_welcome, test_layout)
    if (step_ == w::Step::Zone) {
      const long long minute = (long long)::time(nullptr) / 60;
      if (minute_ != minute) {  // the clocks of the rows change once a minute: one rebuild, never per frame
        const bool first = minute_ < 0;
        minute_ = minute;
        if (!first) rebuild(cx), changed = true;
      }
    } else if (step_ == w::Step::Wifi) {
      poll_ += dt;
      if (poll_ >= kPollSeconds) {
        poll_ = 0;
        const std::string sig = wifi_signature(cx);
        if (sig != wifi_sig_) {
          wifi_sig_ = sig;
          rebuild(cx), changed = true;
        }
      }
    }
    if (s_->place_pending && size() > 0) {
      place(cx);
      changed = true;
    }
    return changed;
  }

  std::optional<float> wake_in() const override {
    std::optional<float> base = Menu::wake_in();
    float mine = -1;
    if (step_ == w::Step::Zone) mine = float(60 - (long long)::time(nullptr) % 60) + 0.05f;
    else if (step_ == w::Step::Wifi) mine = std::max(kPollSeconds - poll_, 0.0f) + 0.01f;
    if (mine < 0) return base;
    return base ? std::min(*base, mine) : mine;
  }

  void on_dismiss(Ctx& cx) override {
    store_done(cx, *s_);  // closed by Start from a menu opened on top (the WiFi list): the welcome counts as seen
    Menu::on_dismiss(cx);
  }

  static std::string wifi_signature(Ctx& cx) {
    return cx.conf.get_or("wifi.enabled", "") + "|" + cx.conf.get_or("wifi.ssid", "") + "|" + std::to_string(netmenu::local_ip_list().size());
  }

 private:
  static constexpr float kPollSeconds = 2.0f;

  /// Puts the cursor on the entry `want_id` (the list is rebuilt, so the cursor is moved with the keys the user has: bounded loop).
  /// Menu has no setter by id, hence the keys.
  void place(Ctx& cx) {
    s_->place_pending = false;
    size_t target = size();
    for (size_t i = 0; i < size(); ++i)
      if (entry(i).id == s_->want_id) target = i;
    if (target == size()) return;
    for (size_t guard = 0; guard < 2 * size() && cursor() != int(target); ++guard) Menu::handle(cursor() < int(target) ? Action::Down : Action::Up, cx);
  }

  std::shared_ptr<Session> s_;
  w::Step step_;
  long long minute_ = -1;
  float poll_ = 0;
  std::string wifi_sig_;
};

// ---- step 1: language ----

MenuSpec language_spec(Ctx& cx, const std::shared_ptr<Session>& s) {
  if (s->langs.empty()) s->langs = offered_languages();
  if (s->want_id.empty() && !s->langs.empty()) {  // the cursor starts on the language that is on now
    s->want_id = "lang:" + s->langs[w::language_index(s->langs, cx.conf.get_or("system.language", ""))].code;
    s->place_pending = true;
  }
  MenuSpec m;
  m.title = tr("Choose your language");
  m.title_key = N_("Choose your language");
  m.title_short = tr("Language");
  m.title_short_key = N_("Language");
  m.help = kHelpLang;
  m.help_n = sizeof kHelpLang / sizeof *kHelpLang;
  m.build = [s](MenuBuilder& b, Ctx&) {
    b.header(counter(*s));
    for (const w::LangName& l : s->langs) {
      const std::string code = l.code;
      b.action(l.name, [s, code](Ctx& cx) -> Transition {
         std::string err;
         msys::keyboard_follow_language(cx, code);  // before the key: it compares with the layout of the old language
         if (!cx.conf.persist("system.language", code, &err)) {
           cx.sv->toasts.push(tr("Could not save the settings"));
           return Transition::none();
         }
         i18n::set_language(code, ui_language_options());  // the next step is built in the new language
         s->want_id.clear();
         return advance(cx, s, w::Ev::Next);
       }).with_id("lang:" + code).hint(tr("You can change it later in System."));
    }
  };
  return m;
}

// ---- step 2: WiFi ----

MenuSpec wifi_spec(Ctx&, const std::shared_ptr<Session>& s) {
  MenuSpec m;
  m.title = tr("Connect to WiFi");
  m.title_key = N_("Connect to WiFi");
  m.title_short = tr("WiFi");
  m.title_short_key = N_("WiFi");
  m.help = kHelpWifi;
  m.help_n = sizeof kHelpWifi / sizeof *kHelpWifi;
  m.build = [s](MenuBuilder& b, Ctx& cx) {
    const bool enabled = cx.conf.get_bool("wifi.enabled", false);
    const bool connected = enabled && !netmenu::local_ip_list().empty();
    const std::string ssid = cx.conf.get_or("wifi.ssid", "");
    const std::string why = tr("Clock, updates and file transfer.");
    b.header(counter(*s));
    if (connected) b.info(tr("Connected"), ssid);
    const Refresh refresh = b.refresh;
    // Already connected: "Continue" comes first, so the cursor starts on it and A does not open the list by mistake (seen on the R36 Ultra).
    auto next = [&] {
      b.action(connected ? tr("Continue") : tr("Not now"), [s](Ctx& cx) -> Transition { return advance(cx, s, w::Ev::Next); }).with_id("wifi:next").hint(why);
    };
    if (connected) next();
    // The existing list does the scan, the key and the connection (menu_network.cpp); it opens on top and B comes back here.
    b.action(connected ? tr("Choose another network") : tr("Choose a network"), [refresh, s, connected](Ctx& cx) -> Transition {
       if (!cx.conf.get_bool("wifi.enabled", false)) netmenu::set_wifi_enabled(cx, true, refresh);  // the list is only reachable with WiFi on
       s->wifi_list_open = true;
       s->wifi_ssid_before = cx.conf.get_or("wifi.ssid", "");
       s->wifi_online_before = connected;
       return Transition::open("menu.network.wifi", "welcome");  // it closes itself after a successful join
     }).with_id("wifi:choose").hint(why);
    if (!connected) next();
  };
  return m;
}

// ---- step 3: time zone ----

MenuSpec zone_spec(Ctx& cx, const std::shared_ptr<Session>& s) {
  if (!s->zones_started) {
    s->zones_started = true;
    if (const char* env = std::getenv("ZF_WELCOME_ZONES")) {
      s->zones = split_commas(env);
      s->zones_loaded = true;
    }
  }
  MenuSpec m;
  m.title = tr("Where are you?");
  m.title_key = N_("Where are you?");
  m.title_short = tr("Time zone");
  m.title_short_key = N_("Time zone");
  m.help = kHelpZone;
  m.help_n = sizeof kHelpZone / sizeof *kHelpZone;
  m.on_open = [s](Ctx& cx, const Refresh& refresh) {
    if (s->zones_loaded) {
      // The suggestion needs the zone list: when it is already here (back from step 4) just put the cursor on it.
      return;
    }
    Services* sv = cx.sv.get();
    auto out = std::make_shared<std::string>();
    sv->jobs.post_slow([sv, out] { *out = sv->cmd.run("/usr/bin/timeinfo timezones", 10000).out; },
                       [s, out, refresh] {
                         s->zones = parse_zone_list(*out);
                         s->zones_loaded = true;
                         refresh();
                       });
  };
  m.build = [s](MenuBuilder& b, Ctx& cx) {
    // Region and cursor are decided once, the first time the list is here: the zone of `suggest_zone` (wifi.country -> language -> UTC).
    // Without internet access of any kind: the country is the console's own setting.
    if (s->zones_loaded && s->region.empty()) {
      // The cursor starts on the zone the console has now (when it is a real one, not UTC), else on the suggestion; with neither, "Keep UTC".
      const std::string cur = w::resolve_zone(s->zones, cx.conf.get_or("system.timezone", ""));
      const std::string pick = cur != "UTC" ? cur : w::suggest_zone(cx.conf.get_or("wifi.country", ""), cx.conf.get_or("system.language", ""), s->zones);
      s->keep_first = pick == "UTC";  // no clue: "Keep UTC" first, in the region of the current zone (or Europe)
      s->region = w::initial_region(pick, cur, w::regions(s->zones));
      s->want_id = "zone:" + pick;
      s->place_pending = true;
    }
    b.header(counter(*s));
    const long long now = (long long)::time(nullptr);
    const std::string why = tr("Sets your local time.");
    if (s->zones_loaded && !s->region.empty()) {
      // The region is a row of its own (left / right changes it, A opens the list), not only a word in the header and an L / R hint
      // nobody saw (seen on the R36 Ultra: "only America"). L / R still work anywhere in the step.
      std::vector<Option> regs;
      for (const std::string& r : w::regions(s->zones)) regs.push_back({r, r});
      const Refresh refresh = b.refresh;
      b.choice(tr("REGION"), std::move(regs), [s] { return s->region; }, [s, refresh](const std::string& v) {
         if (v == s->region) return;
         s->region = v;
         s->want_id = "zone:region";  // the zones of the new region come in under it: the cursor stays on this row
         s->place_pending = true;
         if (refresh) refresh();
       }).with_id("zone:region").hint(why);
    }
    auto pick = [s](const std::string& zone) {
      return [s, zone](Ctx& cx) -> Transition {
        const std::string z = w::resolve_zone(s->zones, zone);  // an unknown zone never reaches system.timezone
        msys::set_timezone(cx, z);  // the same setter as SYSTEM > TIMEZONE
        return advance(cx, s, w::Ev::Next);
      };
    };
    auto keep_row = [&] { b.action(tr("Keep UTC"), pick("UTC")).value(w::zone_clock("UTC", now, system_offset)).with_id("zone:UTC").hint(why); };
    if (s->keep_first) keep_row();
    if (!s->zones_loaded) b.info(tr("LOADING..."));  // translated in every catalog
    for (const std::string& z : w::zones_in_region(s->zones, s->region)) {
      b.action(w::pretty_zone(z), pick(z)).value(w::zone_clock(z, now, system_offset)).with_id("zone:" + z).hint(why);
    }
    if (!s->keep_first) keep_row();
  };
  return m;
}

// ---- step 4: all set ----

MenuSpec done_spec(Ctx&, const std::shared_ptr<Session>& s) {
  MenuSpec m;
  m.title = tr("All set");
  m.title_key = N_("All set");
  m.help = kHelpDone;
  m.help_n = sizeof kHelpDone / sizeof *kHelpDone;
  m.build = [s](MenuBuilder& b, Ctx& cx) {
    b.header(counter(*s));
    // Online: the first row is a button (the cursor starts on it) that turns the web transfer on, as the TRANSFER FILES switch does,
    // and shows the QR code; back from it, the welcome is still here. Offline the QR cannot work: the row is a hint and the cursor
    // starts on "Start playing" (many consoles have no WiFi at all).
    const bool online = addgames::online(cx);
    if (online) {
      b.action(tr("Add games from your phone"), [](Ctx& cx) -> Transition { return addgames::open_phone_page(cx); })
          .value("QR")
          .with_id("done:qr")
          .hint(tr("Scan it with the phone camera: the page opens and knows the PIN."));
    } else {
      b.info(tr("Add games from your phone"), tr("Network"));
    }
    b.separator();
    b.info(tr("Start opens the menu"), tr("anywhere"));
    b.info(tr("Screen looks wrong?"), tr("Zenith DTB Selector"));  // two halves: one long sentence was clipped with an ellipsis at 480x320 and 640x480
    b.separator();
    b.action(tr("Start playing"), [s](Ctx& cx) -> Transition { return advance(cx, s, w::Ev::Next); }).with_id("done:go");
  };
  return m;
}

std::unique_ptr<Screen> make_step(Ctx& cx, const std::shared_ptr<Session>& s) {
  MenuSpec spec;
  switch (s->flow.step()) {
    case w::Step::Language: spec = language_spec(cx, s); break;
    case w::Step::Wifi: spec = wifi_spec(cx, s); break;
    case w::Step::Zone: spec = zone_spec(cx, s); break;
    case w::Step::Done: spec = done_spec(cx, s); break;
  }
  return std::make_unique<WelcomeMenu>(std::move(spec), s);
}

}  // namespace

std::vector<w::LangName> welcome_languages(const i18n::Options& opts) {
  i18n::Options o = opts;
  o.domains = {"zfront"};  // available_languages looks at the LAST domain: only zfront's own catalogs count, never ES's
  return languages_of(i18n::available_languages(o));
}

bool welcome_due(Ctx& cx) {
  w::Facts f;
  f.done = cx.conf.get_bool(kDoneKey, false);
  f.restricted = uimode::of(cx.es) != uimode::Mode::Full;
  f.wifi_saved = !cx.conf.get_or("wifi.ssid", "").empty();
  // A console updated by OTA keeps its settings: any key in es_settings.cfg that the pristine image does NOT ship, or a file-transfer choice,
  // means it was set up by somebody, so it never gets a welcome it did not ask for. The image's own es_settings.cfg is not empty: emulationstation's
  // package.mk adds EnableSounds, ThemeSet, ScreenSaverTime (and BackgroundJoystickInput on S905), so those do not count.
  bool es_touched = false;
  for (const std::string& k : cx.es.names())
    if (k != "EnableSounds" && k != "ThemeSet" && k != "ScreenSaverTime" && k != "BackgroundJoystickInput") es_touched = true;
  f.used_marker = es_touched || !cx.conf.get_or("zfront.webtransfer", "").empty() || !cx.conf.get_or("zfront.files", "").empty();
  return w::should_show(f);
}

std::unique_ptr<Screen> make_welcome(Ctx& cx, bool forced) {
  auto s = std::make_shared<Session>(wifi_present());
  s->forced = forced;
  return make_step(cx, s);
}

namespace {

// `menu.welcome`: arg "" = start, "again" = Run welcome again. Test hook: arg "2".."4" opens that step (test_layout draws all of them).
std::unique_ptr<Screen> make_for_registry(Ctx& cx, const std::string& arg) {
  auto s = std::make_shared<Session>(wifi_present());
  s->forced = arg == "again";
  if (arg.size() == 1 && arg[0] >= '2' && arg[0] <= '4') {
    const int want = arg[0] - '0';
    while (s->flow.number() < want && !s->flow.finished()) s->flow.apply(w::Ev::Next);
  }
  return make_step(cx, s);  // the "is it due?" decision is `welcome_due`, asked by main before pushing this screen
}

}  // namespace

ZF_REGISTER_SCREEN("menu.welcome", [](Ctx& cx, const std::string& arg) { return make_for_registry(cx, arg); });

}  // namespace zf
