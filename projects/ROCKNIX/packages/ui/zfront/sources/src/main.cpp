// zfront entry point and test harness. Same options as the Rust prototype:
//   [--theme <dir>] [--size WxH] [--log-path <dir>] [--preview out.png] [--script a,b,c] [--cursor N] [--demo list]
//   [--systems es_systems.cfg] [--roms <root replacing /storage/roms>] [--conf system.cfg] [--es-settings es_settings.cfg] [--dry-run] [--dump] [--status ...]
//   [--fullscreen]
// Without --preview (and with SDL compiled in) it runs the real loop.
#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "app.hpp"
#include "bootargs.hpp"
#include "cpu.hpp"
#include "detailed.hpp"
#include "i18n.hpp"
#include "launch.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "pad.hpp"
#include "scene.hpp"
#include "screens.hpp"
#include "status.hpp"
#ifdef ZF_WITH_SDL
#include "sdlr.hpp"
#endif

using namespace zf;

namespace {

struct Args {
  std::string theme;     // empty: from ES's ThemeSet (boot::resolve_theme)
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

int run(int argc, char** argv, std::string& err) {
  Args args;
  if (!parse_args(argc, argv, args, err)) return 1;
  if (!args.log_path.empty() && !args.has_preview && !args.dump) boot::redirect_log(args.log_path);
  if (args.theme.empty()) {  // like ES: the theme comes from the ThemeSet setting
    const EsSettings tes = EsSettings::load(args.es_settings);
    const std::string* set = tes.get("ThemeSet");
    args.theme = boot::resolve_theme(set ? *set : std::string());
    if (args.theme.empty()) {
      err = "no encuentro el tema Zenith (ThemeSet='" + (set ? *set : std::string()) + "'): pasa --theme <carpeta del tema ya combinada>";
      return 1;
    }
  }
  Ctx cx;
  cx.w = float(args.w);
  cx.h = float(args.h);
#ifdef ZF_WITH_SDL
  if (!args.has_preview && !args.dump) oscontract::startup(cx.sv, args.dry_run);  // OS contract: bl_power=0, set_kill, SIGHUP
#endif
  if (!Theme::load(args.theme, ratio_name(args.w, args.h), cx.th, err)) return 1;
  if (!cx.assets.load_font(cx.th.path(cx.th.str("font")), err)) return 1;
#ifdef ZF_WITH_SDL
  if (!args.has_preview && !args.dump) oscontract::wait_roms();  // ES_WAIT_ROMS_READY, bounded
#endif
  if (!args.systems.empty()) {
    // device loop: first frame does not wait for the counts (they arrive from a background thread)
    bool lazy = args.lazy_counts || (!args.has_preview && !args.dump);
    if (!(lazy ? load_systems_lazy(args.systems, args.roms, cx.systems, err) : load_systems_index(args.systems, args.roms, cx.systems, err))) return 1;
  } else {
    cx.systems = demo_systems(args.demo);
    if (args.bench > 0 && args.demo == "list") {  // enough games to exercise the cache eviction
      std::vector<std::string> names;
      for (int i = 0; i < 240; ++i) names.push_back("Game " + std::to_string(i));
      cx.systems = {demo_system("gbc", "Game Boy Color", names)};
    }
  }
  detailed_set_demo(args.demo == "list");
  cx.conf = SystemConf::load(args.conf);
  i18n::init(cx.conf);  // tr() catalogs for system.language (English when there is none)
  cx.es = EsSettings::load(args.es_settings);
  cx.sv->cmd.dry_run = args.dry_run || args.has_preview;
  set_writes_blocked(cx.sv->cmd.dry_run);  // never write system.cfg / es_settings.cfg / gamelists / collections in preview or dry-run
  cx.sv->cmd.keep_log = args.has_preview;
  cx.async_covers = !args.has_preview && !args.dump;
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
  for (const System& s : cx.systems) cx.items.push_back(SystemItem{s.theme, s.fullname, uint32_t(s.count)});
  if (!load_common(cx.th, cx.assets, cx.common, err)) return 1;
  long n = long(cx.items.size());
  int start = int(n > 0 ? ((args.cursor % n) + n) % n : 0);
  std::unique_ptr<Pad> pad = make_pad("/storage/.config/emulationstation/es_settings.cfg");
  cx.pad = pad.get();
  App app(std::move(cx), std::make_unique<CarouselScreen>(start), make_status_source("/sys"));
  if (!args.has_preview) app.start_counts(2, size_t(start));
  if (!args.has_preview && !args.dump) Registry::instance().start_services(app);

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
  bool ran = sdl_run(app, pad.get(), args.w, args.h, args.fullscreen, err);
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
