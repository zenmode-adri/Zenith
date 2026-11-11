// `menu.savebackup` (LIBRARY > "SAVE BACKUPS"): archives of the game saves in /storage/roms/backup (see savebackup.hpp for what is kept).
//
//   BACK UP SAVES NOW   makes saves-YYYYMMDD-HHMM.tar.gz (space checked first; toast with the file count and size)
//   <one row per backup, newest first>   opens `menu.savebackup.item` (argument = the file name):
//       RESTORE         YES/NO (NO by default), automatic backup of the current saves first, then extract
//       DELETE BACKUP   YES/NO, deletes that one archive (the name must be a valid backup name)
//   CLOUD BACKUP        only with /storage/.config/rclone/rclone.conf and /usr/bin/cloud_backup: runs the script, reports its exit code
//
// Everything slow runs on the slow lane (`post(..., slow=true)`); the list shows PLEASE WAIT meanwhile and refuses a second job.
// Not in the kiosk / kid whitelist (uimode.cpp).
#include <atomic>
#include <cstdio>
#include <memory>

#include "menu.hpp"
#include "menu_system.hpp"
#include "registry.hpp"
#include "savebackup.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf {
namespace {

using namespace msys;
namespace sb = savebackup;

std::atomic<bool>& g_busy = sb::busy_flag();  // a backup / restore / cloud job is running (shared with the web page: one at a time)
std::atomic<int>& g_gen = sb::generation();   // bumped when the list of backups may have changed (the list screen rebuilds)

std::string storage() { return os_path("/storage"); }
bool cloud_available() { return exists("/storage/.config/rclone/rclone.conf") && have_bin("cloud_backup"); }

std::string err_text(sb::Err e) {
  switch (e) {
    case sb::Err::NoSaves: return tr("NO SAVES FOUND");
    case sb::Err::NoSpace: return tr("NOT ENOUGH FREE SPACE");
    case sb::Err::Exists: return tr("A BACKUP FROM THIS MINUTE ALREADY EXISTS");
    case sb::Err::Damaged: return tr("THE BACKUP IS DAMAGED OR MISSING");
    default: return tr("AN ERROR OCCURRED");
  }
}

std::string done_text(const sb::Result& r) {
  return std::string(tr("BACKUP DONE")) + ": " + std::to_string(r.files) + " " + tr("FILES") + ", " + sb::size_text(r.bytes);
}

void start_backup(Ctx& cx, const Refresh& refresh) {
  if (g_busy.exchange(true)) return;
  auto sv = cx.sv;
  auto res = std::make_shared<sb::Result>();
  const std::string st = storage(), tmp = probe().tmp, stamp = sb::stamp_now();
  if (refresh) refresh();
  post(cx, [sv, res, st, tmp, stamp] { *res = sb::create(*sv, st, tmp, stamp, false); },
       [&cx, res, refresh] {
         g_busy = false;
         ++g_gen;
         cx.sv->toasts.push(res->err == sb::Err::None ? done_text(*res) : err_text(res->err), res->err == sb::Err::None ? 4000 : 5000);
         if (refresh) refresh();
       },
       true);
}

void start_restore(Ctx& cx, const std::string& name, const Refresh& refresh) {
  if (g_busy.exchange(true)) return;
  auto sv = cx.sv;
  auto res = std::make_shared<sb::Result>();
  const std::string st = storage(), tmp = probe().tmp, stamp = sb::stamp_now();
  if (refresh) refresh();
  post(cx, [sv, res, st, tmp, stamp, name] { *res = sb::restore(*sv, st, tmp, stamp, name); },
       [&cx, res, refresh] {
         g_busy = false;
         ++g_gen;
         cx.sv->toasts.push(res->err == sb::Err::None ? std::string(tr("SAVES RESTORED")) : err_text(res->err), res->err == sb::Err::None ? 4000 : 6000);
         if (refresh) refresh();
       },
       true);
}

void start_cloud(Ctx& cx, const Refresh& refresh) {
  if (g_busy.exchange(true)) return;
  auto sv = cx.sv;
  auto rc = std::make_shared<int>(-1);
  if (refresh) refresh();
  post(cx, [sv, rc] { *rc = sv->cmd.run("/usr/bin/cloud_backup", 3600000).exit_code; },
       [&cx, rc, refresh] {
         g_busy = false;
         std::string t = *rc == 0 ? std::string(tr("CLOUD BACKUP DONE")) : std::string(tr("CLOUD BACKUP FAILED")) + " (" + std::to_string(*rc) + ")";
         cx.sv->toasts.push(t, *rc == 0 ? 4000 : 6000);
         if (refresh) refresh();
       },
       true);
}

std::string label_of(const std::string& name) {
  std::string l = sb::pretty_date(name);
  if (sb::is_pre(name)) l += std::string(" (") + tr("BEFORE A RESTORE") + ")";
  return l;
}

// The list rebuilds itself when a job finished or a backup was deleted from the screen above it.
class ListMenu : public Menu {
 public:
  explicit ListMenu(MenuSpec s) : Menu(std::move(s)), seen_(g_gen.load()) {}
  bool tick(float dt, Ctx& cx) override {
    int g = g_gen.load();
    if (g != seen_) {
      seen_ = g;
      refresher()();
    }
    return Menu::tick(dt, cx);
  }

 private:
  int seen_;
};

std::unique_ptr<Screen> make_list() {
  MenuSpec s;
  s.title = tr("SAVE BACKUPS");
  s.title_key = N_("SAVE BACKUPS");
  s.build = [](MenuBuilder& m, Ctx& cx) {
    if (g_busy) {
      m.info(tr("PLEASE WAIT"), "...");
    } else {
      m.action(tr("BACK UP SAVES NOW"), [&cx, r = m.refresh] { start_backup(cx, r); }).with_id("backup").hint(tr("SAVEGAMES, SAVESTATES AND PSP SAVES, WITHOUT ROMS OR MEDIA"));
    }
    if (cloud_available() && !g_busy) m.action(tr("CLOUD BACKUP"), [&cx, r = m.refresh] { start_cloud(cx, r); }).with_id("cloud");

    m.header(tr("BACKUPS"));
    std::vector<sb::Backup> all = sb::list(sb::backup_dir(storage()));
    if (all.empty()) m.info(tr("NO BACKUPS YET"));
    for (const sb::Backup& b : all) m.submenu(label_of(b.name), "menu.savebackup.item", b.name).value(sb::size_text(b.bytes));
  };
  return std::make_unique<ListMenu>(std::move(s));
}

// One backup: RESTORE / DELETE BACKUP. After a delete the screen has nothing left to offer: any key goes back to the list.
class ItemMenu : public Menu {
 public:
  ItemMenu(MenuSpec s, std::shared_ptr<bool> gone) : Menu(std::move(s)), gone_(std::move(gone)) {}
  Transition handle(Action a, Ctx& cx) override {
    if (*gone_) return Transition::pop();
    return Menu::handle(a, cx);
  }

 private:
  std::shared_ptr<bool> gone_;
};

std::unique_ptr<Screen> make_item(const std::string& name) {
  if (!sb::valid_name(name)) return nullptr;  // an argument is required (and checked: the name ends up in a path)
  auto gone = std::make_shared<bool>(false);
  const std::string date = sb::pretty_date(name);
  MenuSpec s;
  s.title = label_of(name);
  s.build = [name, date, gone](MenuBuilder& m, Ctx&) {
    if (*gone) {
      m.info(tr("BACKUP DELETED"));
      return;
    }
    char msg[512];
    std::snprintf(msg, sizeof msg,
                  tr("THIS WILL REPLACE YOUR CURRENT SAVES WITH THE ONES FROM %s. A BACKUP OF THE CURRENT SAVES IS MADE FIRST."), date.c_str());
    m.action(tr("RESTORE"), [name, r = m.refresh, &cx = m.cx] {
       if (g_busy) {
         cx.sv->toasts.push(tr("PLEASE WAIT"), 2000);
         return;
       }
       start_restore(cx, name, r);
     }).confirm(msg).danger().with_id("restore");
    m.action(tr("DELETE BACKUP"), [name, gone, r = m.refresh, &cx = m.cx] {
       if (g_busy) {
         cx.sv->toasts.push(tr("PLEASE WAIT"), 2000);
         return;
       }
       if (sb::remove_backup(sb::backup_dir(storage()), name)) {
         *gone = true;
         ++g_gen;
         cx.sv->toasts.push(tr("BACKUP DELETED"), 3000);
       } else {
         cx.sv->toasts.push(tr("AN ERROR OCCURRED"), 4000);
       }
       if (r) r();
     }).confirm(tr("DELETE THIS BACKUP?")).danger().with_id("delete");
  };
  return std::make_unique<ItemMenu>(std::move(s), gone);
}

}  // namespace

ZF_REGISTER_SCREEN("menu.savebackup", [](Ctx&, const std::string&) { return make_list(); });
ZF_REGISTER_SCREEN("menu.savebackup.item", [](Ctx&, const std::string& arg) { return make_item(arg); });

}  // namespace zf
