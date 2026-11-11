// zfront entry point and test harness. Same options as the Rust prototype:
//   [--theme <dir> | --es-theme <ES theme dir> [--print-report]] [--size WxH] [--log-path <dir>] [--preview out.png] [--script a,b,c] [--cursor N] [--demo list]
//   [--systems es_systems.cfg] [--roms <root replacing /storage/roms>] [--conf system.cfg] [--es-settings es_settings.cfg] [--dry-run] [--dump] [--status ...]
//   [--fullscreen]
// Without --preview (and with SDL compiled in) it runs the real loop.
#include <malloc.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>

#include "app.hpp"
#include "bootargs.hpp"
#include "cpu.hpp"
#include "detailed.hpp"
#include "homemenu.hpp"
#include "i18n.hpp"
#include "launch.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "resume_last.hpp"
#include "snapshot.hpp"
#include "pad.hpp"
#include "savestates.hpp"
#include "scene.hpp"
#include "screens.hpp"
#include "status.hpp"
#include "syslist.hpp"
#include "players.hpp"
#include "uimode.hpp"
#include "util.hpp"
#ifdef ZF_WITH_SDL
#include "sdlr.hpp"
#endif

using namespace zf;

namespace {

struct Args {
  std::string theme;     // empty: from ES's ThemeSet (boot::resolve_theme)
  std::string es_theme;  // `--es-theme DIR`: an EmulationStation theme translated to a Zenith one (estheme::generate)
  std::string es_root;   // `--es-root DIR`: prefix for the Zenith base folders (/usr/share/zfront/theme-light|dark) the translation builds on (tests)
  bool print_report = false;  // `--print-report` (with --es-theme or an ES ThemeSet): print the translation coverage and exit
  std::string log_path;  // ES --log-path: stderr goes to <dir>/zfront.log
  uint32_t w = 720, h = 720;
  std::string preview;
  bool has_preview = false;
  std::vector<std::string> script;
  long cursor = 2;
  bool fullscreen = false;
  Sampling sampling = Sampling::CatmullRom;
  std::string demo;
  std::string systems, roms;
  bool dump = false;
  std::string conf = "/storage/.config/system/configs/system.cfg";
  std::string es_settings = "/storage/.emulationstation/es_settings.cfg";
  bool dry_run = false;  // menus run no commands and write no settings files (always on in --preview)
  std::string status;
  bool has_status = false;
  bool lazy_counts = false;  // preview/dump: show the "count pending" state of the real loop
  /// `--bench N`: N cursor moves with a render each; prints ms/frame, retained image bytes and first-render costs.
  long bench = 0;
  /// `--sim-gpu` (with `--bench`): release the CPU pixels of every image a frame drew, like the SDL loop after upload.
  bool sim_gpu = false;
  /// `--sim-reset`: before the final render, upload everything drawn (release CPU pixels) and then lose all textures, as a
  /// window recreation does (`Assets::forget_released`); the PNG must not change.
  bool sim_reset = false;
  /// `--advance S`: seconds the screen's timers run before the preview is rendered (description scroll).
  double advance = 0;
  /// `--toast TEXT`: a notice shown in the preview (toast placement is measured with it).
  std::string toast;
  /// `--home-panel`: not the frontend but the in-game HOME panel (homemenu.hpp): one menu, its answer is the exit code. Started by the
  /// frontend itself while the game is paused; `--home-states` adds the SAVE STATE / LOAD STATE rows.
  bool home_panel = false, home_states = false;
};

System demo_system(const std::string& t, const std::string& n, std::vector<std::string> games) {
  System s;
  s.name = t;
  s.fullname = n;
  s.theme = t;
  for (auto& g : games) {
    Game x;
    x.name = std::move(g);
    s.games.push_back(std::move(x));
  }
  s.count = s.games.size();
  return s;
}

/// Demo systems that match the capture of the console (2 games in Music), used when no `--systems` file is given.
/// `--demo list` has a single Game Boy Color system with the games of the reference capture.
std::vector<System> demo_systems(const std::string& kind) {
  auto n = [](size_t c) {
    std::vector<std::string> v;
    for (size_t i = 0; i < c; ++i) v.push_back("Juego " + std::to_string(i + 1));
    return v;
  };
  std::vector<System> out;
  if (kind == "list") {
    out.push_back(demo_system("gbc", "Game Boy Color",
                              {"Donkey Kong Country", "Dragon Warrior Monsters 2", "Harvest Moon 3 GBC", "Zelda: Link's Awakening", "Mario Golf", "Mario Tennis",
                               "Mega Man Xtreme", "Perfect Dark", "Shantae", "Rayman", "Super Mario Bros. Deluxe", "Tetris DX"}));
    return out;
  }
  out.push_back(demo_system("auto-favorites", "Favorites", n(0)));
  out.push_back(demo_system("psp", "PlayStation Portable", n(0)));
  out.push_back(demo_system("music", "Music", n(2)));
  out.push_back(demo_system("tools", "Tools", n(6)));
  return out;
}

/// Cost of the list column: first render of each game after the cursor settles (decode + wrap), steady frames, and the
/// image bytes still held after N cursor moves (the LRU keeps them under `kAssetBudget`).
/// `sim_gpu` (`--sim-gpu`): after every frame the images it drew are "uploaded" as `sdl_run` does (CPU pixels released), to
/// measure the RAM the device keeps.
void run_bench(App& app, long moves, bool sim_gpu) {
  auto ms = [](TimePoint a, TimePoint b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  auto render = [&]() -> const std::vector<Op>& {
    const std::vector<Op>& o = app.render();
    if (sim_gpu)
      for (const Op& op : o)
        if (op.kind == Op::Image) app.cx.assets.release_pixels(op.id);
    return o;
  };
  render();
  size_t floor_bytes = app.cx.assets.bytes();
  // 1. fast scrolling: the cursor moves every 50 ms, so the column is drawn without cover and description
  double fast = 0, first_sum = 0, first_max = 0, steady = 0;
  long first_n = 0;
  TimePoint t0 = Clock::now();
  for (long i = 0; i < moves; ++i) {
    app.handle((i / 11) % 2 == 0 ? Action::Down : Action::Up);
    app.tick(0.05f);
    render();
  }
  fast = ms(t0, Clock::now()) / double(moves);
  size_t held_fast = app.cx.assets.bytes();
  // 2. the cursor rests on every game once: the first render after the settle delay is the expensive one (cold caches)
  size_t most = 1;  // the system the script entered: assume the one with most games
  for (const System& sy : app.cx.systems) most = std::max(most, sy.games.size());
  long games = long(most);
  size_t peak = 0, wrapped_sum = 0, delta_sum = 0;
  double warm_sum = 0, evict_sum = 0;  // cold renders while the cache is under budget / once it evicts
  long warm_n = 0, evict_n = 0;
  for (long i = 0; i < games; ++i) {
    app.handle(Action::Down);
    app.tick(0.0f);
    render();  // unsettled frame
    app.tick(0.2f);
    size_t before = app.cx.assets.bytes();
    TimePoint a = Clock::now();
    const std::vector<Op>& ops = render();  // settled: decode + wrap
    double d = ms(a, Clock::now());
    first_sum += d, first_max = std::max(first_max, d), ++first_n;
    if (i < 8) warm_sum += d, ++warm_n;
    else if (app.cx.assets.bytes() > kAssetBudget * 9 / 10) evict_sum += d, ++evict_n;
    peak = std::max(peak, app.cx.assets.bytes());
    if (ops.size() > 3 && ops.back().kind == Op::Unclip) {
      const Img& wi = app.cx.assets.img(ops[ops.size() - 2].id);
      wrapped_sum += size_t(wi.w) * wi.h * 4;
    }
    if (i < 8) delta_sum += app.cx.assets.bytes() - before;
  }
  // 3. same game redrawn (a scroll step): everything cached
  TimePoint b0 = Clock::now();
  const long reps = 200;
  for (long i = 0; i < reps; ++i) render();
  steady = ms(b0, Clock::now()) / double(reps);
  struct mallinfo2 mi = mallinfo2();
  std::printf("bench: memory: images total %zu B (CPU copy still allocated %zu B), process heap in use %zu B (arena %zu B)%s\n", app.cx.assets.bytes(), app.cx.assets.cpu_bytes(),
              size_t(mi.uordblks), size_t(mi.arena + mi.hblkhd), sim_gpu ? " [--sim-gpu: pixels released after each frame]" : "");
  std::printf("bench: %ld games, per game avg: wrapped description image %zu B, bytes added by the first render %zu B (cover + wrapped description + texts)\n", games,
              wrapped_sum / size_t(std::max(games, 1L)), delta_sum / 8);
  std::printf("bench: cold render: first 8 games (under budget) %.3f ms, once the cache evicts %.3f ms; peak image bytes %zu\n", warm_sum / double(std::max(warm_n, 1L)),
              evict_sum / double(std::max(evict_n, 1L)), peak);
  std::printf("bench: %ld moves | unsettled frame %.3f ms | cold settled render (decode+wrap) avg %.3f ms max %.3f ms | cached redraw %.3f ms | image bytes: base %zu, after fast moves %zu, at end %zu (budget %zu)\n",
              moves, fast, first_sum / double(std::max(first_n, 1L)), first_max, steady, floor_bytes, held_fast, app.cx.assets.bytes(), kAssetBudget);
}

bool parse_args(int argc, char** argv, Args& a, std::string& err) {
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&](std::string& out) {
      if (i + 1 >= argc) {
        err = k + ": falta el valor";
        return false;
      }
      out = argv[++i];
      return true;
    };
    std::string v;
    if (k == "--theme") {
      if (!val(a.theme)) return false;
    } else if (k == "--es-theme") {
      if (!val(a.es_theme)) return false;
    } else if (k == "--es-root") {
      if (!val(a.es_root)) return false;
    } else if (k == "--print-report") {
      a.print_report = true;
    } else if (k == "--log-path") {
      if (!val(a.log_path)) return false;
    } else if (k == "--size") {
      if (!val(v)) return false;
      size_t x = v.find('x');
      long ww = 0, hh = 0;
      if (x == std::string::npos || !parse_int(v.substr(0, x), ww) || !parse_int(v.substr(x + 1), hh) || ww <= 0 || hh <= 0) {
        err = "--size WxH";
        return false;
      }
      a.w = uint32_t(ww);
      a.h = uint32_t(hh);
    } else if (k == "--preview") {
      if (!val(a.preview)) return false;
      a.has_preview = true;
    } else if (k == "--script") {
      if (!val(v)) return false;
      size_t p = 0;
      while (p <= v.size()) {
        size_t e = v.find(',', p);
        if (e == std::string::npos) e = v.size();
        std::string_view s = trim(std::string_view(v).substr(p, e - p));
        if (!s.empty()) a.script.emplace_back(s);
        p = e + 1;
      }
    } else if (k == "--cursor") {
      if (!val(v)) return false;
      if (!parse_int(v, a.cursor)) {
        err = "--cursor";
        return false;
      }
    } else if (k == "--cpu-filter" && i + 1 < argc) {
      a.sampling = std::string(argv[++i]) == "triangle" ? Sampling::Triangle : Sampling::CatmullRom;
    } else if (k == "--fullscreen") {
      a.fullscreen = true;
    } else if (k == "--demo") {
      if (!val(a.demo)) return false;
    } else if (k == "--systems") {
      if (!val(a.systems)) return false;
    } else if (k == "--roms") {
      if (!val(a.roms)) return false;
    } else if (k == "--lazy-counts") {
      a.lazy_counts = true;
    } else if (k == "--dump") {
      a.dump = true;
    } else if (k == "--status") {
      if (!val(a.status)) return false;
      a.has_status = true;
    } else if (k == "--conf") {
      if (!val(a.conf)) return false;
    } else if (k == "--es-settings") {
      if (!val(a.es_settings)) return false;
    } else if (k == "--home-panel") {
      a.home_panel = true;
    } else if (k == "--home-states") {
      a.home_states = true;
    } else if (k == "--dry-run") {
      a.dry_run = true;
    } else if (k == "--sim-gpu") {
      a.sim_gpu = true;
    } else if (k == "--sim-reset") {
      a.sim_reset = true;
    } else if (k == "--bench") {
      if (!val(v) || !parse_int(v, a.bench)) {
        err = "--bench N";
        return false;
      }
    } else if (k == "--toast") {
      if (!val(a.toast)) return false;
    } else if (k == "--advance") {
      if (!val(v)) return false;
      float f = 0;
      if (!parse_float(v, f)) {
        err = "--advance S";
        return false;
      }
      a.advance = double(f);
    } else {
      err = "opcion desconocida: " + k;
      return false;
    }
  }
  return true;
}

/// The boot game as a press on it would launch it: the POST /launch lookup (system and game by path, the command with the controllers
/// the device has now) plus the savestate arguments of a plain launch. Nothing when no system holds it.
std::optional<Outcome> startup_outcome(App& app, const std::string& rom) {
  Outcome o;
  if (!oscontract::resolve_launch(app.cx, rom, o)) {
    std::fprintf(stderr, "zfront: startup game: no system holds %s, or it has no command\n", rom.c_str());
    return std::nullopt;
  }
  if (size_t(o.sys) < app.cx.systems.size() && size_t(o.game) < app.cx.systems[size_t(o.sys)].games.size())  // an interrupted autosave backup first
    savestates::recover(app.cx.systems[size_t(o.sys)], app.cx.systems[size_t(o.sys)].games[size_t(o.game)]);
  o.cmd += savestates::plain_args(app.cx, size_t(o.sys), size_t(o.game));
  return o;
}

#ifdef ZF_WITH_SDL
// Instant start (snapshot.hpp): turns on the picture for the next start and, when the saved one is for this theme, size, language
// and build, shows it now. Before the theme and the systems are loaded; a corrupt or missing file is simply ignored.
void snapshot_start(const Args& args) {
  if (args.dry_run || !args.script.empty() || args.systems.empty()) return;  // harness runs write and show nothing
  const EsSettings es = EsSettings::load(args.es_settings);
  const SystemConf conf = SystemConf::load(args.conf);
  snapshot::Key key;
  key.theme = args.es_theme + "|" + args.theme;
  if (const std::string* set = es.get("ThemeSet")) key.theme_set = *set;
  key.lang = conf.get_or("system.language", "");
  key.build = snapshot::build_id();
  key.w = args.w;
  key.h = args.h;
  sdl_set_snapshot_key(key);  // the picture for the next start is taken whether or not one is shown now
  // A game starts by itself (the boot game, or "resume last game" with its countdown): the carousel would flash for nothing.
  if (!conf.get_or(boot::kBootGamePath, "").empty() || conf.get_or(resume::kKey, "") == "1") return;
  std::string why;
  const std::optional<snapshot::Image> im = snapshot::load(snapshot::dir(), key, why);
  if (!im) {
    std::fprintf(stderr, "zfront: no snapshot shown (%s)\n", why.c_str());
    return;
  }
  sdl_splash(*im, args.w, args.h, args.fullscreen);
}
#endif

int run(int argc, char** argv, std::string& err) {
  Args args;
  if (!parse_args(argc, argv, args, err)) return 1;
  if (!args.log_path.empty() && !args.has_preview && !args.dump) boot::redirect_log(args.log_path);
  // a previous zfront that died while it had a game paused (HOME menu): resume what it left stopped. Free when there is no file.
  if (!args.home_panel && !args.has_preview && !args.dump) homemenu::recover_stopped(homemenu::stopped_file_path(), [](int pid, int sig) { return ::kill(pid_t(pid), sig); });
  homemenu::set_panel_mode(args.home_panel);
  Ctx cx;
#ifdef ZF_WITH_SDL
  if (!args.has_preview && !args.dump && !args.home_panel) {  // the panel is a guest: no OS contract (it would rewrite the kill target)
    oscontract::startup(cx.sv, args.dry_run);  // OS contract: bl_power=0, set_kill, SIGHUP
    if (args.fullscreen) sdl_output_size(args.w, args.h);  // when Sway is already up an ES theme is translated and the theme loaded once, for the real output
    snapshot_start(args);
  }
#endif
  bool theme_from_set = false;
  {  // an ES theme (`--es-theme`, or the ThemeSet names one): translated to a generated Zenith-structured folder
    std::string es_dir = args.es_theme;
    EsSettings tes = EsSettings::load(args.es_settings);
    if (es_dir.empty() && args.theme.empty()) {
      const std::string* set = tes.get("ThemeSet");
      es_dir = boot::resolve_es_theme(set ? *set : std::string());
      theme_from_set = !es_dir.empty();
    }
    if (!es_dir.empty()) {
      std::string gen, report, why;
      auto lookup = [&](const std::string& k) { return tes.get(k); };
      if (boot::translate_es_theme(es_dir, args.w, args.h, lookup, args.es_root, {}, gen, report, why)) {
        if (args.print_report) {
          std::printf("ES theme %s -> %s\n%s", es_dir.c_str(), gen.c_str(), report.c_str());
          return 0;
        }
        args.theme = gen;
      } else if (theme_from_set) {
        std::fprintf(stderr, "zfront: no puedo traducir el tema ES '%s' (%s)\n", es_dir.c_str(), why.c_str());
        args.theme = es_dir;  // not drawable: pick_theme falls back to Zenith with a notice
      } else {
        err = "--es-theme " + es_dir + ": " + why;
        return 1;
      }
    }
  }
  if (args.theme.empty()) {  // like ES: the theme comes from the ThemeSet setting
    const EsSettings tes = EsSettings::load(args.es_settings);
    const std::string* set = tes.get("ThemeSet");
    args.theme = boot::resolve_theme(set ? *set : std::string());
    theme_from_set = true;
    if (args.theme.empty()) {
      err = "no encuentro el tema Zenith (ThemeSet='" + (set ? *set : std::string()) + "'): pasa --theme <carpeta del tema ya combinada>";
      return 1;
    }
  }
  cx.w = float(args.w);
  cx.h = float(args.h);
  // A theme that does not load, lacks essential variables or has no font is never drawn: with the theme chosen in ThemeSet this
  // falls back to Zenith (the frontend is restarted by systemd, so exiting here would loop forever); `--theme` stays strict.
  auto theme_problem = [&](const std::string& dir, std::string& why) {
    Theme t;
    if (!Theme::load(dir, ratio_name(args.w, args.h), t, why)) return true;
    const std::string miss = t.missing();
    if (!miss.empty()) {
      why = "falta la variable '" + miss + "'";
      return true;
    }
    if (!file_exists(t.path(t.str("font")))) {
      why = "falta la fuente '" + std::string(t.str("font")) + "'";
      return true;
    }
    return false;
  };
  bool theme_fallback = false;
  {
    std::string used;
    if (!boot::pick_theme(args.theme, theme_from_set ? boot::resolve_theme("") : std::string(), theme_problem, used, theme_fallback, err)) return 1;  // err says why
    if (theme_fallback) std::fprintf(stderr, "zfront: el tema '%s' no sirve (%s): uso Zenith\n", args.theme.c_str(), err.c_str());
    err.clear();
    args.theme = used;
  }
  if (!Theme::load(args.theme, ratio_name(args.w, args.h), cx.th, err)) return 1;
  if (!cx.assets.load_font(cx.th.path(cx.th.str("font")), err, cx.th.font_fallbacks())) return 1;  // fontFallback: the theme's extra faces
  if (const std::string_view alt = cx.th.str("esFontAlt", ""); !alt.empty()) {  // a remade ES theme: its face for titles
    std::string why;
    if (!cx.assets.load_alt_font(std::string(alt), why)) std::fprintf(stderr, "zfront: %s\n", why.c_str());
  }
#ifdef ZF_WITH_SDL
  if (!args.has_preview && !args.dump && !args.home_panel) oscontract::wait_roms();  // ES_WAIT_ROMS_READY, bounded
#endif
  cx.es = EsSettings::load(args.es_settings);
  players::set(players::read(cx.es));
  uimode::apply(cx.es);  // Kiosk / Kid decide which games and systems are loaded
  // before the systems: LoadEmptySystems, HiddenSystems and SortSystems decide what the carousel shows
  if (!args.systems.empty()) {
    // device loop: first frame does not wait for the counts (they arrive from a background thread)
    bool lazy = args.lazy_counts || (!args.has_preview && !args.dump);
    // The same loader serves the hot reload (`App::maybe_reload_systems`): it captures only paths and the mode, never `cx`.
    auto loader = [cfg = args.systems, roms = args.roms, lazy](bool load_empty, std::vector<System>& out, std::string& e) {
      return lazy ? load_systems_lazy(cfg, roms, out, e, load_empty) : load_systems_index(cfg, roms, out, e, load_empty);
    };
    if (!loader(cx.es.get_bool("LoadEmptySystems", false), cx.systems, err)) return 1;
    if (!args.dump) cx.system_loader = std::move(loader);
  } else if (!args.home_panel) {
    cx.systems = demo_systems(args.demo);
    if (args.bench > 0 && args.demo == "list") {  // enough games to exercise the cache eviction
      std::vector<std::string> names;
      for (int i = 0; i < 240; ++i) names.push_back("Game " + std::to_string(i));
      cx.systems = {demo_system("gbc", "Game Boy Color", names)};
    }
  }
  detailed_set_demo(args.demo == "list");
  cx.conf = SystemConf::load(args.conf);
  boot::apply_user_theme(cx.th, [&](const std::string& k) { return cx.conf.get_or(k, ""); });  // the user's choices over the theme's own
  i18n::init(cx.conf);  // tr() catalogs for system.language (English when there is none)
  if (theme_fallback) cx.sv->toasts.push(tr("The chosen theme could not be loaded: using Zenith"));
  cx.sv->cmd.dry_run = args.dry_run || args.has_preview;
  set_writes_blocked(cx.sv->cmd.dry_run);  // never write system.cfg / es_settings.cfg / gamelists / collections in preview or dry-run
  cx.sv->cmd.keep_log = args.has_preview;
  cx.async_covers = !args.has_preview && !args.dump;
  {  // window / screen motion only on the device, and not when ES is set to instant transitions or to the instant power saver
    const std::string* style = cx.es.get("TransitionStyle");
    const std::string* saver = cx.es.get("PowerSaverMode");
    cx.ui_motion = cx.async_covers && !(style && *style == "instant") && !(saver && *saver == "instant");
  }
  if (args.dump) {
    for (size_t i = 0; i < cx.systems.size(); ++i) {
      cx.open_system(i);
      const System& s = cx.systems[i];
      std::printf("%s [%s] theme=%s games=%zu emu=%s/%s\n", s.name.c_str(), s.fullname.c_str(), s.theme.c_str(), s.games.size(), s.emulator.c_str(), s.core.c_str());
      if (!s.games.empty()) {
        const Game& g = s.games.front();
        std::printf("    %s -> %s\n", g.name.c_str(), launch::build_command(s, g, cx.conf.map(), {}).c_str());
      }
    }
    return 0;
  }
  cx.items = make_items(cx.systems, cx.es);
  if (!load_common(cx.th, cx.assets, cx.common, err)) return 1;
  long n = long(cx.items.size());
  int start = int(n > 0 ? ((args.cursor % n) + n) % n : 0);
  std::unique_ptr<Pad> pad = make_pad(args.es_settings);  // the file the menu writes (--es-settings), not a fixed path
  cx.pad = pad.get();
  // ES launchStartupGame: the game set in system.cfg starts once per boot, before the menus (bootargs.cpp, startup_outcome)
  std::function<std::optional<Outcome>(App&)> startup;
  if (boot::startup_game_allowed(args.has_preview, args.dry_run, args.dump, !args.script.empty() || args.systems.empty())) {
    std::string why;
    const std::string rom = boot::startup_game(cx.conf.get_or(boot::kBootGamePath, ""), boot::startup_marker(),
                                               [](const std::string& p) { return file_exists(p); }, why);
    if (!why.empty()) std::fprintf(stderr, "zfront: %s\n", why.c_str());
    if (!rom.empty()) {
      startup = [rom](App& app) { return startup_outcome(app, rom); };
    } else {  // no boot game: "resume last game at startup" (off by default; resume_last.hpp)
      startup = [](App& app) -> std::optional<Outcome> {
        std::string why2;
        app.push_screen(resume::begin(app.cx, resume::default_paths(), std::time(nullptr), why2));
        if (!why2.empty()) std::fprintf(stderr, "zfront: %s\n", why2.c_str());
        return std::nullopt;
      };
    }
  }
  std::unique_ptr<Screen> root;
  if (args.home_panel) {
    root = Registry::instance().make("menu.home", cx, args.home_states ? "states" : "");
    if (!root) {
      err = "--home-panel: no menu.home";
      return 1;
    }
  } else {
    root = std::make_unique<CarouselScreen>(start);
  }
#ifdef ZF_WITH_SDL
  if (!args.home_panel && !args.has_preview && !args.dump) {  // how the in-game HOME menu starts its panel: this binary, same theme, files and screen
    char exe[512];
    ssize_t en = ::readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (en > 0) {
      exe[en] = 0;
      std::vector<std::string> pa = {"--theme", args.theme, "--conf", args.conf, "--es-settings", args.es_settings};
      if (args.fullscreen) pa.push_back("--fullscreen");
      pa.push_back("--size");
      pa.push_back(std::to_string(args.w) + "x" + std::to_string(args.h));
      homemenu::set_panel_args(exe, pa);
    }
  }
#endif
  App app(std::move(cx), std::move(root), make_status_source("/sys"));
  if (!args.has_preview && !args.home_panel) app.start_counts(2, size_t(start));
  if (!args.home_panel) app.refresh_collections(args.has_preview);  // the enabled collections join the carousel (a job; at once in the harness)
  if (!args.has_preview && !args.dump && !args.home_panel) Registry::instance().start_services(app);

  if (args.has_preview) {
    // deterministic bar for captures: fixed demo values unless --status overrides some of them
    Status st = Status::demo(args.demo == "list" ? "07:13" : "12:04");
    if (args.has_status && !Status::parse_override(args.status, st, st, err)) return 1;
    app.freeze_status(st);
    for (const std::string& name : args.script) {
      auto a = action_from_name(name);
      if (!a) {
        err = "--script: accion desconocida '" + name + "'";
        return 1;
      }
      Outcome o = app.handle(*a);
      if (o.kind == Outcome::Launch) std::fprintf(stderr, "zfront: (preview) lanzaria: %s\n", o.cmd.c_str());
      if (o.kind == Outcome::Quit) break;
      app.tick(1.0f);  // let animations finish
    }
    for (double left = args.advance; left > 0;) {  // run the screen timers like the loop would (render only when due)
      app.render();
      std::optional<TimePoint> dl = app.screen_deadline();
      float dt = 0.5f;
      if (dl) dt = std::max(std::chrono::duration<float>(*dl - Clock::now()).count(), 0.001f);
      dt = float(std::min<double>(dt, left));
      app.tick(dt);
      left -= double(dt);
    }
    if (!args.toast.empty()) app.cx.sv->toasts.push(args.toast, 600000);
    if (args.bench > 0) run_bench(app, args.bench, args.sim_gpu);
    if (args.sim_reset) {
      for (const Op& op : app.render())
        if (op.kind == Op::Image) app.cx.assets.release_pixels(op.id);
      app.cx.assets.forget_released();
    }
    const std::vector<Op>& ops = app.render();
    Canvas cv = cpu_render(ops, app.cx.assets, args.w, args.h, args.sampling);
    return save_png(cv, args.preview, err) ? 0 : 1;
  }
#ifdef ZF_WITH_SDL
  if (args.has_status) {
    Status st;
    if (!Status::parse_override(args.status, app.cx.status, st, err)) return 1;
    app.freeze_status(st);
  }
  bool ran = sdl_run(app, pad.get(), args.w, args.h, args.fullscreen, err, std::move(startup));
  if (args.home_panel) return ran ? homemenu::exit_code_of(homemenu::panel_result()) : 1;  // the answer; no OS contract to finish
  oscontract::finish();  // panel on, ready file removed, pending quit mode (reboot/shutdown) performed
  return ran ? 0 : 1;
#else
  err = "compilado sin SDL; usa --preview";
  return 1;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  std::string err;
  int rc = run(argc, argv, err);
  if (rc != 0 && !err.empty()) std::fprintf(stderr, "zfront: %s\n", err.c_str());
  return rc;
}
