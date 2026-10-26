// menu.updates: gating, the check -> confirm -> download flow (commands recorded in dry-run, status files under a temp dir),
// progress polling, "update ready" and the reboot that applies it.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "menu_system.hpp"
#include "registry.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

using namespace zf;
using namespace zf::msys;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string g_dir;

void mkdirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i)
    if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
}

void put_file(const std::string& rel, const std::string& content = {}) {
  std::string p = g_dir + rel;
  mkdirs(p.substr(0, p.rfind('/')));
  std::ofstream(p, std::ios::binary) << content;
}

void make_console(Ctx& cx, bool with_updater = true) {
  static int n = 0;
  g_dir = "/tmp/zf_test_updates_" + std::to_string(getpid()) + "_" + std::to_string(++n);
  mkdirs(g_dir + "/root");
  mkdirs(g_dir + "/tmp");
  Probe& p = probe();
  p = Probe();
  p.root = g_dir + "/root";
  p.tmp = g_dir + "/tmp";
  p.use_env = true;
  p.env["OS_VERSION"] = "20261024";
  p.env["OS_BUILD"] = "rocknix";
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = true;
  cx.sv->cmd.log.clear();
  SystemConf::lock_path() = g_dir + "/lock";
  put_file("/system.cfg", "system.language=en_US\n");
  cx.conf = SystemConf::load(g_dir + "/system.cfg");
  if (with_updater) put_file("/root/usr/bin/rocknix-update");
}

void settle(Ctx& cx) {
  for (int i = 0; i < 1000; ++i) {
    cx.sv->jobs.drain();
    if (cx.sv->jobs.idle()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  cx.sv->jobs.drain();
}

Menu* open(std::unique_ptr<Screen>& hold, Ctx& cx) {
  hold = Registry::instance().make("menu.updates", cx, "");
  CHECK(hold != nullptr);
  Menu* m = static_cast<Menu*>(hold.get());
  m->tick(0.0f, cx);
  return m;
}

int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}

bool go(Menu& m, Ctx& cx, const std::string& label) {
  int want = find(m, label);
  if (want < 0) return false;
  for (size_t k = 0; k <= m.size() + 1 && m.cursor() != want; ++k) m.handle(Action::Down, cx);
  return m.cursor() == want;
}

bool logged(Ctx& cx, const std::string& part) {
  for (const std::string& l : cx.sv->cmd.log)
    if (l.find(part) != std::string::npos) return true;
  return false;
}

bool toasted(Ctx& cx, const std::string& part) {
  for (const Toast& t : cx.sv->toasts.items())
    if (t.text.find(part) != std::string::npos) return true;
  return false;
}

// START UPDATE pressed with Wi-Fi up; waits for the check
void run_check(Menu& m, Ctx& cx) {
  cx.status.wifi = true;
  CHECK(go(m, cx, "START UPDATE"));
  m.handle(Action::A, cx);
  m.tick(0.0f, cx);
  settle(cx);
  m.tick(0.0f, cx);
}

}  // namespace

int main() {
  // ---- no updater in the image: nothing to offer, nothing dimmed
  {
    Ctx cx;
    make_console(cx, false);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx);
    CHECK(Registry::instance().has("menu.updates"));
    CHECK_EQ(m->size(), size_t(1));
    CHECK(m->entry(0).kind == EntryKind::Info);
    for (const char* l : {"THEMES", "CONTENT DOWNLOADER", "THE BEZEL PROJECT", "CHECK FOR UPDATES", "UPDATE BRANCH", "START UPDATE"})
      CHECK(find(*m, l) < 0);
  }

  // ---- the updater is there: FORCE NEXT UPDATE + START UPDATE; ES-only / no-effect rows stay hidden
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx);
    CHECK(find(*m, "FORCE NEXT UPDATE") >= 0);
    CHECK(find(*m, "START UPDATE") >= 0);
    for (const char* l : {"THEMES", "CONTENT DOWNLOADER", "THE BEZEL PROJECT", "CHECK FOR UPDATES", "UPDATE BRANCH", "START LOCAL MEDIA UPDATE"})
      CHECK(find(*m, l) < 0);
    CHECK(go(*m, cx, "FORCE NEXT UPDATE"));
    m->handle(Action::A, cx);
    CHECK_EQ(cx.conf.get_or("updates.force", ""), std::string("1"));  // the key rocknix-update reads
    m->handle(Action::A, cx);
    CHECK_EQ(cx.conf.get_or("updates.force", ""), std::string("0"));
  }

  // ---- no network: a message, no command
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx);
    cx.status.wifi = false;
    CHECK(go(*m, cx, "START UPDATE"));
    Transition t = m->handle(Action::A, cx);
    CHECK(t.kind == Transition::Push && t.screen);
    CHECK(cx.sv->cmd.log.empty());
  }

  // ---- the three DNS pings fail: "network needed", the check is never run
  {
    Ctx cx;
    make_console(cx);
    for (const char* ip : {"8.8.8.8", "1.1.1.1", "9.9.9.9"}) probe().canned[std::string("ping -c 1 -W 2 -t 255 ") + ip] = {1, ""};
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx);
    run_check(*m, cx);
    CHECK(toasted(cx, "NETWORK CONNECTION NEEDED"));
    CHECK(logged(cx, "ping -c 1 -W 2 -t 255 9.9.9.9"));
    CHECK(!logged(cx, "rocknix-update check"));
    CHECK(find(*m, "START UPDATE") >= 0 && m->entry(size_t(find(*m, "START UPDATE"))).kind == EntryKind::Action);
  }

  // ---- nothing new
  {
    Ctx cx;
    make_console(cx);
    probe().canned["rocknix-update check"] = {1, ""};
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx);
    run_check(*m, cx);
    CHECK(toasted(cx, "NO UPDATE AVAILABLE"));
    CHECK(!logged(cx, "echo $? >"));
    CHECK(m->entry(size_t(find(*m, "START UPDATE"))).value_text.empty());
  }

  // ---- an update exists: the version shows, the confirmation comes first, NO changes nothing, YES starts the download
  {
    Ctx cx;
    make_console(cx);
    probe().canned["rocknix-update check"] = {0, "20261101\n"};
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx);
    run_check(*m, cx);
    CHECK(logged(cx, "ping -c 1 -W 2 -t 255 8.8.8.8"));  // answered at once: the others are not tried
    CHECK(!logged(cx, "ping -c 1 -W 2 -t 255 1.1.1.1"));
    int s = find(*m, "START UPDATE");
    CHECK_EQ(m->entry(size_t(s)).value_text, std::string("20261101"));
    cx.sv->cmd.log.clear();
    CHECK(go(*m, cx, "START UPDATE"));
    Transition t = m->handle(Action::A, cx);
    CHECK(t.kind == Transition::Push && t.screen);
    CHECK(cx.sv->cmd.log.empty());
    Transition no = t.screen->handle(Action::A, cx);  // focus starts on NO
    CHECK(no.kind == Transition::Pop);
    CHECK(cx.sv->cmd.log.empty());
    Transition t2 = m->handle(Action::A, cx);
    t2.screen->handle(Action::Left, cx);  // YES
    t2.screen->handle(Action::A, cx);
    CHECK_EQ(cx.sv->cmd.log.size(), size_t(1));
    const std::string& c = cx.sv->cmd.log[0];
    CHECK(c.find("rocknix-update > '" + g_dir + "/tmp/zf-update.log' 2>&1") != std::string::npos);
    CHECK(c.find("echo $? > '" + g_dir + "/tmp/zf-update.rc'") != std::string::npos);
    CHECK(c.find("rm -f '" + g_dir + "/tmp/zf-update.rc'") == 0);  // stale status removed first
    // right after starting, the menu keeps waking up to see the status files appear
    CHECK(m->wake_in().has_value());
  }

  // ---- forced update with a failing check: still offered, version unknown
  {
    Ctx cx;
    make_console(cx);
    cx.conf.persist("updates.force", "1");
    probe().canned["rocknix-update check"] = {1, ""};
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx);
    run_check(*m, cx);
    CHECK(!toasted(cx, "NO UPDATE AVAILABLE"));
    CHECK_EQ(m->entry(size_t(find(*m, "START UPDATE"))).value_text, std::string("available"));
  }

  // ---- a download in progress: progress row, polling every second, result when it ends
  {
    Ctx cx;
    make_console(cx);
    put_file("/tmp/zf-update.run");
    put_file("/tmp/zf-update.log", "Update available: 20261101\r\n######                      23.1%\r######################     84.0%");
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx);
    int u = find(*m, "UPDATING");
    CHECK(u >= 0);
    CHECK_EQ(m->entry(size_t(u)).value_text, std::string("######################     84.0%"));
    CHECK(find(*m, "START UPDATE") < 0);  // cannot be started twice
    CHECK(m->wake_in().has_value());
    CHECK(*m->wake_in() <= 1.0f);
    // it ends well: the tar and its checksum are in /storage/.update
    ::unlink((g_dir + "/tmp/zf-update.run").c_str());
    put_file("/tmp/zf-update.rc", "0\n");
    put_file("/root/storage/.update/AURKNIX-RK3326.aarch64-20261101.tar");
    put_file("/root/storage/.update/AURKNIX-RK3326.aarch64-20261101.tar.sha256");
    m->tick(1.0f, cx);
    m->tick(0.0f, cx);
    CHECK(find(*m, "UPDATING") < 0);
    CHECK(find(*m, "APPLY UPDATE") >= 0);
    CHECK(find(*m, "START UPDATE") < 0);
    CHECK(toasted(cx, "UPDATE IS READY"));
    CHECK(!m->wake_in().has_value());
    // APPLY: asks, and YES reboots (what really applies the OTA; ES's RESTART only restarts the frontend)
    int hits = 0;
    probe().reboot = [&hits](Ctx&) { ++hits; };
    CHECK(go(*m, cx, "APPLY UPDATE"));
    Transition t = m->handle(Action::A, cx);
    CHECK(t.kind == Transition::Push && t.screen);
    CHECK_EQ(hits, 0);
    t.screen->handle(Action::A, cx);  // NO is focused: nothing
    CHECK_EQ(hits, 0);
    Transition t2 = m->handle(Action::A, cx);
    t2.screen->handle(Action::Left, cx);
    t2.screen->handle(Action::A, cx);
    CHECK_EQ(hits, 1);
  }

  // ---- it fails: the last log line explains
  {
    Ctx cx;
    make_console(cx);
    put_file("/tmp/zf-update.run");
    put_file("/tmp/zf-update.log", "Downloading update file...\nFailed to download update file.\n");
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx);
    ::unlink((g_dir + "/tmp/zf-update.run").c_str());
    put_file("/tmp/zf-update.rc", "1\n");
    m->tick(1.0f, cx);
    m->tick(0.0f, cx);
    CHECK(toasted(cx, "AN ERROR OCCURRED: Failed to download update file."));
    CHECK(find(*m, "START UPDATE") >= 0);
    CHECK(find(*m, "APPLY UPDATE") < 0);
  }

  // ---- reopened later (or after restarting the frontend): the waiting update is found on disk
  {
    Ctx cx;
    make_console(cx);
    put_file("/root/storage/.update/x.tar");  // half-downloaded: no checksum yet
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx);
    CHECK(find(*m, "APPLY UPDATE") < 0);
    CHECK(find(*m, "START UPDATE") >= 0);
    put_file("/root/storage/.update/x.tar.sha256");
    std::unique_ptr<Screen> h2;
    Menu* m2 = open(h2, cx);
    CHECK(find(*m2, "APPLY UPDATE") >= 0);
  }

  return zt::done();
}
