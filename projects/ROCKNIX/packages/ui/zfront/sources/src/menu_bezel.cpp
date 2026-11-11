// `menu.bezelproject` (ES: GAME SETTINGS > UPDATES & DOWNLOADS > THE BEZEL PROJECT, GuiBezelInstaller). Commands and parsing: bezelproject.hpp.
//
// The list (`rocknix-es-thebezelproject list`) is read on the worker when the screen opens. An install takes minutes: it runs detached
// (`cmd.spawn`, status in files under /tmp, like the update download of `menu.updates`) and the open menu polls the files once a second to
// show the progress line. A remove is short and runs as a job. Only sets of systems zfront knows are listed (ES does the same).
#include <algorithm>
#include <cstdlib>
#include <memory>

#include "bezelproject.hpp"
#include "menu_game_int.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf::bezel {
namespace {

constexpr const char* kLog = "zf-bezel.log";
constexpr const char* kRc = "zf-bezel.rc";
constexpr const char* kRun = "zf-bezel.run";
constexpr const char* kName = "zf-bezel.name";

std::string tmpf(const char* n) {
  const char* b = std::getenv("ZF_BEZEL_TMP");
  return std::string(b && *b ? b : "/tmp") + "/" + n;
}
bool running() { return file_exists(tmpf(kRun)); }

std::string read_trim(const std::string& path) {
  std::string t;
  if (!read_file(path, t)) return {};
  return std::string(trim(t));
}

// Progress: the last line of the log (the script prints `[snes] Installing bezels >>> 42%`)
std::string progress() {
  std::string t;
  if (!read_file(tmpf(kLog), t)) return {};
  std::string l = last_line(t);
  size_t cr = l.find_last_of('\r');
  return cr == std::string::npos ? l : l.substr(cr + 1);
}

struct State {
  enum Phase { Idle, Loading, Done, Failed } phase = Idle;
  std::vector<Set> sets;
  bool was_running = false;
  bool busy = false;  // a remove is running
};

void start_list(Ctx& cx, const std::shared_ptr<State>& st, const Refresh& rf) {
  st->phase = State::Loading;
  std::shared_ptr<Services> sv = cx.sv;
  auto res = std::make_shared<std::vector<Set>>();
  auto ok = std::make_shared<bool>(false);
  sv->jobs.post([sv, res, ok] {
    CmdResult r = sv->cmd.run(list_command(), 60000);
    if (r.dry) return;
    *res = parse_list(r.out);
    *ok = r.exit_code == 0;
  }, [st, res, ok, rf] {
    st->sets = *res;
    st->phase = *ok ? State::Done : State::Failed;
    if (rf) rf();
  });
}

void start_install(Ctx& cx, const std::string& name) {
  const std::string rc = shell_quote(tmpf(kRc)), run = shell_quote(tmpf(kRun)), log = shell_quote(tmpf(kLog)), nm = shell_quote(tmpf(kName));
  cx.sv->cmd.spawn("rm -f " + rc + " " + log + "; echo " + shell_quote(name) + " > " + nm + "; touch " + run + "; ( " + install_command(name) + " > " + log +
                   " 2>&1; echo $? > " + rc + "; rm -f " + run + " )");
}

class BezelMenu : public Menu {
 public:
  BezelMenu(MenuSpec s, std::shared_ptr<State> st) : Menu(std::move(s)), st_(std::move(st)) {}

  bool tick(float dt, Ctx& cx) override {
    const bool run = running();
    if (run) {
      std::string p = progress();
      if (p != last_) {
        last_ = p;
        refresher()();
      }
    }
    if (run != st_->was_running) {
      st_->was_running = run;
      if (!run) finish(cx);
      refresher()();
    }
    return Menu::tick(dt, cx);
  }
  std::optional<float> wake_in() const override {
    std::optional<float> base = Menu::wake_in();
    if (!running() && !st_->was_running) return base;
    return base ? std::min(*base, 1.0f) : 1.0f;
  }

 private:
  void finish(Ctx& cx) {
    const std::string name = read_trim(tmpf(kName));
    if (read_trim(tmpf(kRc)) == "0") {
      on_installed(cx, name);
      cx.sv->toasts.push(tr("BEZELS INSTALLED SUCCESSFULLY"), 3000);
    } else {
      std::string t;
      read_file(tmpf(kLog), t);
      const std::string l = last_line(t);
      cx.sv->toasts.push(std::string(tr("AN ERROR OCCURRED")) + (l.empty() ? "" : ": " + l), 6000);
    }
    start_list(cx, st_, refresher());  // the sets change: ask again
  }
  std::shared_ptr<State> st_;
  std::string last_;
};

Transition set_popup(Ctx& cx, const std::shared_ptr<State>& st, const Set& s, Refresh rf) {
  std::vector<Option> o;
  o.push_back({"install", s.installed ? tr("UPDATE") : tr("INSTALL")});
  if (s.installed) o.push_back({"remove", tr("REMOVE")});
  const std::string name = s.name;
  Ctx* c = &cx;
  return ui::choose(name, std::move(o), "", [c, st, name, rf](Ctx&, const std::string& v) {
    if (!valid_name(name)) return;
    if (v == "install") {
      start_install(*c, name);
      if (rf) rf();
    } else if (v == "remove") {
      st->busy = true;
      std::shared_ptr<Services> sv = c->sv;
      auto ok = std::make_shared<bool>(false);
      sv->jobs.post([sv, name, ok] { *ok = sv->cmd.run(remove_command(name), 120000).exit_code == 0; }, [c, st, name, ok, rf] {
        st->busy = false;
        if (*ok) {
          on_removed(*c, name);
          c->sv->toasts.push(tr("BEZELS UNINSTALLED SUCCESSFULLY"), 3000);
        } else {
          c->sv->toasts.push(tr("AN ERROR OCCURRED"), 4000);
        }
        start_list(*c, st, rf);
      });
      if (rf) rf();
    }
  });
}

std::unique_ptr<Screen> make_bezel() {
  auto st = std::make_shared<State>();
  st->was_running = running();
  MenuSpec s;
  s.title = tr("THE BEZEL PROJECT");
  s.title_key = N_("THE BEZEL PROJECT");
  s.on_open = [st](Ctx& cx, const Refresh& rf) {
    if (st->phase == State::Idle) start_list(cx, st, rf);
  };
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    Ctx* c = &cx;
    Refresh rf = m.refresh;
    m.info(tr("SELECT BEZELS TO INSTALL/REMOVE"));
    if (running()) {
      const std::string p = progress(), n = read_trim(tmpf(kName));
      m.info(n.empty() ? std::string(tr("CURRENTLY IN DOWNLOAD QUEUE")) : n, p.empty() ? std::string("...") : p);
    }
    if (st->phase == State::Idle || st->phase == State::Loading || st->busy) {  // Idle: the first build comes before on_open asks
      m.info(tr("PLEASE WAIT"));
      return;
    }
    if (st->phase == State::Failed && st->sets.empty()) {
      m.info(tr("AN ERROR OCCURRED"));
      return;
    }
    bool any = false;
    for (const Set& set : st->sets) {
      const System* sys = nullptr;
      for (const System& x : cx.systems)
        if (x.name == set.name) sys = &x;
      if (!sys) continue;
      any = true;
      Set copy = set;
      Entry& e = m.action(sys->fullname, [c, st, copy, rf] { return set_popup(*c, st, copy, rf); });
      e.value(set.installed ? std::string(tr("INSTALLED")) : std::string()).disabled(running());
    }
    if (!any) m.info(tr("NO GAMES FOUND"));
  };
  return std::make_unique<BezelMenu>(std::move(s), st);
}

}  // namespace

ZF_REGISTER_SCREEN("menu.bezelproject", [](Ctx&, const std::string&) { return make_bezel(); });

}  // namespace zf::bezel
