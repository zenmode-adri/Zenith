// `menu.updates` (ES: "UPDATES & DOWNLOADS", GuiMenu::openUpdatesSettings + GuiUpdate + ThreadedUpdater, ROCKNIX build).
//
// What ES offers here and how it maps:
//   SOFTWARE UPDATES   (needs /usr/bin/rocknix-update)
//     FORCE NEXT UPDATE  updates.force (read by rocknix-update)
//     START UPDATE       `rocknix-update check` (+ ping), ES's confirmation, then `rocknix-update` (downloads the OTA tar to
//                        /storage/.update and verifies its sha256; the initramfs applies it on the next boot)
//     APPLY UPDATE       the update is downloaded: ES quits ("RESTART" = restart the frontend, which does not apply anything);
//                        here it offers the reboot that really applies it
// Hidden: CHECK FOR UPDATES (`updates.enabled`) and UPDATE BRANCH (`updates.branch`): no script of this OS reads them, so they
// would do nothing; CONTENT DOWNLOADER / torrent / local media (Batocera tools, absent); THEMES (ES's own
// theme downloader; zfront has one theme).
//
// The download runs outside the process (`cmd.spawn`, status in files under /tmp): the single job worker must not be blocked for
// minutes. While the menu is open it polls the files once a second; closing it loses nothing (the state is rediscovered).
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "bezelproject.hpp"
#include "menu.hpp"
#include "menu_system.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "ui_prefs.hpp"
#include "util.hpp"

namespace zf {
namespace {

using namespace msys;

constexpr const char* kLog = "zf-update.log";
constexpr const char* kRc = "zf-update.rc";
constexpr const char* kRun = "zf-update.run";

std::string tmpf(const char* n) { return probe().tmp + "/" + n; }

// Last "line" of the log (curl's progress bar rewrites its line with \r), at most `max` bytes.
std::string log_tail(size_t max = 64) {
  std::string path = tmpf(kLog);
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || st.st_size <= 0) return {};
  std::string t;
  uint64_t n = uint64_t(st.st_size), take = std::min<uint64_t>(n, 1024);
  if (!read_file_range(path, n - take, size_t(take), t)) return {};
  size_t e = t.find_last_not_of(" \r\n");
  if (e == std::string::npos) return {};
  t.resize(e + 1);
  size_t b = t.find_last_of("\r\n");
  std::string last = b == std::string::npos ? t : t.substr(b + 1);
  last = std::string(trim(last));
  if (last.size() > max) last = last.substr(last.size() - max);
  return last;
}

bool is_running() { return file_exists(tmpf(kRun)); }

// curl's `--progress-bar` line ends in "NN.N%": the percentage, or -1 when the log has none yet (checking, verifying, ...).
int percent_of(const std::string& line) {
  size_t p = line.rfind('%');
  if (p == std::string::npos || p == 0) return -1;
  size_t b = p;
  while (b > 0 && (std::isdigit(static_cast<unsigned char>(line[b - 1])) || line[b - 1] == '.')) --b;
  if (b == p) return -1;
  int v = int(std::atof(line.substr(b, p - b).c_str()));
  return std::max(0, std::min(100, v));
}

// A downloaded and verified OTA waits in /storage/.update (rocknix-update removes both files when the checksum fails).
bool update_waiting() {
  std::vector<std::string> names = list_dir("/storage/.update");
  for (const std::string& n : names)
    if (ends_with(n, ".tar") && std::find(names.begin(), names.end(), n + ".sha256") != names.end()) return true;
  return false;
}

struct Upd {
  enum Phase { Idle, Checking, Available } phase = Idle;
  std::string version;  // of the update found ("" = unknown / forced)
  bool was_running = false;
  bool cancelled = false;  // the user stopped the download: the end of the job is not an error
  float cancel_wait = 0;  // seconds the kill has to work: a download still running after that was not stopped
  float starting = 0;  // seconds left to wait for the status files of a download that was just started
};

std::string fmt1(const char* fmt, const std::string& a) {
  char buf[512];
  std::snprintf(buf, sizeof buf, tr(fmt), a.c_str());
  return buf;
}

std::string confirm_text(const std::string& ver) {
  if (ver.empty()) return tr("REALLY UPDATE?");
  std::string build = devenv("OS_BUILD");
  if (build == "official" || build == "nightly") {
    char buf[768];
    std::snprintf(buf, sizeof buf, tr("YOU ARE CURRENTLY USING VERSION %s\nDO YOU WANT TO UPDATE TO VERSION %s?"), devenv("OS_VERSION").c_str(), ver.c_str());
    return buf;
  }
  return fmt1("UNOFFICIAL SYSTEM MODIFICATIONS DETECTED.\nUPGRADING COULD BREAK YOUR SYSTEM.\nDO YOU WANT TO UPDATE TO VERSION %s?", ver);
}

void start_download(Ctx& cx) {
  // status files first (a stale rc would be read as the result), then the update itself, detached from us
  std::string rc = shell_quote(tmpf(kRc)), run = shell_quote(tmpf(kRun)), log = shell_quote(tmpf(kLog));
  cx.sv->cmd.spawn("rm -f " + rc + " " + log + "; touch " + run + "; ( rocknix-update > " + log + " 2>&1; echo $? > " + rc + "; rm -f " + run + " )");
}

// Stops the download without leaving anything half done. The script has no cancel: it is killed (the script, then curl, which would
// outlive it), and what it leaves is removed the way its own failure path does (`rm` of the partial file, `clear_redirect_part`).
// A .tar WITH its .sha256 is a complete, verified update: it stays (init only applies those, and the menu offers the reboot).
void cancel_download(Ctx& cx) {
  std::string clean;
  for (const char* d : {"/storage/.update", "/storage/roms/.update"}) {
    std::string dir = d;
    clean += "for f in " + dir + "/*.tar; do [ -e \"$f\" ] && [ ! -e \"$f.sha256\" ] && rm -f \"$f\"; done; ";
  }
  clean += "rm -f /storage/.update/redirect_part; ";
  cx.sv->cmd.spawn("pkill -f '^/bin/sh /usr/bin/[r]ocknix-update'; pkill -f '[c]url --progress-bar'; sleep 1; " + clean);
}

class UpdatesMenu : public Menu {
 public:
  UpdatesMenu(MenuSpec s, std::shared_ptr<Upd> u) : Menu(std::move(s)), u_(std::move(u)) {}

  bool tick(float dt, Ctx& cx) override {
    if (u_->starting > 0) u_->starting -= dt;
    bool run = is_running();
    if (run && u_->cancelled && (u_->cancel_wait -= dt) <= 0) {  // pkill did not kill it: it is not a cancelled download any more
      u_->cancelled = false;
      cx.sv->toasts.push(tr("COULD NOT CANCEL THE DOWNLOAD"), 4000);
    }
    if (run) {
      std::string t = log_tail();
      if (t != last_tail_) {
        last_tail_ = t;
        refresher()();
      }
    }
    if (run != u_->was_running) {
      u_->was_running = run;
      if (!run) {  // finished: the exit status tells how
        // rocknix-update resets `updates.force` in system.cfg itself: read the file again so FORCE NEXT UPDATE does not stay ON
        if (!cx.conf.path().empty()) cx.conf = SystemConf::load(cx.conf.path());
        std::string rc = read_line_plain(tmpf(kRc));
        if (u_->cancelled) {
          u_->cancelled = false;
          cx.sv->toasts.push(tr("DOWNLOAD CANCELLED"), 3000);
        } else if (rc == "0") {
          cx.sv->toasts.push(tr("UPDATE IS READY"), 3000);
        } else {
          std::string tail = log_tail(80);
          cx.sv->toasts.push(std::string(tr("THE UPDATE COULD NOT BE DOWNLOADED. CHECK THE CONNECTION AND TRY AGAIN.")) + (tail.empty() ? "" : " (" + tail + ")"), 7000);
        }
      }
      refresher()();
    }
    return Menu::tick(dt, cx);
  }

  std::optional<float> wake_in() const override {
    std::optional<float> base = Menu::wake_in();
    if (!is_running() && u_->starting <= 0) return base;
    float step = is_running() ? 1.0f : 0.5f;
    return base ? std::min(*base, step) : step;
  }

 private:
  static std::string read_line_plain(const std::string& path) {
    std::string t;
    if (!read_file(path, t)) return {};
    return std::string(trim(t));
  }
  std::shared_ptr<Upd> u_;
  std::string last_tail_;
};

std::unique_ptr<Screen> make_updates() {
  auto u = std::make_shared<Upd>();
  u->was_running = is_running();

  MenuSpec s;
  s.title = tr("UPDATES & DOWNLOADS");
  s.title_key = N_("UPDATES & DOWNLOADS");
  s.build = [u](MenuBuilder& m, Ctx& cx) {
    if (bezel::available()) m.submenu(tr("THE BEZEL PROJECT"), "menu.bezelproject");
    if (!have_bin("rocknix-update")) {
      m.info(tr("No update source available"));
      return;
    }
    m.header(tr("SOFTWARE UPDATES"));
    m.toggle(tr("FORCE NEXT UPDATE"), [&cx] { return get_bool(cx, "updates.force", false); }, [&cx](bool b) { put_bool(cx, "updates.force", b); });

    if (is_running()) {
      std::string t = log_tail();
      int pct = percent_of(t);
      if (pct >= 0) {
        // the real slider bar, read only: its value is the download, not the user's (left/right change nothing)
        m.slider(tr("UPDATING"), 0, 100, 1, [] { return std::max(0, percent_of(log_tail())); }, [](int) {})
            .format([](int v) { return std::to_string(v) + "%"; });
      } else {
        m.info(tr("UPDATING"), t.empty() ? std::string("...") : t);
      }
      m.action(tr("CANCEL DOWNLOAD"), [u]() {
        ui::ConfirmOpts o;
        o.danger = true;
        return ui::confirm(tr("CANCEL THE DOWNLOAD? THE PARTIAL FILE IS DELETED."), [u](Ctx& c) {
          if (is_running()) {
            u->cancelled = true;
            u->cancel_wait = 8.0f;
            cancel_download(c);
          }
          return Transition::none();
        }, o);
      }).danger().with_id("cancel");
    } else if (update_waiting()) {
      m.action(tr("APPLY UPDATE"), []() {
        ui::ConfirmOpts o;
        o.yes = tr("Reboot now");
        o.no = tr("Later");
        return ui::confirm(std::string(tr("UPDATE IS READY")) + "\n\n" + tr("REBOOT TO APPLY"), [](Ctx& c) {
          do_reboot(c);
          return Transition::none();
        }, o);
      }).with_id("apply");
    } else if (u->phase == Upd::Checking) {
      m.info(tr("START UPDATE"), tr("Checking..."));
    } else {
      std::string ver = u->phase == Upd::Available ? u->version : std::string();
      bool avail = u->phase == Upd::Available;
      Entry& e = m.action(tr("START UPDATE"), [u, &cx, avail, ver, refresh = m.refresh] {
        if (avail) {
          ui::ConfirmOpts o;
          o.danger = true;
          return ui::confirm(confirm_text(ver), [u, refresh](Ctx& c) {
            u->phase = Upd::Idle;
            start_download(c);
            u->starting = 4.0f;  // the status files appear a moment later; the poll then follows them
            if (refresh) refresh();
            return Transition::none();
          }, o);
        }
        // the raw status: hiding the network indicator (ES: ShowNetworkIndicator) must not mean "not connected"
        if (!ui_prefs::raw_wifi(cx.status.wifi)) return ui::message(tr("YOU ARE NOT CONNECTED TO A NETWORK"));
        u->phase = Upd::Checking;
        auto sv = cx.sv;
        struct Res {
          bool net = false, can = false;
          std::string ver;
        };
        auto res = std::make_shared<Res>();
        bool forced = get_bool(cx, "updates.force", false);
        post(cx, 
            [sv, res, forced] {
              // ES (ApiSystem::ping): Google, Cloudflare, Quad9 DNS, 2 s each
              for (const char* ip : {"8.8.8.8", "1.1.1.1", "9.9.9.9"}) {
                if (query(*sv, std::string("ping -c 1 -W 2 -t 255 ") + ip, 8000).exit_code == 0) {
                  res->net = true;
                  break;
                }
              }
              if (!res->net) return;
              CmdResult r = query(*sv, "rocknix-update check", 30000);
              res->can = r.exit_code == 0 || forced;
              res->ver = std::string(trim(r.out));
              size_t nl = res->ver.find('\n');
              if (nl != std::string::npos) res->ver.resize(nl);
            },
            [u, res, refresh, &cx] {
              u->phase = Upd::Idle;
              if (!res->net) {
                cx.sv->toasts.push(tr("NETWORK CONNECTION NEEDED"), 3000);
              } else if (!res->can) {
                cx.sv->toasts.push(tr("NO UPDATE AVAILABLE"), 3000);
              } else {
                u->phase = Upd::Available;
                u->version = res->ver;
              }
              if (refresh) refresh();
            });
        if (refresh) refresh();
        return Transition::none();
      });
      e.with_id("start");
      if (avail) e.value(ver.empty() ? std::string(tr("available")) : ver);
    }
  };
  return std::make_unique<UpdatesMenu>(std::move(s), u);
}

}  // namespace

ZF_REGISTER_SCREEN("menu.updates", [](Ctx&, const std::string&) { return make_updates(); });

}  // namespace zf
