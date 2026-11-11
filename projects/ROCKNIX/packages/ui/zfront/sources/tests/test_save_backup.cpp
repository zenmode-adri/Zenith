// SAVE BACKUPS (savebackup.cpp + menu_savebackup.cpp): what goes into the archive (saves yes; ROMs, images, videos, the backup
// folder no), archive names, the real tar / gzip round trip on temp dirs, restore order (verify -> automatic backup -> extract; the
// automatic backup really holds the saves that were replaced), delete only for valid names inside the backup folder, CLOUD BACKUP
// hidden without rclone.conf, and the menu flow with its YES/NO dialogs (NO by default).
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <string>
#include <thread>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "menu_system.hpp"
#include "registry.hpp"
#include "savebackup.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

using namespace zf;
using namespace zf::msys;
namespace sb = zf::savebackup;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string g_dir, g_st, g_bk;

void mkdirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i)
    if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
}

void mkf(const std::string& rel, const std::string& content = "x") {
  std::string p = g_st + "/" + rel;
  mkdirs(p.substr(0, p.rfind('/')));
  std::ofstream(p, std::ios::binary) << content;
}

std::string slurp(const std::string& path) {
  std::string t;
  read_file(path, t);
  return t;
}

// A console tree in a fresh temp dir: saves, ROMs and media side by side.
void make_console(Ctx& cx) {
  static int n = 0;
  g_dir = "/tmp/zf_test_savebackup_" + std::to_string(getpid()) + "_" + std::to_string(++n);
  g_st = g_dir + "/root/storage";
  g_bk = g_st + "/roms/backup";
  mkdirs(g_st);
  mkdirs(g_dir + "/tmp");
  Probe& p = probe();
  p = Probe();
  p.root = g_dir + "/root";
  p.tmp = g_dir + "/tmp";
  p.use_env = true;
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = false;
  cx.sv->cmd.keep_log = true;
  cx.sv->cmd.log.clear();
  SystemConf::lock_path() = g_dir + "/lock";
  std::ofstream(g_dir + "/system.cfg") << "system.language=en_US\n";
  cx.conf = SystemConf::load(g_dir + "/system.cfg");

  mkf("roms/savestates/gba/Game.state1", "state-one");
  mkf("roms/savestates/psx/Disc 1.state.auto", "auto");
  mkf("roms/gba/Game.gba", "ROM-DATA-NOT-A-SAVE");
  mkf("roms/gba/Game.srm", "battery-original");
  mkf("roms/gba/Game.state2", "state-two");
  mkf("roms/gba/Game.state.auto", "auto2");
  mkf("roms/gba/Game.zip", "zip");
  mkf("roms/gba/images/Game-image.png", "png");
  mkf("roms/gba/manuals/Game.pdf", "pdf");
  mkf("roms/snes/My Game (USA).sav", "space-and-parens");
  mkf("roms/snes/My Game (USA).sfc", "rom");
  mkf("roms/psp/PSP/SAVEDATA/ULUS10001DATA00/DATA.BIN", "psp-save");
  mkf("roms/psp/PSP/SAVEDATA/ULUS10001DATA00/PARAM.SFO", "sfo");
  mkf("roms/psp/PSP/GAME/Homebrew/EBOOT.PBP", "not a save");
  mkf("roms/psp/God of War.iso", "iso");
  mkf(".config/ppsspp/PSP/SYSTEM/ppsspp.ini", "ini");
  mkf(".config/ppsspp/PSP/GAME/other.bin", "not saves");
  mkf("roms/videos/intro.mp4", "video");
  mkf("roms/backup/stray.sav", "inside the backup folder: never archived");
}

void settle(Ctx& cx) {
  for (int i = 0; i < 2000; ++i) {
    cx.sv->jobs.drain();
    if (cx.sv->jobs.idle()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  cx.sv->jobs.drain();
}

std::set<std::string> as_set(const std::vector<std::string>& v) { return {v.begin(), v.end()}; }

// names inside an archive, through the PC's own tar
std::set<std::string> archive_names(Ctx& cx, const std::string& archive) {
  CmdResult r = cx.sv->cmd.run("tar -tzf '" + archive + "'", 20000);
  std::set<std::string> s;
  size_t a = 0;
  while (a < r.out.size()) {
    size_t e = r.out.find('\n', a);
    if (e == std::string::npos) e = r.out.size();
    if (e > a) s.insert(r.out.substr(a, e - a));
    a = e + 1;
  }
  return s;
}

int idx(const std::vector<std::string>& log, const std::string& part) {
  for (size_t i = 0; i < log.size(); ++i)
    if (log[i].find(part) != std::string::npos) return int(i);
  return -1;
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower((unsigned char)c));
  return s;
}

bool toasted(Ctx& cx, const std::string& part) {
  for (const Toast& t : cx.sv->toasts.items())
    if (lower(t.text).find(lower(part)) != std::string::npos) return true;
  return false;
}

Menu* open(std::unique_ptr<Screen>& hold, Ctx& cx, const std::string& name, const std::string& arg = {}) {
  hold = Registry::instance().make(name, cx, arg);
  if (!hold) return nullptr;
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

}  // namespace

int main() {
  // ---- names: only these are ever read, restored or deleted
  CHECK(sb::valid_name("saves-20261109-1430.tar.gz"));
  CHECK(sb::valid_name("saves-20261109-1430-pre.tar.gz"));
  CHECK(sb::valid_name("saves-20261109-1430-pre2.tar.gz"));
  for (const char* bad : {"", "saves-20261109-1430.tar", "saves-2026110-1430.tar.gz", "saves-20261109-143.tar.gz", "saves-20261109-14300.tar.gz",
                          "saves-20261109-1430-pre0.tar.gz", "saves-20261109-1430-x.tar.gz", "saves-20261109-1430.tar.gz.part", "../saves-20261109-1430.tar.gz",
                          "saves-20261109-1430.tar.gz/../x", "saves-2026110a-1430.tar.gz", "other.tar.gz", "saves-.tar.gz", "ROCKNIX_BACKUP.zip",
                          "/etc/passwd", "saves-20261109-1430-pre.tar.gz "})
    CHECK(!sb::valid_name(bad));
  CHECK_EQ(sb::pretty_date("saves-20261109-1430.tar.gz"), std::string("2026-11-09 14:30"));
  CHECK_EQ(sb::pretty_date("nope"), std::string());
  CHECK(sb::is_pre("saves-20261109-1430-pre.tar.gz") && !sb::is_pre("saves-20261109-1430.tar.gz"));
  CHECK_EQ(sb::stamp_now().size(), size_t(13));
  CHECK_EQ(sb::size_text(12), std::string("12 B"));
  CHECK_EQ(sb::size_text(2048), std::string("2 KB"));
  CHECK_EQ(sb::size_text(3u << 20), std::string("3.0 MB"));

  // ---- what is archived
  {
    Ctx cx;
    make_console(cx);
    sb::Plan p = sb::plan(g_st);
    std::set<std::string> want = {"roms/savestates/gba/Game.state1",
                                  "roms/savestates/psx/Disc 1.state.auto",
                                  "roms/gba/Game.srm",
                                  "roms/gba/Game.state2",
                                  "roms/gba/Game.state.auto",
                                  "roms/snes/My Game (USA).sav",
                                  "roms/psp/PSP/SAVEDATA/ULUS10001DATA00/DATA.BIN",
                                  "roms/psp/PSP/SAVEDATA/ULUS10001DATA00/PARAM.SFO",
                                  ".config/ppsspp/PSP/SYSTEM/ppsspp.ini"};
    CHECK(as_set(p.files) == want);
    CHECK_EQ(p.files.size(), want.size());  // no duplicates
    for (const std::string& f : p.files) CHECK(f[0] != '/');
    CHECK(p.bytes > 0);

    // commands: every path quoted, tar with -T and a separate gzip (no -z), names relative to storage
    std::string c = sb::create_cmd("/storage", "/storage/roms/backup", "saves-20261109-1430.tar.gz", "/tmp/my list.txt");
    CHECK(c.find("cd '/storage' && tar -cf '/storage/roms/backup/saves-20261109-1430.tar.tmp' -T '/tmp/my list.txt'") == 0);
    CHECK(c.find("gzip -c") != std::string::npos && c.find(" -z") == std::string::npos && c.find("czf") == std::string::npos);
    CHECK(c.find("mv '/storage/roms/backup/saves-20261109-1430.tar.gz.part' '/storage/roms/backup/saves-20261109-1430.tar.gz'") != std::string::npos);

    // ---- real create
    sb::Result r = sb::create(*cx.sv, g_st, g_dir + "/tmp", "20261109-1430", false);
    CHECK(r.err == sb::Err::None);
    CHECK_EQ(r.name, std::string("saves-20261109-1430.tar.gz"));
    CHECK_EQ(r.files, want.size());
    CHECK(r.bytes > 0);
    CHECK(file_exists(g_bk + "/saves-20261109-1430.tar.gz"));
    CHECK(archive_names(cx, g_bk + "/saves-20261109-1430.tar.gz") == want);
    CHECK(!file_exists(g_bk + "/saves-20261109-1430.tar.gz.part"));
    CHECK(!file_exists(g_bk + "/saves-20261109-1430.tar.tmp"));
    // the same minute again: refused, nothing overwritten
    CHECK(sb::create(*cx.sv, g_st, g_dir + "/tmp", "20261109-1430", false).err == sb::Err::Exists);
    // the folder lists it, size included; strays and half-made files are not backups
    mkf("roms/backup/notes.txt", "mine");
    mkf("roms/backup/saves-20261109-1430.tar.gz.part", "half");
    std::vector<sb::Backup> l = sb::list(g_bk);
    CHECK_EQ(l.size(), size_t(1));
    CHECK_EQ(l[0].bytes, r.bytes);
  }

  // ---- no saves at all / not enough room
  {
    Ctx cx;
    make_console(cx);
    std::string cmd = "rm -rf '" + g_st + "/roms' '" + g_st + "/.config'";
    cx.sv->cmd.run(cmd);
    CHECK(sb::create(*cx.sv, g_st, g_dir + "/tmp", "20261109-1430", false).err == sb::Err::NoSaves);
    CHECK(!file_exists(g_bk + "/saves-20261109-1430.tar.gz"));
    CHECK(sb::free_bytes(g_bk) > 0);  // nearest existing parent
  }

  // ---- restore: verify, automatic backup of what is there, then extract
  {
    Ctx cx;
    make_console(cx);
    sb::Result a = sb::create(*cx.sv, g_st, g_dir + "/tmp", "20261109-1430", false);
    CHECK(a.err == sb::Err::None);
    // the world moves on: a save changes, a savestate disappears, a new save appears, a ROM is replaced
    mkf("roms/gba/Game.srm", "battery-NEWER");
    cx.sv->cmd.run("rm -f '" + g_st + "/roms/savestates/gba/Game.state1'");
    mkf("roms/nes/Zelda.srm", "only-in-newer");
    mkf("roms/gba/Game.gba", "ROM-CHANGED");

    sb::Result r = sb::restore(*cx.sv, g_st, g_dir + "/tmp", "20261109-1500", "saves-20261109-1430.tar.gz");
    CHECK(r.err == sb::Err::None);
    CHECK_EQ(r.pre, std::string("saves-20261109-1500-pre.tar.gz"));
    CHECK_EQ(slurp(g_st + "/roms/gba/Game.srm"), std::string("battery-original"));
    CHECK_EQ(slurp(g_st + "/roms/savestates/gba/Game.state1"), std::string("state-one"));
    CHECK_EQ(slurp(g_st + "/roms/gba/Game.gba"), std::string("ROM-CHANGED"));  // ROMs are not part of it
    // the automatic backup holds the saves as they were just before: the newer ones
    CmdResult o = cx.sv->cmd.run("tar -xzOf '" + g_bk + "/saves-20261109-1500-pre.tar.gz' roms/gba/Game.srm");
    CHECK_EQ(o.out, std::string("battery-NEWER"));
    CHECK(archive_names(cx, g_bk + "/saves-20261109-1500-pre.tar.gz").count("roms/nes/Zelda.srm") == 1);
    // a second restore in the same minute does not overwrite the first automatic backup
    sb::Result r2 = sb::restore(*cx.sv, g_st, g_dir + "/tmp", "20261109-1500", "saves-20261109-1430.tar.gz");
    CHECK(r2.err == sb::Err::None);
    CHECK_EQ(r2.pre, std::string("saves-20261109-1500-pre2.tar.gz"));
    // damaged / missing / invalid: nothing touched, no automatic backup
    mkf("roms/gba/Game.srm", "keep-me");
    mkf("roms/backup/saves-20261109-1600.tar.gz", "this is not gzip");
    sb::Result bad = sb::restore(*cx.sv, g_st, g_dir + "/tmp", "20261109-1601", "saves-20261109-1600.tar.gz");
    CHECK(bad.err == sb::Err::Damaged);
    CHECK_EQ(slurp(g_st + "/roms/gba/Game.srm"), std::string("keep-me"));
    CHECK(!file_exists(g_bk + "/saves-20261109-1601-pre.tar.gz"));
    CHECK(sb::restore(*cx.sv, g_st, g_dir + "/tmp", "20261109-1601", "saves-20261109-1700.tar.gz").err == sb::Err::Damaged);
    CHECK(sb::restore(*cx.sv, g_st, g_dir + "/tmp", "20261109-1601", "../../etc/passwd").err == sb::Err::BadName);
    // newest first
    std::vector<sb::Backup> l = sb::list(g_bk);
    CHECK_EQ(l.size(), size_t(4));
    CHECK_EQ(l[0].name, std::string("saves-20261109-1600.tar.gz"));
    CHECK_EQ(l[1].name, std::string("saves-20261109-1500-pre2.tar.gz"));
    CHECK_EQ(l.back().name, std::string("saves-20261109-1430.tar.gz"));
  }

  // ---- restore order in the command log (dry run): verify, then the automatic backup, then the extraction
  {
    Ctx cx;
    make_console(cx);
    mkf("roms/backup/saves-20261109-1430.tar.gz", "dry-run: never read");
    cx.sv->cmd.dry_run = true;
    cx.sv->cmd.log.clear();
    sb::Result r = sb::restore(*cx.sv, g_st, g_dir + "/tmp", "20261109-1500", "saves-20261109-1430.tar.gz");
    CHECK(r.err == sb::Err::None);
    const std::vector<std::string>& lg = cx.sv->cmd.log;
    CHECK_EQ(lg.size(), size_t(3));
    int ver = idx(lg, "gzip -t"), mk = idx(lg, "tar -cf"), ex = idx(lg, "tar -xf");
    CHECK(ver >= 0 && mk > ver && ex > mk);
    CHECK(lg[size_t(mk)].find("saves-20261109-1500-pre.tar.gz") != std::string::npos);
    CHECK(lg[size_t(ex)].find("saves-20261109-1430.tar.gz") != std::string::npos);
    CHECK(lg[size_t(ex)].find("-C '" + g_st + "'") != std::string::npos);
    // nothing was archived or extracted for real
    CHECK(!file_exists(g_bk + "/saves-20261109-1500-pre.tar.gz"));
  }

  // ---- delete: valid names inside the backup folder only
  {
    Ctx cx;
    make_console(cx);
    mkf("roms/backup/saves-20261109-1430.tar.gz", "a");
    mkf("roms/backup/notes.txt", "mine");
    mkf("roms/gba/saves-20261109-1430.tar.gz", "same name, other folder");
    mkf("saves-20261109-1430.tar.gz", "same name, storage root");
    for (const char* bad : {"notes.txt", "../gba/saves-20261109-1430.tar.gz", "../../saves-20261109-1430.tar.gz", "", "saves-20261109-1430.tar.gz/..",
                            "ROCKNIX_BACKUP.zip", "saves-20261109-1430.tar.gz.part"})
      CHECK(!sb::remove_backup(g_bk, bad));
    CHECK(file_exists(g_bk + "/notes.txt") && file_exists(g_bk + "/saves-20261109-1430.tar.gz"));
    CHECK(sb::remove_backup(g_bk, "saves-20261109-1430.tar.gz"));
    CHECK(!file_exists(g_bk + "/saves-20261109-1430.tar.gz"));
    CHECK(file_exists(g_bk + "/notes.txt") && file_exists(g_st + "/roms/gba/saves-20261109-1430.tar.gz") && file_exists(g_st + "/saves-20261109-1430.tar.gz"));
    CHECK(!sb::remove_backup(g_bk, "saves-20261109-1430.tar.gz"));  // already gone
  }

  // ---- the menu
  {
    Ctx cx;
    make_console(cx);
    CHECK(Registry::instance().has("menu.savebackup") && Registry::instance().has("menu.savebackup.item"));
    std::unique_ptr<Screen> h;
    Menu* m = open(h, cx, "menu.savebackup");
    CHECK(m != nullptr);
    CHECK(find(*m, "BACK UP SAVES NOW") >= 0);
    CHECK(find(*m, "CLOUD BACKUP") < 0);  // no rclone.conf
    CHECK(find(*m, "NO BACKUPS YET") >= 0);

    // the item screen refuses an argument that is not a backup name
    std::unique_ptr<Screen> hx;
    CHECK(open(hx, cx, "menu.savebackup.item", "") == nullptr);
    CHECK(open(hx, cx, "menu.savebackup.item", "../../etc/passwd") == nullptr);
    CHECK(open(hx, cx, "menu.savebackup.item", "notes.txt") == nullptr);

    // BACK UP SAVES NOW: job on the slow lane, a toast with the result, the list shows the new archive
    CHECK(go(*m, cx, "BACK UP SAVES NOW"));
    m->handle(Action::A, cx);
    m->tick(0.0f, cx);
    settle(cx);
    m->tick(0.0f, cx);
    m->tick(0.0f, cx);
    std::vector<sb::Backup> l = sb::list(g_bk);
    CHECK_EQ(l.size(), size_t(1));
    CHECK(toasted(cx, "backup done") && toasted(cx, "9 files"));
    CHECK(find(*m, "NO BACKUPS YET") < 0);
    CHECK(find(*m, sb::pretty_date(l[0].name)) >= 0);
    const std::string name = l[0].name;

    // RESTORE: the dialog starts on NO; NO / B change nothing
    std::unique_ptr<Screen> hi;
    Menu* it = open(hi, cx, "menu.savebackup.item", name);
    CHECK(it != nullptr);
    CHECK(find(*it, "RESTORE") >= 0 && find(*it, "DELETE BACKUP") >= 0);
    mkf("roms/gba/Game.srm", "battery-NEWER");
    CHECK(go(*it, cx, "RESTORE"));
    Transition t = it->handle(Action::A, cx);
    CHECK(t.kind == Transition::Push && t.screen != nullptr);
    auto* dlg = static_cast<ui::ConfirmDialog*>(t.screen.get());
    CHECK_EQ(dlg->focus(), 1);
    Transition tn = dlg->handle(Action::A, cx);  // NO
    CHECK(tn.kind == Transition::Pop);
    settle(cx);
    CHECK_EQ(slurp(g_st + "/roms/gba/Game.srm"), std::string("battery-NEWER"));
    CHECK_EQ(sb::list(g_bk).size(), size_t(1));
    // YES: automatic backup first, then the saves come back
    Transition t2 = it->handle(Action::A, cx);
    auto* d2 = static_cast<ui::ConfirmDialog*>(t2.screen.get());
    d2->handle(Action::Left, cx);
    CHECK_EQ(d2->focus(), 0);
    d2->handle(Action::A, cx);
    settle(cx);
    CHECK_EQ(slurp(g_st + "/roms/gba/Game.srm"), std::string("battery-original"));
    std::vector<sb::Backup> l2 = sb::list(g_bk);
    CHECK_EQ(l2.size(), size_t(2));
    CHECK(sb::is_pre(l2[0].name) || sb::is_pre(l2[1].name));
    CHECK(toasted(cx, "saves restored"));

    // DELETE BACKUP: confirm (NO by default), then only that archive goes; any key leaves the screen
    CHECK(go(*it, cx, "DELETE BACKUP"));
    Transition t3 = it->handle(Action::A, cx);
    auto* d3 = static_cast<ui::ConfirmDialog*>(t3.screen.get());
    CHECK_EQ(d3->focus(), 1);
    d3->handle(Action::A, cx);
    CHECK(file_exists(g_bk + "/" + name));
    Transition t4 = it->handle(Action::A, cx);
    auto* d4 = static_cast<ui::ConfirmDialog*>(t4.screen.get());
    d4->handle(Action::Left, cx);
    d4->handle(Action::A, cx);
    CHECK(!file_exists(g_bk + "/" + name));
    CHECK_EQ(sb::list(g_bk).size(), size_t(1));  // the automatic one is still there
    CHECK(file_exists(g_bk + "/stray.sav"));
    it->tick(0.0f, cx);
    CHECK(it->handle(Action::A, cx).kind == Transition::Pop);
    m->tick(0.0f, cx);  // the list rebuilds itself
    m->tick(0.0f, cx);
    CHECK(find(*m, sb::pretty_date(name)) < 0);  // the deleted one is gone; the automatic one stays (label with the extra note)
    bool pre_row = false;
    for (size_t i = 0; i < m->size(); ++i) pre_row = pre_row || (m->entry(i).kind == EntryKind::Action && m->entry(i).label.find(sb::pretty_date(sb::list(g_bk)[0].name)) == 0);
    CHECK(pre_row);
    CHECK_EQ(sb::list(g_bk).size(), size_t(1));
  }

  // ---- CLOUD BACKUP: only with rclone.conf (and the script); runs the script and reports the exit code
  {
    Ctx cx;
    make_console(cx);
    std::unique_ptr<Screen> h;
    mkdirs(g_dir + "/root/storage/.config/rclone");
    std::ofstream(g_dir + "/root/storage/.config/rclone/rclone.conf") << "[x]\n";
    Menu* m = open(h, cx, "menu.savebackup");
    CHECK(find(*m, "CLOUD BACKUP") < 0);  // conf alone is not enough: the script has to exist
    mkdirs(g_dir + "/root/usr/bin");
    std::ofstream(g_dir + "/root/usr/bin/cloud_backup") << "x";
    std::unique_ptr<Screen> h2;
    m = open(h2, cx, "menu.savebackup");
    CHECK(find(*m, "CLOUD BACKUP") >= 0);
    cx.sv->cmd.dry_run = true;
    cx.sv->cmd.log.clear();
    CHECK(go(*m, cx, "CLOUD BACKUP"));
    m->handle(Action::A, cx);
    settle(cx);
    CHECK_EQ(cx.sv->cmd.log.size(), size_t(1));
    CHECK(cx.sv->cmd.log[0] == "/usr/bin/cloud_backup");
    CHECK(toasted(cx, "cloud backup done"));
    // without the conf it is hidden again
    ::unlink((g_dir + "/root/storage/.config/rclone/rclone.conf").c_str());
    std::unique_ptr<Screen> h3;
    CHECK(find(*open(h3, cx, "menu.savebackup"), "CLOUD BACKUP") < 0);
  }
  return zt::done();
}
