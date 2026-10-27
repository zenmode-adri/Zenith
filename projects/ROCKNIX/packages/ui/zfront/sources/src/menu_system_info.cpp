// `menu.system.info` (ES: GuiSystemInformation, ROCKNIX build = `rocknix-info --full`) and `menu.system.reset` (ES: "SYSTEM
// MANAGEMENT AND RESET", GuiMenu::openResetOptions). Same scripts and the same warning texts as ES; every reset asks first.
#include <cstdio>
#include <memory>

#include "menu.hpp"
#include "menu_system.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf {
namespace {

using namespace msys;

struct InfoRow {
  std::string label, value;
  bool header = false;
};

struct InfoData {
  std::vector<InfoRow> rows;
  bool loaded = false;
};

// ES: "LABEL:" alone is a group title, "LABEL: value" a row (the value keeps its own colons: "IP ADDRESS: fe80::1").
std::vector<InfoRow> parse_info(const std::string& out) {
  std::vector<InfoRow> rows;
  size_t i = 0;
  while (i <= out.size()) {
    size_t e = out.find('\n', i);
    if (e == std::string::npos) e = out.size();
    std::string_view line = trim(std::string_view(out).substr(i, e - i));
    i = e + 1;
    if (line.empty()) {
      if (e >= out.size()) break;
      continue;
    }
    size_t c = line.find(':');
    InfoRow r;
    if (c == std::string_view::npos) {
      r.label = std::string(line);
      r.header = true;
    } else {
      r.label = std::string(trim(line.substr(0, c)));
      r.value = std::string(trim(line.substr(c + 1)));
      r.header = r.value.empty();
    }
    rows.push_back(std::move(r));
    if (e >= out.size()) break;
  }
  return rows;
}

std::string uptime_text_now() {
  std::string up = read_line("/proc/uptime");
  long secs = 0;
  size_t sp = up.find_first_of(" .");
  if (sp == std::string::npos || !parse_int(up.substr(0, sp), secs)) return {};
  long d = secs / 86400, h = secs % 86400 / 3600, m = secs % 3600 / 60;
  char buf[48];
  if (d > 0)
    std::snprintf(buf, sizeof buf, "%ldd %ld:%02ld", d, h, m);
  else
    std::snprintf(buf, sizeof buf, "%ld:%02ld", h, m);
  return buf;
}

std::unique_ptr<Screen> make_info() {
  auto data = std::make_shared<InfoData>();
  MenuSpec s;
  s.title = tr("INFORMATION");
  s.title_key = N_("INFORMATION");
  s.build = [data](MenuBuilder& m, Ctx&) {
    if (!data->loaded) {
      m.info(tr("Loading..."));
      return;
    }
    bool uptime_done = false;
    std::string up = uptime_text_now();
    int groups = 0;
    for (const InfoRow& r : data->rows) {
      if (r.header) {
        // the uptime closes the first group (SYSTEM INFORMATION)
        if (groups == 1 && !uptime_done && !up.empty()) m.info(tr("UPTIME"), up), uptime_done = true;
        m.header(tr(r.label.c_str()));
        ++groups;
      } else {
        if (groups == 0) m.header(tr("SYSTEM")), groups = 1;
        m.info(tr(r.label.c_str()), r.value);
      }
    }
    if (!uptime_done && !up.empty()) m.info(tr("UPTIME"), up);
    if (data->rows.empty()) m.info(tr("No information available"));
  };
  s.on_open = [data](Ctx& cx, const Refresh& refresh) {
    auto sv = cx.sv;
    auto rows = std::make_shared<std::vector<InfoRow>>();
    post(cx, [sv, rows] { *rows = parse_info(query(*sv, "rocknix-info --full", 20000).out); },
                     [data, rows, refresh] {
                       data->rows = std::move(*rows);
                       data->loaded = true;
                       if (refresh) refresh();
                     });
  };
  return make_menu(std::move(s));
}

// ---- reset ------------------------------------------------------------------------------------------------------------

// ES: `Utils::Platform::runSystemCommand("/usr/bin/run \"<cmd>\"")`; `run` shows the command in a terminal over the UI.
Act run_script(const std::string& script) {
  return [script](Ctx& cx) {
    cx.sv->cmd.spawn("/usr/bin/run \"" + script + "\"");
    return Transition::none();
  };
}

std::unique_ptr<Screen> make_reset() {
  MenuSpec s;
  s.title = tr("SYSTEM MANAGEMENT AND RESET");
  s.title_key = N_("SYSTEM MANAGEMENT AND RESET");
  s.build = [](MenuBuilder& m, Ctx&) {
    auto ask = [&m](const char* label, const char* msg, const std::string& script, bool danger) {
      Entry& e = m.action(tr(label), [msg, script, danger]() {
        ui::ConfirmOpts o;
        o.danger = danger;
        return ui::confirm(fit_message(tr(msg)), run_script(script), o);
      });
      if (danger) e.danger();
    };

    m.header(tr("DATA MANAGEMENT"));
    ask(N_("BACKUP CONFIGURATIONS"),
        N_("WARNING THIS WILL RESTART THE FRONTEND!\n\nAFTER THE SCRIPT IS DONE REMEMBER TO COPY THE FILE /storage/roms/backup/ROCKNIX_BACKUP.zip TO "
        "SOME PLACE SAFE OR IT WILL BE DELETED ON NEXT REBOOT!\n\nBACKUP CURRENT CONFIG AND RESTART?"),
        "/usr/bin/backuptool backup", false);
    ask(N_("RESTORE FROM BACKUP"), N_("WARNING THIS WILL REBOOT YOUR DEVICE!\n\nYOUR EXISTING CONFIGURATION WILL BE OVERWRITTEN!\n\nRESTORE FROM BACKUP AND RESTART?"),
        "/usr/bin/backuptool restore", true);

    m.header(tr("EMULATOR MANAGEMENT"));
    ask(N_("RESET RETROARCH CONFIG TO DEFAULT"),
        N_("WARNING: RETROARCH CONFIG WILL RESET TO DEFAULT\n\nPER-CORE CONFIGURATIONS WILL NOT BE AFFECTED AND NO BACKUP WILL BE CREATED!\n\nRESET RETROARCH "
        "CONFIG TO DEFAULT?"),
        "/usr/bin/factoryreset retroarch", true);
    ask(N_("RESET OVERLAYS (CORES, CHEATS, JOYPADS, ETC)"),
        N_("WARNING: ALL CUSTOM RETROARCH OVERLAYS WILL BE REMOVED\n\nCUSTOM CORES, JOYSTICKS, CHEATS, ETC. NO BACKUP WILL BE CREATED!\n\nRESET RETROARCH OVERLAYS TO "
        "DEFAULT?"),
        "/usr/bin/factoryreset overlays", true);
    ask(N_("FULLY RESET RETROARCH"),
        N_("WARNING: RETROARCH AND ALL USER SAVED CONFIGURATIONS WILL RESET TO DEFAULT\n\nPER-CORE CONFIGURATIONS WILL BE REMOVED AND NO BACKUP WILL BE "
        "CREATED!\n\nRESET RETROARCH?"),
        "/usr/bin/factoryreset retroarch-full && /usr/bin/factoryreset overlays", true);
    ask(N_("RESET MEDNAFEN CONFIG TO DEFAULT"),
        N_("WARNING: MEDNAFEN CONFIG WILL RESET TO DEFAULT\n\nNO BACKUP WILL BE CREATED!\n\nRESET MEDNAFEN CONFIG TO DEFAULT?"), "/usr/bin/factoryreset mednafen", true);
    ask(N_("RESET STANDALONE EMULATOR CONFIGS TO DEFAULT"),
        N_("WARNING: STANDALONE EMULATOR CONFIGS WILL RESET TO DEFAULT\n\nNO BACKUP WILL BE CREATED!\n\nRESET STANDALONE EMULATOR CONFIGS TO DEFAULT?"),
        "/usr/bin/factoryreset standalone", true);
    ask(N_("FULLY RESET PORTMASTER"), N_("WARNING: PORTMASTER WILL RESET TO DEFAULT\n\nNO BACKUP WILL BE CREATED!\n\nRESET RESET PORTMASTER TO DEFAULT?"),
        "/usr/bin/factoryreset portmaster", true);

    m.header(tr("SYSTEM MANAGEMENT"));
    ask(N_("AUDIO RESET"), N_("WARNING: AUDIO SETTINGS WILL BE RESET TO DEFAULTS AND THE SYSTEM WILL REBOOT!\n\nRESET AUDIO AND RESTART?"), "/usr/bin/factoryreset audio", true);
    // ES: DEVICE_RK817_FUEL_GAUGE is written to /storage/.config/profile.d by the platform quirk when the PMIC is present
    if (devflag("DEVICE_RK817_FUEL_GAUGE") || read_all("/storage/.config/profile.d/015-rk817_config").find("DEVICE_RK817_FUEL_GAUGE=\"true\"") != std::string::npos)
      ask(N_("RESET BATTERY FUEL GAUGE"),
          N_("WARNING: THE PMIC FUEL GAUGE WILL BE RESET AND THE SYSTEM WILL POWER OFF!\n\nLEAVE THE CHARGER UNPLUGGED AND POWER THE DEVICE BACK ON AFTER A FEW "
          "SECONDS. THE BATTERY LEVEL IS RE-ESTIMATED FROM THE RESTING BATTERY VOLTAGE AND BECOMES ACCURATE AGAIN AFTER A FULL CHARGE.\n\nRESET FUEL GAUGE AND POWER OFF?"),
          "/usr/bin/factoryreset rk817-fuelgauge", true);
    ask(N_("FACTORY RESET"),
        N_("WARNING: YOUR DATA AND ALL OTHER CONFIGURATIONS WILL BE RESET TO DEFAULTS!\n\nIF YOU WANT TO KEEP YOUR SETTINGS MAKE A BACKUP AND SAVE IT ON AN EXTERNAL "
        "DRIVE BEFORE RUNING THIS OPTION!\n\nEJECT YOUR GAME CARD BEFORE PROCEEDING!\n\nRESET SYSTEM AND RESTART?"),
        "/usr/bin/factoryreset ALL", true);
  };
  return make_menu(std::move(s));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.system.info", [](Ctx&, const std::string&) { return make_info(); });
ZF_REGISTER_SCREEN("menu.system.reset", [](Ctx&, const std::string&) { return make_reset(); });

}  // namespace zf
