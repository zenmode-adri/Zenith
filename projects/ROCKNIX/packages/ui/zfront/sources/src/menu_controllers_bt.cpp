// Bluetooth screens: live scan + pair (ES GuiBluetoothPair), the paired list (GuiBluetoothDevices) and the per-device
// actions (GuiBluetoothDeviceOptions). Everything goes through `rocknix-bluetooth`, exactly the commands ES uses.
//
// The scan is `rocknix-bluetooth live_devices`, which stays alive until `stop_live_devices`: it is started with `spawn`
// (never as a job: the single job thread would be blocked and the stop command could not run) and its output goes to a
// log file the screen reads once a second while it is open. The stop command is sent from the screen's destructor.
#include "menu_controllers.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf::ctl {

namespace {

const char* icon_for(const std::string& type) {
  if (type == "joystick") return "controller";
  if (type == "audio") return "sound";
  return "";
}

// "ARE YOU SURE YOU WANT TO CONNECT TO '%s' ?" -> with the name in
std::string with_name(const char* key, const std::string& name) {
  std::string s = tr(key);
  size_t p = s.find("%s");
  if (p != std::string::npos) s.replace(p, 2, name);
  return s;
}

// ---- paired list (shared with the per-device screen, which asks it to reload) ----------------------------------------
struct ListState {
  std::shared_ptr<Services> sv;
  Refresh rf;
  std::vector<BtDevice> devs;
  bool loaded = false, loading = false;

  void reload() {
    if (loading) return;
    loading = true;
    auto out = std::make_shared<std::string>();
    sv->jobs.post([sv = sv, out] { *out = sv->cmd.run("rocknix-bluetooth list", 30000).out; },
                  [self = weak_self.lock(), out] {
                    if (!self) return;
                    self->devs = parse_bt_list(*out);
                    self->loaded = true;
                    self->loading = false;
                    if (self->rf) self->rf();
                  });
  }
  std::weak_ptr<ListState> weak_self;
};
std::weak_ptr<ListState> g_list;  // the list screen that is open (below the device screen), if any

// ---- pairing --------------------------------------------------------------------------------------------------------
struct PairState {
  std::shared_ptr<Services> sv;
  std::vector<BtDevice> devs;
  std::string err, last_text;
  bool pairing = false, close = false;
};

std::unique_ptr<Screen> make_pair(Ctx& cx) {
  auto st = std::make_shared<PairState>();
  st->sv = cx.sv;
  const std::string log = paths().bt_live_log;
  MenuSpec s;
  s.title = tr("PAIR A BLUETOOTH DEVICE");
  s.title_key = N_("PAIR A BLUETOOTH DEVICE");
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    Refresh rf = m.refresh;
    if (!st->err.empty()) m.info(st->err);
    if (st->devs.empty() && st->err.empty()) m.info(tr("SCANNING BLUETOOTH"));
    for (const BtDevice& d : st->devs) {
      BtDevice dev = d;
      m.action(d.name.empty() ? d.id : d.name,
               [&cx, st, dev, rf]() {
                 if (st->pairing) return;
                 if (!valid_mac(dev.id)) return;
                 st->pairing = true;
                 cx.sv->toasts.push(tr("PLEASE WAIT"), 3000);
                 auto ok = std::make_shared<bool>(false);
                 // trust loops up to 60 s (it ends as soon as the device shows as trusted): the user is waiting anyway
                 cx.sv->jobs.post(
                     [sv = cx.sv, dev, ok] {
                       sv->cmd.run("rocknix-bluetooth trust " + shell_quote(dev.id), 75000);
                       for (const BtDevice& t : parse_bt_list(sv->cmd.run("rocknix-bluetooth list", 30000).out))
                         if (t.id == dev.id) *ok = true;
                     },
                     [&cx, st, dev, ok, rf] {
                       st->pairing = false;
                       if (*ok) {
                         cx.sv->toasts.push(dev.name + std::string(": ") + tr("PAIRED"), 3000);
                         st->close = true;  // ES closes the screen when pairing worked
                       } else {
                         cx.sv->toasts.push(tr("Could not pair") + std::string(" ") + dev.name, 4000);
                       }
                       rf();
                     });
               })
          .value(d.id)
          .icon(icon_for(d.type))
          .with_id(d.id);
    }
  };
  s.on_open = [st, log](Ctx& cx, const Refresh&) {
    cx.sv->cmd.spawn("rocknix-bluetooth live_devices >" + shell_quote(log) + " 2>&1");
  };
  CtlMenu::Hooks h;
  h.next_poll = []() -> std::optional<float> { return 1.0f; };
  h.poll = [st, log](Ctx&, const Refresh& rf) {
    std::string text;
    if (!read_file(log, text) || text == st->last_text) return;
    st->last_text = text;
    st->devs.clear();
    apply_bt_live(st->devs, text);
    st->err.clear();
    size_t pos = 0;
    while (pos < text.size()) {  // the script's own complaint ("ERROR: Bluetooth agent is not running")
      size_t e = text.find('\n', pos);
      if (e == std::string::npos) e = text.size();
      std::string_view line = trim(std::string_view(text).substr(pos, e - pos));
      if (starts_with(line, "ERROR:")) st->err = std::string(line);
      pos = e + 1;
    }
    rf();
  };
  h.want_pop = [st] {
    bool c = st->close;
    st->close = false;
    return c;
  };
  h.on_destroy = [sv = cx.sv, log] { sv->cmd.spawn("rocknix-bluetooth stop_live_devices; rm -f " + shell_quote(log)); };
  return std::make_unique<CtlMenu>(std::move(s), std::move(h));
}

// ---- paired devices -------------------------------------------------------------------------------------------------
std::unique_ptr<Screen> make_list(Ctx& cx) {
  auto st = std::make_shared<ListState>();
  st->sv = cx.sv;
  st->weak_self = st;
  MenuSpec s;
  s.title = tr("BLUETOOTH DEVICE LIST");
  s.title_key = N_("BLUETOOTH DEVICE LIST");
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    Refresh rf = m.refresh;
    st->rf = rf;
    if (!st->loaded) {
      m.info(tr("PLEASE WAIT"));
      return;
    }
    if (st->devs.empty()) {
      m.info(tr("NO BLUETOOTH DEVICES FOUND"));
      return;
    }
    for (const BtDevice& d : st->devs) {
      std::string arg = d.id + (d.connected ? " 1 " : " 0 ") + d.name;
      m.submenu(d.name.empty() ? d.id : d.name, "menu.controllers.btdevice", arg).icon(icon_for(d.type)).value(d.connected ? tr("Connected") : std::string()).with_id(d.id);
    }
    m.separator();
    m.action(tr("REMOVE ALL"),
             [&cx, st] {
               cx.sv->jobs.post([sv = cx.sv] { sv->cmd.run("rocknix-config forgetBT", 30000); },
                                [&cx, st] {
                                  cx.sv->toasts.push(tr("BLUETOOTH DEVICES HAVE BEEN DELETED."), 3000);
                                  st->devs.clear();
                                  st->loaded = false;
                                  st->reload();
                                  if (st->rf) st->rf();
                                });
             })
        .danger()
        .confirm(tr("Remove all Bluetooth devices?"));
  };
  s.on_open = [st](Ctx&, const Refresh& rf) {
    st->rf = rf;
    g_list = st;
    st->reload();
  };
  return std::make_unique<Menu>(std::move(s));
}

// ---- one device -----------------------------------------------------------------------------------------------------
struct DeviceState {
  std::string id, name;
  bool connected = false, busy = false, close = false;
};

std::unique_ptr<Screen> make_device(Ctx& cx, const std::string& arg) {
  auto st = std::make_shared<DeviceState>();
  size_t a = arg.find(' ');
  if (a == std::string::npos) return nullptr;
  st->id = arg.substr(0, a);
  size_t b = arg.find(' ', a + 1);
  if (b == std::string::npos || !valid_mac(st->id)) return nullptr;
  st->connected = arg.substr(a + 1, b - a - 1) == "1";
  st->name = arg.substr(b + 1);
  MenuSpec s;
  s.title = st->name.empty() ? st->id : st->name;

  // runs `verb <mac>` as a job; afterwards the list reloads and this screen closes (as ES does)
  auto run = [st](Ctx& cx, const std::string& verb, int timeout_ms, const char* fail) {
    if (st->busy) return;
    st->busy = true;
    cx.sv->toasts.push(tr("PLEASE WAIT"), 3000);
    auto rc = std::make_shared<int>(0);
    cx.sv->jobs.post([sv = cx.sv, st, verb, timeout_ms, rc] { *rc = sv->cmd.run("rocknix-bluetooth " + verb + " " + shell_quote(st->id), timeout_ms).exit_code; },
                     [&cx, st, rc, fail] {
                       st->busy = false;
                       if (*rc != 0) cx.sv->toasts.push(tr(fail) + std::string(" ") + st->name, 4000);
                       if (std::shared_ptr<ListState> l = g_list.lock()) l->reload();
                       st->close = true;
                     });
  };
  s.build = [st, run](MenuBuilder& m, Ctx&) {
    if (st->connected) {
      m.action(tr("DISCONNECT"), [st, run](Ctx& cx) { run(cx, "disconnect", 20000, N_("Could not disconnect")); })
          .confirm(with_name(N_("ARE YOU SURE YOU WANT TO DISCONNECT '%s' ?"), st->name));
    } else {
      m.action(tr("CONNECT"), [st, run](Ctx& cx) { run(cx, "connect", 45000, N_("Could not connect")); })
          .confirm(with_name(N_("ARE YOU SURE YOU WANT TO CONNECT TO '%s' ?"), st->name));
    }
    m.action(tr("FORGET"), [st, run](Ctx& cx) { run(cx, "remove", 30000, N_("Could not remove")); })
        .danger()
        .confirm(with_name(N_("ARE YOU SURE YOU WANT TO FORGET '%s' ?"), st->name));
  };
  CtlMenu::Hooks h;
  h.want_pop = [st] {
    bool c = st->close;
    st->close = false;
    return c;
  };
  (void)cx;
  return std::make_unique<CtlMenu>(std::move(s), std::move(h));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.controllers.btpair", [](Ctx& cx, const std::string&) { return make_pair(cx); });
ZF_REGISTER_SCREEN("menu.controllers.btdevices", [](Ctx& cx, const std::string&) { return make_list(cx); });
ZF_REGISTER_SCREEN("menu.controllers.btdevice", [](Ctx& cx, const std::string& arg) { return make_device(cx, arg); });

}  // namespace zf::ctl
