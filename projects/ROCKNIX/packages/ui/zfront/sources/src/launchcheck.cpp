#include "launchcheck.hpp"

#include "i18n.hpp"
#include "menu_game.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf::launchcheck {

bool bios_missing(std::string_view out, std::string_view system) {
  bool in_system = false;
  size_t i = 0;
  while (i < out.size()) {
    size_t e = out.find('\n', i);
    std::string_view line = out.substr(i, e == std::string_view::npos ? std::string_view::npos : e - i);
    i = e == std::string_view::npos ? out.size() : e + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (line.size() >= 2 && line.substr(0, 2) == "> ") {
      in_system = line.substr(2) == system;
      continue;
    }
    if (in_system && line.substr(0, 8) == "MISSING ") return true;
  }
  return false;
}

bool bios_ok(Ctx& cx, size_t sys) {
  if (sys >= cx.systems.size()) return true;
  if (!cx.es.get_bool("CheckBiosesAtLaunch", false) || !mg::bin_exists("rocknix-systems")) return true;
  const std::string name = cx.systems[sys].name;
  std::string out;
  if (cx.sv->cmd.dry_run) {  // tests and the preview harness do not run the script; a sample output can stand in (as in the BIOS menu)
    read_file(mg::env().rooted("/tmp/rocknix-systems.out"), out);
  } else {
    out = cx.sv->cmd.run("rocknix-systems --filter " + shell_quote(name), 60000).out;
  }
  return !bios_missing(out, name);
}

Transition ask(std::function<Transition(Ctx&)> go) {
  return ui::confirm(tr("WARNING: THE SYSTEM HAS MISSING BIOS FILE(S) AND THE GAME MAY NOT WORK CORRECTLY.\nDO YOU WANT TO LAUNCH IT ANYWAY?"), std::move(go));
}

Transition guard(Ctx& cx, size_t sys, Transition launch) {
  if (launch.kind != Transition::Launch || bios_ok(cx, sys)) return launch;
  std::string cmd = std::move(launch.cmd);
  const int s = launch.sys, g = launch.game;
  return ask([cmd, s, g](Ctx&) {
    Transition t = Transition::launch(cmd);
    t.sys = s;
    t.game = g;
    return t;
  });
}

}  // namespace zf::launchcheck
