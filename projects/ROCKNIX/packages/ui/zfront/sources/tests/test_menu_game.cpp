// menu-game: es_features.cfg parsing and resolution, the keys the menus write to system.cfg (system and game), the
// emulator/core picker, lists re-rooted to a temp tree, and that a changed option reaches the next launch command.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "launch.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "registry.hpp"
#include "screens.hpp"
#include "util.hpp"

using namespace zf;

namespace {

const char* kFeatures = R"(<features>
  <sharedFeatures>
    <feature name="colour boost" value="colour_boost" description="Boost the colours." preset="switch"/>
  </sharedFeatures>
  <globalFeatures>
    <feature name="extra global" value="global.extra_global" group="MISC">
      <choice name="one" value="1"/><choice name="two" value="2"/>
    </feature>
  </globalFeatures>
  <emulator name="libretro" features="ratio,smooth,rewind,shaders,latency_reduction,decoration,autocontrollers">
    <core name="gambatte" features="colorization">
      <feature name="palette" value="palette" submenu="PALETTES">
        <choice name="mono" value="mono"/><choice name="color" value="color"/>
      </feature>
      <feature name="hidden one" value="hid" preset="hidden"/>
      <sharedFeature value="colour_boost"/>
      <feature name="volume boost" value="volboost" preset="slider" preset-parameters="0 100 10 50 %"/>
      <feature name="frame skip" value="frameskip" preset="sliderauto" preset-parameters="0 5 1"/>
      <feature name="smooth on" value="smoothon" preset="switchon"/>
      <feature name="smooth off" value="smoothoff" preset="switchoff"/>
      <feature name="engine" value="engine" order="-1">
        <choice name="fast" value="fast"/><choice name="accurate" value="accurate"/>
      </feature>
    </core>
    <core name="mgba"/>
  </emulator>
</features>)";

System make_gb() {
  System s;
  s.name = "gb";
  s.fullname = "Game Boy";
  s.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR% --controllers=\"%CONTROLLERSCONFIG%\"";
  s.emulator = "libretro";
  s.core = "gambatte";
  s.emus = {{"libretro", {"gambatte", "mgba"}}};
  s.rom_dir = "/roms/gb";
  s.loaded = true;
  Game a;
  a.name = "Tetris";
  a.path = "/roms/gb/Tetris.gb";
  Game b;
  b.name = "Zelda";
  b.path = "/roms/gb/Zelda=1#.gb";
  s.games = {a, b};
  s.count = 2;
  return s;
}

System make_nes() {
  System s;
  s.name = "nes";
  s.fullname = "NES";
  s.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE%";
  s.emulator = "libretro";
  s.core = "fceumm";
  s.emus = {{"libretro", {"fceumm"}}};
  s.loaded = true;
  return s;
}

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string slurp(const std::string& p) {
  std::string t;
  read_file(p, t);
  return t;
}

void spit(const std::string& p, const std::string& t) {
  std::ofstream f(p, std::ios::binary);
  f << t;
}

void mkdirs(const std::string& p) {
  std::string cur;
  for (size_t i = 0; i <= p.size(); ++i) {
    if (i == p.size() || p[i] == '/') {
      if (!cur.empty()) ::mkdir(cur.c_str(), 0755);
    }
    if (i < p.size()) cur += p[i];
  }
}

Menu* as_menu(const std::unique_ptr<Screen>& s) { return static_cast<Menu*>(s.get()); }

int find(const Menu& m, const std::string& label) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == label) return int(i);
  return -1;
}

std::string cfg(const std::string& dir) { return slurp(dir + "/system.cfg"); }

bool has_line(const std::string& text, const std::string& line) {
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    std::string l = text.substr(i, e == std::string::npos ? std::string::npos : e - i);
    if (l == line) return true;
    i = e == std::string::npos ? text.size() : e + 1;
  }
  return false;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-mg-test-XXXXXX";
  const char* d = mkdtemp(tmpl);
  CHECK(d != nullptr);
  if (!d) return 1;
  std::string dir = d;

  // ---- a re-rooted device tree
  mg::env().root = dir + "/root";
  mkdirs(dir + "/root/tmp/shaders/crt");
  spit(dir + "/root/tmp/shaders/crt/crt-easymode.glslp", "x");
  spit(dir + "/root/tmp/shaders/sharp.glslp", "x");
  spit(dir + "/root/tmp/shaders/ignored.slangp", "x");
  mkdirs(dir + "/root/usr/share/retroarch/filters/64bit/video");
  spit(dir + "/root/usr/share/retroarch/filters/64bit/video/Grid3x.filt", "x");
  mkdirs(dir + "/root/tmp/overlays/bezels/Nintendo");
  mkdirs(dir + "/root/sys/devices/system/cpu/cpufreq/policy0");
  spit(dir + "/root/sys/devices/system/cpu/cpufreq/policy0/scaling_available_governors", "conservative ondemand userspace powersave performance schedutil\n");
  mkdirs(dir + "/root/usr/bin");
  spit(dir + "/root/usr/bin/rocknix-systems", "#!/bin/sh\n");
  mkdirs(dir + "/root/storage/.config/emulationstation");
  spit(dir + "/root/storage/.config/emulationstation/es_features.cfg", kFeatures);
  mg::reset_caches();

  // ---- es_features.cfg: flags, custom features, order, hidden, shared, lookup by emulator / core
  {
    System gb = make_gb();
    const mg::FeatureDb& db = mg::features();
    CHECK(db.loaded());
    mg::Resolved r = db.resolve(gb, "libretro", "gambatte");
    CHECK(r.has(mg::F_ratio) && r.has(mg::F_smooth) && r.has(mg::F_rewind) && r.has(mg::F_shaders));
    CHECK(r.has(mg::F_colorization));
    CHECK(!r.has(mg::F_netplay));
    CHECK(r.custom.size() >= 7);
    CHECK_EQ(r.custom.front().value, std::string("engine"));  // order -1 first, the rest keep file order
    CHECK(r.has_custom("colour_boost"));                      // <sharedFeature> resolved
    CHECK(r.has_custom("hid"));
    mg::Resolved m = db.resolve(gb, "libretro", "mgba");  // the core has no extras, the emulator flags still apply
    CHECK(m.has(mg::F_ratio) && !m.has(mg::F_colorization) && m.custom.empty());
    mg::Resolved bad = db.resolve(gb, "nope", "x");  // unknown emulator: nothing
    CHECK(bad.flags == 0 && bad.custom.empty());
    CHECK(db.has_global("extra_global"));
    CHECK(db.has_global("global.extra_global"));
    CHECK(!db.has_global("ratio"));
    CHECK(db.system_has_features(gb));
    System nes = make_nes();
    CHECK(db.system_has_features(nes));  // flags of the emulator apply to every core
    // a features file that is not one
    mg::FeatureDb x;
    CHECK(!x.add_text("<nope/>"));
    CHECK(!x.add_text("not xml"));
  }

  // ---- key format and system.cfg semantics
  Ctx cx;
  {
    std::string err;
    CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
    cx.w = 640;
    cx.h = 480;
  }
  cx.sv->cmd.dry_run = true;
  SystemConf::lock_path() = dir + "/lock";
  spit(dir + "/system.cfg", "audio.volume=50\nglobal.ratio=16/9\n");
  cx.conf = SystemConf::load(dir + "/system.cfg");
  cx.systems = {make_gb(), make_nes()};
  CHECK_EQ(mg::game_cfg("gb", "Zelda=1#.gb"), std::string("gb[\"Zelda1.gb\"]"));
  CHECK_EQ(mg::game_cfg("gb", "Tetris.gb"), std::string("gb[\"Tetris.gb\"]"));
  {
    std::string sys, rom;
    CHECK(mg::split_game_arg(mg::game_arg("gb", "/roms/gb/Tetris.gb"), sys, rom));
    CHECK_EQ(sys, std::string("gb"));
    CHECK_EQ(rom, std::string("/roms/gb/Tetris.gb"));
    CHECK(!mg::split_game_arg("gb", sys, rom));
  }
  CHECK(mg::conf_put(cx, "gb.ratio", "4/3"));
  CHECK(has_line(cfg(dir), "gb.ratio=4/3"));
  CHECK(mg::conf_put(cx, "gb.ratio", "auto"));  // auto removes the line
  CHECK(!has_line(cfg(dir), "gb.ratio=4/3"));
  CHECK(cfg(dir).find("gb.ratio") == std::string::npos);
  CHECK(has_line(cfg(dir), "audio.volume=50"));  // other keys survive
  CHECK(mg::conf_put(cx, "gb.smooth", "1"));
  CHECK(mg::conf_put(cx, "gb.smooth", ""));
  CHECK(cfg(dir).find("gb.smooth") == std::string::npos);
  // a no-op change does not touch the file (its mtime is what invalidates runemu.sh --prepare)
  {
    struct stat a, b;
    ::stat((dir + "/system.cfg").c_str(), &a);
    ::usleep(20000);
    CHECK(mg::conf_put(cx, "global.ratio", "16/9"));
    CHECK(mg::conf_put(cx, "gb.nothing", "auto"));
    ::stat((dir + "/system.cfg").c_str(), &b);
    CHECK(a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec);
  }

  // ---- lists from the re-rooted tree (synchronous in dry-run)
  {
    bool loaded = true;
    mg::shader_list(&loaded);
    CHECK(!loaded);
    mg::load_shader_list();
    mg::load_filter_list();
    const auto& sh = mg::shader_list(&loaded);
    CHECK(loaded);
    CHECK_EQ(sh.size(), size_t(2));  // .slangp is not offered on this image (UI_SHADER = glslp)
    CHECK_EQ(sh[0], std::string("crt/crt-easymode.glslp"));
    CHECK_EQ(sh[1], std::string("sharp.glslp"));
    CHECK_EQ(mg::filter_list().size(), size_t(1));
    std::vector<std::string> g = mg::cpu_governors();
    CHECK_EQ(g.front(), std::string("default"));
    CHECK_EQ(g.size(), size_t(7));
    CHECK_EQ(mg::decoration_sets().size(), size_t(1));
    CHECK(mg::bin_exists("rocknix-systems"));
    CHECK(!mg::bin_exists("rocknix-nothing"));
    CHECK_EQ(std::string(mg::ratios().front().second), std::string("auto"));
  }

  // ---- emulator / core pick: explicit vs resolved, per game over per system
  {
    const System& gb = cx.systems[0];
    mg::Pick p = mg::explicit_pick(gb, cx.conf.map(), "gb");
    CHECK(p.emu.empty() && p.core.empty());
    CHECK(mg::conf_put(cx, "gb.core", "mgba"));
    p = mg::explicit_pick(gb, cx.conf.map(), "gb");
    CHECK_EQ(p.core, std::string("mgba"));
    mg::Pick r = mg::resolved_pick(gb, cx.conf.map(), "");
    CHECK_EQ(r.emu, std::string("libretro"));
    CHECK_EQ(r.core, std::string("mgba"));
    CHECK(mg::conf_put(cx, "gb[\"Tetris.gb\"].core", "gambatte"));
    r = mg::resolved_pick(gb, cx.conf.map(), "Tetris.gb");
    CHECK_EQ(r.core, std::string("gambatte"));
    r = mg::resolved_pick(gb, cx.conf.map(), "Other.gb");
    CHECK_EQ(r.core, std::string("mgba"));
    CHECK(mg::conf_put(cx, "gb.core", "bogus"));  // unknown core: not forced
    p = mg::explicit_pick(gb, cx.conf.map(), "gb");
    CHECK(p.core.empty());
    CHECK(mg::conf_put(cx, "gb.core", ""));
    CHECK(mg::conf_put(cx, "gb[\"Tetris.gb\"].core", ""));
    CHECK(cfg(dir).find("core") == std::string::npos);
  }

  // ---- the screens are registered
  for (const char* n : {"menu.game", "menu.game.systems", "menu.game.ai", "menu.game.bios", "menu.game.latency", "menu.game.sysopt", "menu.game.features",
                        "menu.system_options", "menu.game_options"})
    CHECK(Registry::instance().has(n));

  // ---- menu.game_options: entries, writes in the game's own key, hidden items, launch command
  {
    std::unique_ptr<Screen> s = Registry::instance().make("menu.game_options", cx, mg::game_arg("gb", "/roms/gb/Tetris.gb"));
    CHECK(s != nullptr);
    Menu* m = as_menu(s);
    m->rebuild(cx);
    CHECK_EQ(m->title(), std::string("Tetris"));
    int emu = find(*m, "EMULATOR");
    int ratio = find(*m, "GAME ASPECT RATIO");
    int smooth = find(*m, "SMOOTH GAMES (BILINEAR FILTERING)");
    int shader = find(*m, "SHADER SET");
    CHECK(emu >= 0 && ratio >= 0 && smooth >= 0 && shader >= 0);
    CHECK(find(*m, "REWIND") >= 0);
    CHECK(find(*m, "LATENCY REDUCTION") >= 0);
    CHECK(find(*m, "COLORIZATION") >= 0);
    CHECK(find(*m, "HIDDEN ONE") < 0);     // preset hidden
    CHECK(find(*m, "PALETTES") >= 0);      // a submenu of custom features
    CHECK(find(*m, "COLOUR BOOST") >= 0);  // shared feature, preset switch
    CHECK(find(*m, "INTERNAL RESOLUTION") < 0);  // the system does not offer it: hidden, not dimmed
    CHECK(find(*m, "FULL BOOT") < 0);

    // a choice writes `gb["Tetris.gb"].ratio`
    m->entry(size_t(ratio)).set_s("16/9");
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].ratio=16/9"));
    m->entry(size_t(ratio)).set_s("auto");
    CHECK(cfg(dir).find("Tetris.gb\"].ratio") == std::string::npos);
    m->entry(size_t(smooth)).set_s("0");
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].smooth=0"));
    CHECK_EQ(m->entry(size_t(smooth)).get_s(), std::string("0"));
    m->entry(size_t(smooth)).set_s("auto");
    // the shader list has AUTO, NONE and the presets
    bool has_crt = false;
    for (const Option& o : m->entry(size_t(shader)).options) has_crt |= o.value == "crt/crt-easymode.glslp";
    CHECK(has_crt);
    m->entry(size_t(shader)).set_s("sharp.glslp");
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].shaderset=sharp.glslp"));
    m->entry(size_t(shader)).set_s("auto");

    // colorization uses the key setsettings.sh reads (`<system>.renderer.colorization`), not ES's `-renderer`
    int col = find(*m, "COLORIZATION");
    m->entry(size_t(col)).set_s("GB - DMG");
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].renderer.colorization=GB - DMG"));
    m->entry(size_t(col)).set_s("auto");
    CHECK(cfg(dir).find("colorization") == std::string::npos);

    // the switch presets
    int boost = find(*m, "COLOUR BOOST");
    m->entry(size_t(boost)).set_b(true);
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].colour_boost=1"));
    m->entry(size_t(boost)).set_b(false);
    CHECK(cfg(dir).find("colour_boost") == std::string::npos);
    int on = find(*m, "SMOOTH ON");  // "switchon": on = no line, off = 0
    CHECK(m->entry(size_t(on)).get_b());
    m->entry(size_t(on)).set_b(false);
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].smoothon=0"));
    CHECK(!m->entry(size_t(on)).get_b());
    m->entry(size_t(on)).set_b(true);
    CHECK(cfg(dir).find("smoothon") == std::string::npos);
    int off = find(*m, "SMOOTH OFF");  // "switchoff": on = no line, off = 1
    m->entry(size_t(off)).set_b(false);
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].smoothoff=1"));
    m->entry(size_t(off)).set_b(true);
    CHECK(cfg(dir).find("smoothoff") == std::string::npos);

    // sliders: plain keeps the default, "auto" removes the line at its lowest step
    int vb = find(*m, "VOLUME BOOST");
    CHECK_EQ(m->entry(size_t(vb)).get_i(), 50);
    m->entry(size_t(vb)).set_i(70);
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].volboost=70"));
    CHECK_EQ(m->entry(size_t(vb)).get_i(), 70);
    int fs = find(*m, "FRAME SKIP");
    CHECK_EQ(m->entry(size_t(fs)).get_i(), -1);
    m->entry(size_t(fs)).set_i(2);
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].frameskip=2"));
    m->entry(size_t(fs)).set_i(-1);
    CHECK(cfg(dir).find("frameskip") == std::string::npos);

    // the submenu opens the features screen with the right target
    int pal = find(*m, "PALETTES");
    Transition t = m->entry(size_t(pal)).on_a(cx);
    CHECK(t.kind == Transition::Open && t.name == "menu.game.features");
    std::unique_ptr<Screen> fs_screen = Registry::instance().make(t.name, cx, t.arg);
    Menu* fm = as_menu(fs_screen);
    fm->rebuild(cx);
    int pi = find(*fm, "PALETTE");
    CHECK(pi >= 0);
    fm->entry(size_t(pi)).set_s("color");
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].palette=color"));
    fm->entry(size_t(pi)).set_s("");
    CHECK(cfg(dir).find("palette") == std::string::npos);

    // emulator + core picker: the pair is written, AUTO removes both, a launch command follows
    CHECK(m->entry(size_t(emu)).get_s().empty());
    std::vector<std::string> vals;
    for (const Option& o : m->entry(size_t(emu)).options) vals.push_back(o.value);
    CHECK(std::find(vals.begin(), vals.end(), "libretro/mgba") != vals.end());
    std::string before = launch::build_command(cx.systems[0], cx.systems[0].games[0], cx.conf.map(), {});
    CHECK(before.find("--core=gambatte") != std::string::npos);
    m->entry(size_t(emu)).set_s("libretro/mgba");
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].emulator=libretro"));
    CHECK(has_line(cfg(dir), "gb[\"Tetris.gb\"].core=mgba"));
    std::string after = launch::build_command(cx.systems[0], cx.systems[0].games[0], cx.conf.map(), {});
    CHECK(after.find("--core=mgba") != std::string::npos);
    // the other game of the system is not affected
    std::string other = launch::build_command(cx.systems[0], cx.systems[0].games[1], cx.conf.map(), {});
    CHECK(other.find("--core=gambatte") != std::string::npos);
    m->rebuild(cx);
    CHECK_EQ(m->entry(size_t(find(*m, "EMULATOR"))).get_s(), std::string("libretro/mgba"));
    m->entry(size_t(emu)).set_s("");
    CHECK(cfg(dir).find("emulator") == std::string::npos);
    CHECK(cfg(dir).find("core") == std::string::npos);
    std::string again = launch::build_command(cx.systems[0], cx.systems[0].games[0], cx.conf.map(), {});
    CHECK_EQ(again, before);
  }

  // ---- a change after the idle preparation made its marker invalidates it (runemu.sh compares the mtime of system.cfg)
  {
    spit(dir + "/marker", "x");
    ::usleep(20000);
    CHECK(mg::conf_put(cx, "gb.rewind", "1"));
    struct stat a, b;
    ::stat((dir + "/marker").c_str(), &a);
    ::stat((dir + "/system.cfg").c_str(), &b);
    bool newer = b.st_mtim.tv_sec > a.st_mtim.tv_sec || (b.st_mtim.tv_sec == a.st_mtim.tv_sec && b.st_mtim.tv_nsec > a.st_mtim.tv_nsec);
    CHECK(newer);
    CHECK(mg::conf_put(cx, "gb.rewind", ""));
  }

  // ---- menu.system_options uses `<system>.<key>`, and the ES order: emulator, system options, then the groups
  {
    std::unique_ptr<Screen> s = Registry::instance().make("menu.system_options", cx, "gb");
    Menu* m = as_menu(s);
    m->rebuild(cx);
    CHECK_EQ(m->title(), std::string("Game Boy"));
    int rw = find(*m, "REWIND");
    CHECK(rw >= 0);
    m->entry(size_t(rw)).set_s("1");
    CHECK(has_line(cfg(dir), "gb.rewind=1"));
    m->entry(size_t(rw)).set_s("auto");
    CHECK(cfg(dir).find("gb.rewind") == std::string::npos);
    CHECK(find(*m, "SYSTEM OPTIONS") >= 0);
    // a system with one core has no emulator picker
    std::unique_ptr<Screen> n = Registry::instance().make("menu.system_options", cx, "nes");
    Menu* nm = as_menu(n);
    nm->rebuild(cx);
    CHECK(find(*nm, "EMULATOR") < 0);
  }

  // ---- menu.game.sysopt: governors from sysfs, written as cpugovernor / gpuperf
  {
    std::unique_ptr<Screen> s = Registry::instance().make("menu.game.sysopt", cx, "gb");
    Menu* m = as_menu(s);
    m->rebuild(cx);
    int cpu = find(*m, "CPU SCALING GOVERNOR");
    CHECK(cpu >= 0);
    CHECK_EQ(m->entry(size_t(cpu)).options.size(), size_t(7));
    m->entry(size_t(cpu)).set_s("performance");
    CHECK(has_line(cfg(dir), "gb.cpugovernor=performance"));
    m->entry(size_t(cpu)).set_s("default");
    CHECK(cfg(dir).find("cpugovernor") == std::string::npos);
  }

  // ---- menu.game: ES `global.*` keys
  {
    std::unique_ptr<Screen> s = Registry::instance().make("menu.game", cx, "");
    Menu* m = as_menu(s);
    m->rebuild(cx);
    CHECK_EQ(m->title(), std::string("GAMES"));
    int as = find(*m, "AUTO SAVE/LOAD");
    CHECK(as >= 0);
    m->entry(size_t(as)).set_b(true);
    CHECK(has_line(cfg(dir), "global.autosave=1"));
    m->entry(size_t(as)).set_b(false);
    CHECK(cfg(dir).find("global.autosave") == std::string::npos);
    int ratio = find(*m, "GAME ASPECT RATIO");
    CHECK_EQ(m->entry(size_t(ratio)).get_s(), std::string("16/9"));  // the value that was already in system.cfg
    m->entry(size_t(ratio)).set_s("4/3");
    CHECK(has_line(cfg(dir), "global.ratio=4/3"));
    int inc = find(*m, "INCREMENTAL SAVESTATES");
    m->entry(size_t(inc)).set_s("2");
    CHECK(has_line(cfg(dir), "global.incrementalsavestates=2"));
    m->entry(size_t(inc)).set_s("");
    CHECK(cfg(dir).find("incremental") == std::string::npos);
    int sm = find(*m, "SHOW SAVESTATE MANAGER");
    m->entry(size_t(sm)).set_s("2");
    CHECK(has_line(cfg(dir), "global.savestates=2"));
    m->entry(size_t(sm)).set_s("auto");
    CHECK(cfg(dir).find("global.savestates") == std::string::npos);
    int vf = find(*m, "VIDEO FILTER");
    CHECK(vf >= 0);
    m->entry(size_t(vf)).set_s("Grid3x.filt");
    CHECK(has_line(cfg(dir), "global.videofilter=Grid3x.filt"));
    m->entry(size_t(vf)).set_s("auto");
    CHECK(find(*m, "AI GAME TRANSLATION") >= 0);
    CHECK(find(*m, "PER SYSTEM ADVANCED CONFIGURATION") >= 0);
    CHECK(find(*m, "EXTRA GLOBAL") >= 0);  // from <globalFeatures>
    CHECK(find(*m, "MISSING BIOS CHECK") >= 0);  // rocknix-systems exists in the tree
    // the ES setting that goes with the bios check
    int cb = find(*m, "CHECK BIOS FILES BEFORE RUNNING A GAME");
    CHECK(cb >= 0);
    CHECK(!cx.es.get_bool("CheckBiosesAtLaunch", false));
    CHECK(has_line(cfg(dir), "global.ratio=4/3"));
    m->entry(size_t(ratio)).set_s("16/9");
  }

  // ---- menu.game.systems: only systems that have something to configure
  {
    std::unique_ptr<Screen> s = Registry::instance().make("menu.game.systems", cx, "");
    Menu* m = as_menu(s);
    m->rebuild(cx);
    CHECK(find(*m, "Game Boy") >= 0);
    Transition t = m->entry(size_t(find(*m, "Game Boy"))).on_a(cx);
    CHECK(t.kind == Transition::Open && t.name == "menu.system_options" && t.arg == "gb");
  }

  // ---- menu.game.ai
  {
    std::unique_ptr<Screen> s = Registry::instance().make("menu.game.ai", cx, "");
    Menu* m = as_menu(s);
    m->rebuild(cx);
    int en = find(*m, "ENABLE AI TRANSLATION SERVICE");
    m->entry(size_t(en)).set_b(true);
    CHECK(has_line(cfg(dir), "global.ai_service_enabled=1"));
    m->entry(size_t(en)).set_b(false);
    CHECK(has_line(cfg(dir), "global.ai_service_enabled=0"));  // ES writes an explicit 0 here
    int lang = find(*m, "TARGET LANGUAGE");
    CHECK_EQ(m->entry(size_t(lang)).get_s(), std::string("En"));
    m->entry(size_t(lang)).set_s("Es");
    CHECK(has_line(cfg(dir), "global.ai_target_lang=Es"));
  }

  // ---- menu.game.latency
  {
    std::unique_ptr<Screen> s = Registry::instance().make("menu.game.latency", cx, "gb");
    Menu* m = as_menu(s);
    m->rebuild(cx);
    int ra = find(*m, "RUN-AHEAD FRAMES");
    CHECK(ra >= 0);
    m->entry(size_t(ra)).set_s("2");
    CHECK(has_line(cfg(dir), "gb.runahead=2"));
    m->entry(size_t(ra)).set_s("");
    CHECK(cfg(dir).find("runahead") == std::string::npos);
  }

  // ---- bios screen in dry-run does not run the script and says so
  {
    std::unique_ptr<Screen> s = Registry::instance().make("menu.game.bios", cx, "");
    Menu* m = as_menu(s);
    m->rebuild(cx);
    CHECK_EQ(m->size(), size_t(1));
    // `on_open` ran on the first handle/draw in the app; here the placeholder is enough
    spit(dir + "/root/tmp/rocknix-systems.out",
         "> Sega Saturn\nMISSING 85ec9ca47d8f6807718151cbcca8b964 bios/saturn_bios.bin\nINVALID - bios/sega_101.bin\n> PlayStation\nMISSING 924e392ed05558ffdb115408c263dccf bios/scph1001.bin\n");
    std::unique_ptr<Screen> s2 = Registry::instance().make("menu.game.bios", cx, "");
    Menu* m2 = as_menu(s2);
    m2->rebuild(cx);
    CHECK_EQ(m2->size(), size_t(1));  // not loaded until on_open runs
    m2->handle(Action::Down, cx);     // first handle runs on_open (dry-run: synchronous), then the refresh rebuilds
    m2->rebuild(cx);
    CHECK(find(*m2, "bios/saturn_bios.bin") >= 0);
    CHECK(find(*m2, "bios/scph1001.bin") >= 0);
    CHECK(find(*m2, "Sega Saturn") >= 0);
    CHECK(find(*m2, "bios/sega_101.bin") >= 0);
    CHECK_EQ(m2->entry(size_t(find(*m2, "bios/sega_101.bin"))).value_text, std::string("INVALID"));
  }

  // ---- the game list opens the options of the game under the cursor with Select
  {
    ListScreen ls(0, cx);
    Transition t = ls.handle(Action::Select, cx);
    CHECK(t.kind == Transition::Open && t.name == "menu.game_options");
    CHECK_EQ(t.arg, mg::game_arg("gb", "/roms/gb/Tetris.gb"));
  }

  // ---- write failure: a read-only system.cfg shows a toast and keeps going
  {
    SystemConf broken = SystemConf::from_text("a=1\n", dir + "/nodir/system.cfg");
    Ctx c2;
    c2.sv->cmd.dry_run = true;
    c2.conf = broken;
    CHECK(!mg::conf_put(c2, "gb.ratio", "4/3"));
    CHECK(!c2.sv->toasts.items().empty());
  }

  std::string rm = "rm -rf '" + dir + "'";
  if (std::system(rm.c_str()) != 0) {}
  return zt::done();
}
