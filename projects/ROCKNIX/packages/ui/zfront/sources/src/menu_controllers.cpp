// `menu.controllers`: ES GuiControllersSettings (settings, rumble, Bluetooth, display options) plus the list of connected
// pads that opens the mapping wizard. The Bluetooth screens are in menu_controllers_bt.cpp, the wizard in
// menu_controllers_wiz.cpp.
#include <sys/stat.h>

#include <chrono>
#include <cstdlib>

#include "menu_controllers.hpp"
#include "players.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf::ctl {

// ---- CtlMenu ----------------------------------------------------------------------------------------------------------

bool CtlMenu::tick(float dt, Ctx& cx) {
  bool moving = Menu::tick(dt, cx);
  if (left_ >= 0) {
    left_ -= dt;
    if (left_ <= 0) {
      left_ = -1;
      if (h_.poll) h_.poll(cx, refresher());
    }
  }
  if (left_ < 0 && h_.next_poll) {
    if (std::optional<float> n = h_.next_poll()) left_ = std::max(*n, 0.01f);
  }
  return moving;
}

std::optional<float> CtlMenu::wake_in() const {
  std::optional<float> w = Menu::wake_in();
  if (left_ >= 0) w = w ? std::min(*w, left_ + 0.002f) : left_ + 0.002f;
  return w;
}

Transition CtlMenu::take_transition(Ctx&) {
  if (h_.want_pop && h_.want_pop()) return Transition::pop();
  return Transition::none();
}

namespace {

// ---- the auto-pair scan outlives the page that started it ------------------------------------------------------------
struct AutoPair {
  TimePoint until{};
  bool active() const { return Clock::now() < until; }
};
AutoPair& auto_pair() {
  static AutoPair a;
  return a;
}
constexpr int kAutoPairSeconds = 60;  // `rocknix-bluetooth trust input` waits 60 one-second rounds

// ES SystemConf::getBool(key) without a default: only "1" is true.
bool conf_bool(Ctx& cx, const char* key) { return cx.conf.get_or(key, "") == "1"; }

void conf_set(Ctx& cx, const char* key, bool v) {
  std::string err;
  if (!cx.conf.persist(key, v ? "1" : "0", &err)) cx.sv->toasts.push(tr("Could not save the setting") + std::string(": ") + err, 3000);
}

void es_set(Ctx& cx, const char* name, bool v) {
  cx.es.set_bool(name, v);
  std::string err;
  if (!cx.es.save(&err)) cx.sv->toasts.push(tr("Could not save the setting") + std::string(": ") + err, 3000);
}

struct PageState {
  bool loaded = false;
  std::vector<JoyInfo> joys;
  std::vector<bool> configured;
  std::string sig;       // what the list above was made from (`pad_signature`)
  bool pairing = false;  // the "Searching..." mark was shown at the last poll
};

// The pads known now and the state of es_input.cfg (the wizard writes it): a change means the list of this page is stale.
std::string pad_signature(Ctx& cx) {
  std::string s;
  for (const JoyInfo& j : connected_pads(cx)) s += j.guid + "|" + j.name + ";";
  struct stat sb;
  if (::stat(paths().user_input.c_str(), &sb) == 0)
    s += std::to_string(sb.st_mtim.tv_sec) + "." + std::to_string(sb.st_mtim.tv_nsec) + "/" + std::to_string(sb.st_size);
  return s;
}

// The intro ES shows before the wizard (GuiMsgBox with OK / CANCEL).
Transition mapping_intro(std::function<Transition(Ctx&)> go) {
  ui::ConfirmOpts o;
  o.yes = tr("OK");
  o.no = tr("CANCEL");
  o.default_yes = true;
  return ui::confirm(tr("YOU ARE GOING TO MAP A CONTROLLER. MAP BASED ON THE BUTTON'S POSITION, NOT ITS PHYSICAL LABEL. IF YOU DO NOT HAVE A SPECIAL "
                        "BUTTON FOR HOTKEY, USE THE SELECT BUTTON. SKIP ALL BUTTONS/STICKS YOU DO NOT HAVE BY HOLDING ANY BUTTON. PRESS THE SOUTH BUTTON "
                        "TO CONFIRM WHEN DONE."),
                     std::move(go), std::move(o));
}

// ES PLAYER ASSIGNMENTS, "P<n>'S CONTROLLER" for the 8 players; the list is "default" and the configured pads.
// The choice is es_settings.cfg `INPUT P<n>NAME/GUID/PATH`; the launch reads it through players::order.
std::unique_ptr<Screen> make_players() {
  auto st = std::make_shared<PageState>();
  MenuSpec s;
  s.title = tr("ASSIGN CONTROLLERS TO PLAYERS");
  s.title_key = N_("ASSIGN CONTROLLERS TO PLAYERS");
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    if (!st->loaded) {
      st->loaded = true;
      st->joys = connected_pads(cx);
      st->configured.clear();
      for (const JoyInfo& j : st->joys) st->configured.push_back(pad_is_configured(j));
    }
    std::vector<JoyInfo> pads;
    for (size_t i = 0; i < st->joys.size(); ++i)
      if (i < st->configured.size() && st->configured[i]) pads.push_back(st->joys[i]);
    std::vector<Option> opts = {{"", tr("default")}};
    for (size_t i = 0; i < pads.size(); ++i) opts.push_back({std::to_string(i), "#" + std::to_string(pads[i].index) + " " + pad_label(pads[i].name)});
    for (size_t p = 0; p < players::kSlots; ++p) {
      std::string label = tr("P%i'S CONTROLLER");
      const size_t at = label.find("%i");
      if (at != std::string::npos) label.replace(at, 2, std::to_string(p + 1));
      m.choice(label, opts,
               [&cx, pads, p] {
                 const int c = players::chosen(pads, players::read(cx.es))[p];
                 return c < 0 ? std::string() : std::to_string(c);
               },
               [&cx, pads, p](const std::string& v) {
                 const long k = v.empty() ? -1 : std::strtol(v.c_str(), nullptr, 10);
                 if (!players::assign(cx.es, p, k >= 0 && size_t(k) < pads.size() ? &pads[size_t(k)] : nullptr)) return;
                 std::string err;
                 if (!cx.es.save(&err)) cx.sv->toasts.push(tr("Could not save the setting") + std::string(": ") + err, 3000);
                 players::set(players::read(cx.es));  // the next launch uses it
               })
          .no_cycle();
    }
  };
  return make_menu(std::move(s));
}

std::unique_ptr<Screen> make_page() {
  auto st = std::make_shared<PageState>();
  MenuSpec s;
  s.title = tr("CONTROLLER & BLUETOOTH SETTINGS");
  s.title_key = N_("CONTROLLER & BLUETOOTH SETTINGS");
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    if (!st->loaded) {  // once per opening: reading es_input.cfg is not for every refresh
      st->loaded = true;
      st->sig = pad_signature(cx);
      st->joys = connected_pads(cx);
      st->configured.clear();
      for (const JoyInfo& j : st->joys) st->configured.push_back(pad_is_configured(j));
    }
    Refresh rf = m.refresh;

    if (bt_available()) {
      m.header(tr("BLUETOOTH"));
      bool enabled = conf_bool(cx, "controllers.bluetooth.enabled");
      m.toggle(tr("ENABLE BLUETOOTH"), [&cx] { return conf_bool(cx, "controllers.bluetooth.enabled"); },
               [&cx, rf](bool on) {
                 conf_set(cx, "controllers.bluetooth.enabled", on);
                 auto rc = std::make_shared<int>(0);
                 // ES waits for these on the UI thread; here the entries below appear when the script is done
                 cx.sv->jobs.post_slow(
                     [sv = cx.sv, on, rc] {
                       *rc = sv->cmd.run(on ? "rocknix-bluetooth enable 2>&1 >/dev/null" : "rocknix-bluetooth disable", 30000).exit_code;
                     },
                     [&cx, rf, rc, on] {
                       if (*rc != 0 && on) cx.sv->toasts.push(tr("Could not start Bluetooth"), 3000);
                       rf();
                     });
               });
      if (enabled) {
        Entry& e = m.action(tr("PAIR BLUETOOTH PADS AUTOMATICALLY"), [&cx, rf] {
          if (auto_pair().active()) {
            cx.sv->toasts.push(tr("BLUETOOTH SCAN IS ALREADY RUNNING."));
            return;
          }
          auto_pair().until = Clock::now() + std::chrono::seconds(kAutoPairSeconds + 2);
          cx.sv->cmd.spawn("rocknix-bluetooth trust input");  // runs about a minute: the page is not blocked, nor the job thread
          cx.sv->toasts.push(tr("SCANNING BLUETOOTH") + std::string(": ") + tr("Searching for devices..."), 4000);
          rf();
        });
        if (auto_pair().active()) e.value(tr("Searching..."));
        m.action(tr("PAIR A BLUETOOTH DEVICE MANUALLY"), [&cx]() -> Transition {
          if (auto_pair().active()) {
            cx.sv->toasts.push(tr("BLUETOOTH SCAN IS ALREADY RUNNING."));
            return Transition::none();
          }
          return Transition::open("menu.controllers.btpair");
        }).show_arrow = true;
        m.submenu(tr("BLUETOOTH DEVICE LIST"), "menu.controllers.btdevices");
      }
    }

    m.header(tr("CONTROLLERS"));
    m.action(tr("CONTROLLER MAPPING"), [] { return mapping_intro([](Ctx&) { return Transition::open("menu.controllers.detect"); }); }).show_arrow = true;

    m.toggle(
         tr("SWITCH CONFIRM & CANCEL BUTTONS"), [&cx] { return cx.es.get_bool("InvertButtons", true); },
         [&cx](bool on) {
           es_set(cx, "InvertButtons", on);
           if (cx.pad) cx.pad->set_invert_buttons(on);
         })
        .hint(tr("Switches the South and East buttons' functionality"));
    std::string rumble = rumble_path();
    if (!rumble.empty()) {
      m.toggle(tr("ENABLE RUMBLE"), [&cx] { return cx.conf.get_or("rumble.enabled", "") == "1"; },
               [&cx, rumble](bool b) {
                 cx.sv->cmd.run(std::string("echo ") + (b ? "1" : "0") + " >" + shell_quote(rumble), 3000);
                 cx.conf.persist("rumble.enabled", b ? "1" : "0");
               });
    }
    m.toggle(tr("AUTOCONFIGURE RETROARCH HOTKEYS"), [&cx] { return conf_bool(cx, "system.autohotkeys"); },
             [&cx](bool b) { conf_set(cx, "system.autohotkeys", b); });
    m.toggle(tr("SHOW CONTROLLER NOTIFICATIONS"), [&cx] { return cx.es.get_bool("ShowControllerNotifications", true); },
             [&cx](bool b) { es_set(cx, "ShowControllerNotifications", b); });
    // ES also has SHOW CONTROLLER ACTIVITY / BATTERY LEVEL (ShowControllerActivity / ShowControllerBattery): zfront draws neither,
    // so the rows are not offered (a switch that changes nothing)

    if (!st->joys.empty()) {
      m.header(tr("CONNECTED CONTROLLERS"));
      for (size_t i = 0; i < st->joys.size(); ++i) {
        const JoyInfo j = st->joys[i];
        std::string state = i < st->configured.size() && st->configured[i] ? std::string() : tr("Not configured");
        m.action(j.name.empty() ? tr("Unknown controller") : pad_label(j.name),
                 [j] {
                   return mapping_intro([j](Ctx&) { return Transition::open("menu.controllers.config", j.guid + " " + j.name); });
                 })
            .value(state)
            .with_id("pad:" + std::to_string(i) + ":" + j.guid)
            .show_arrow = true;
      }
    }

    // P1..P8 are eight rows: they live in their own screen
    m.submenu(tr("ASSIGN CONTROLLERS TO PLAYERS"), "menu.controllers.players");
  };
  CtlMenu::Hooks h;
  // the "Searching..." mark must go away when the minute is over; a pad that pairs, or a wizard that saves, shows in the list
  h.next_poll = []() -> std::optional<float> {
    float next = 2.0f;
    if (auto_pair().active()) next = std::min(next, std::chrono::duration<float>(auto_pair().until - Clock::now()).count() + 0.05f);
    return next;
  };
  h.poll = [st](Ctx& cx, const Refresh& rf) {
    const bool pairing = auto_pair().active();
    const std::string sig = pad_signature(cx);
    if (sig != st->sig) st->loaded = false;  // the next build reads the pads again
    if (sig != st->sig || pairing != st->pairing) {
      st->pairing = pairing;
      rf();
    }
  };
  return std::make_unique<CtlMenu>(std::move(s), std::move(h));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.controllers", [](Ctx&, const std::string&) { return make_page(); });
ZF_REGISTER_SCREEN("menu.controllers.players", [](Ctx&, const std::string&) { return make_players(); });

}  // namespace zf::ctl
