// `menu.moonlight` (ES: GAME SETTINGS > TOOLS > MOONLIGHT GAME STREAMING, GuiMoonlight). The commands and files are in moonlight.hpp.
//
// Threads: QUIT and UPDATE are short jobs on the worker. PAIR waits for the PIN to be typed on the server (minutes), so it must not hold the
// single worker: it runs as a detached process (`cmd.spawn`) with its output and exit status in files under /tmp, which the open menu polls
// once a second, like the update download of `menu.updates`. The PIN is made when the screen opens (ES: when GuiMoonlight is created).
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <random>

#include "collections.hpp"
#include "menu_game_int.hpp"
#include "moonlight.hpp"
#include "registry.hpp"
#include "util.hpp"

namespace zf::moonlight {
namespace {

constexpr const char* kLog = "zf-moonlight.log";
constexpr const char* kRc = "zf-moonlight.rc";
constexpr const char* kRun = "zf-moonlight.run";

std::string tmpf(const char* n) {
  const char* b = std::getenv("ZF_ML_TMP");
  return std::string(b && *b ? b : "/tmp") + "/" + n;
}
bool running() { return file_exists(tmpf(kRun)); }

struct State {
  std::string pin;
  bool checking = false;  // `moonlight -v` is being read before the pairing starts
  bool updating = false;
  bool was_running = false;
};

std::string new_pin() {
  std::mt19937 rng(unsigned(std::time(nullptr)) ^ (unsigned(::getpid()) << 8));
  char b[16];
  std::snprintf(b, sizeof b, "%04d", int(rng() % 10000));
  return b;
}

std::string host_of(Ctx& cx) { return mg::conf_get(cx, "moonlight.host"); }

void start_pair(Ctx& cx, const std::shared_ptr<State>& st, const Refresh& rf) {
  const std::string host = host_of(cx);
  if (!valid_host(host)) {
    cx.sv->toasts.push(tr("Unable to connect to server"), 3000);
    return;
  }
  st->checking = true;
  std::shared_ptr<Services> sv = cx.sv;
  auto embedded = std::make_shared<bool>(false);
  sv->jobs.post([sv, embedded] { *embedded = is_embedded(sv->cmd.run("moonlight -v 2>&1", 5000).out); },
                [sv, st, embedded, host, rf] {
                  st->checking = false;
                  const std::string cmd = pair_command(st->pin, host, *embedded);
                  // status files first (a stale exit status would be read as the result), then the pairing, detached from us
                  const std::string rc = shell_quote(tmpf(kRc)), run = shell_quote(tmpf(kRun)), log = shell_quote(tmpf(kLog));
                  sv->cmd.spawn("rm -f " + rc + " " + log + "; touch " + run + "; ( " + cmd + " > " + log + " 2>&1; echo $? > " + rc + "; rm -f " + run + " )");
                  if (rf) rf();
                });
  if (rf) rf();
}

std::string last_line(const std::string& text) {
  size_t e = text.find_last_not_of(" \r\n");
  if (e == std::string::npos) return {};
  size_t b = text.find_last_of('\n', e);
  return text.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b + 1) + 1);
}

class MoonlightMenu : public Menu {
 public:
  MoonlightMenu(MenuSpec s, std::shared_ptr<State> st) : Menu(std::move(s)), st_(std::move(st)) {}

  bool tick(float dt, Ctx& cx) override {
    const bool run = running();
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
  // the pairing ended: ES's per-line callback, once, on the whole output
  void finish(Ctx& cx) {
    std::string log;
    read_file(tmpf(kLog), log);
    size_t i = 0;
    while (i < log.size()) {
      size_t e = log.find('\n', i);
      std::string line = log.substr(i, e == std::string::npos ? std::string::npos : e - i);
      i = e == std::string::npos ? log.size() : e + 1;
      std::string ip;
      if (parse_server_ip(line, &ip) && valid_host(ip) && ip != host_of(cx)) mg::conf_put(cx, "moonlight.host", ip);
    }
    extract_certs();
    if (paired_ok(log)) cx.sv->toasts.push(tr("Succesfully paired with server"), 4000);
    else cx.sv->toasts.push(std::string(tr("AN ERROR OCCURRED")) + ": " + last_line(log), 5000);
  }
  std::shared_ptr<State> st_;
};

std::unique_ptr<Screen> make_moonlight() {
  auto st = std::make_shared<State>();
  st->pin = new_pin();
  st->was_running = running();
  MenuSpec s;
  s.title = tr("MOONLIGHT GAME STREAMING");
  s.title_key = N_("MOONLIGHT GAME STREAMING");
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    Ctx* c = &cx;
    Refresh rf = m.refresh;
    m.header(tr("TOOLS"));
    m.action(tr("QUIT CURRENT GAME"), [c] {
      const std::string host = host_of(*c);
      if (!valid_host(host)) {
        c->sv->toasts.push(tr("Unable to connect to server"), 3000);
        return;
      }
      std::shared_ptr<Services> sv = c->sv;
      auto rc = std::make_shared<int>(0);
      sv->jobs.post([sv, host, rc] { *rc = sv->cmd.run(quit_command(host), 15000).exit_code; },
                    [sv, rc] {
                      if (*rc != 0) sv->toasts.push(tr("Unable to connect to server"), 3000);
                    });
    });
    m.action(tr("UPDATE MOONLIGHT GAMES"), [c, st, rf] {
      const std::string host = host_of(*c);
      if (st->updating) return;
      if (!valid_host(host)) {
        c->sv->toasts.push(tr("Unable to connect to server"), 3000);
        return;
      }
      st->updating = true;
      std::shared_ptr<Services> sv = c->sv;
      auto res = std::make_shared<UpdateResult>();
      sv->jobs.post([sv, host, res] { *res = update_games(*sv, host); },
                    [sv, st, res, rf] {
                      st->updating = false;
                      if (res->ok) {
                        collections::request_rescan();  // the lists are read again when the menus close (ES restarts itself)
                        sv->toasts.push(tr("Game lists updated"), 3000);
                      } else {
                        sv->toasts.push(tr("Unable to connect to server"), 3000);
                      }
                      if (rf) rf();
                    });
      if (rf) rf();
    }).disabled(st->updating);
    if (running() || st->checking) m.info(tr("PAIR WITH SERVER"), tr("PLEASE WAIT"));
    else m.action(tr("PAIR WITH SERVER"), [c, st, rf] { start_pair(*c, st, rf); });
    m.action(tr("UNPAIR WITH SERVER"), [c] {
      const bool qt = qt_build();
      unpair();
      c->sv->toasts.push(qt ? tr("Unpaired and settings cleared") : tr("Unpaired from server"), 3000);
    });

    m.header(tr("SETTINGS"));
    mg::detail::conf_text(m, cx, tr("SERVER IP"), "moonlight.host");
    m.info(tr("PAIRING PIN"), st->pin);
  };
  return std::make_unique<MoonlightMenu>(std::move(s), st);
}

}  // namespace

ZF_REGISTER_SCREEN("menu.moonlight", [](Ctx&, const std::string&) { return make_moonlight(); });

}  // namespace zf::moonlight
