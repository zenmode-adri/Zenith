# zfront (C++17)

Frontend for the Zenith console (RK3326, SDL2 + FreeType). Same architecture as the Rust skeleton, same harness, same
pixels. C++17, `-fno-exceptions -fno-rtti -ffp-contract=off` (identical output on x86_64 and aarch64), `-O2 -flto`.
Vendored code (stb_image, nanosvg, pugixml, libwebp decoder) is compiled in; only libSDL2, libfreetype, libstdc++, libc,
libm and libgcc_s are needed at run time (all already in the image).

## Build

    # PC (no SDL: harness, PNG preview and tests only)
    cmake -S cpp -B build/x86 -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build/x86 && ctest --test-dir build/x86
    # console (needs the ROCKNIX toolchain, see cmake/aarch64-rocknix.cmake)
    cmake -S cpp -B build/aarch64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/aarch64-rocknix.cmake

`src/*.cpp` is globbed into `zf_core` (so adding a file needs a `cmake .` re-run); every `tests/test_*.cpp` becomes a
ctest executable (`tests/check.hpp`: `CHECK`, `CHECK_EQ`, `zt::done()`).
FreeType headers come from the sysroot's `freetype2` directory only (never the whole sysroot include).

Install: `DESTDIR=<staging> cmake --install build/aarch64 --prefix /usr --strip` (binary, `zfront.mo` catalogs, merged Zenith theme,
docs). Choosing zfront or ES on the console, going back, and the OS patches: `docs/install.md`.

## Gaps closed after the menus (module `gaps-final`)

| what | where |
|---|---|
| launching a game from the savestate manager (or from a confirmation dialog) closes them: back on the list with the same cursor | `Screen::closes_on_launch`, `App::apply` (Launch) |
| `game-selected` (list: once the cursor rested 0,12 s, and on entry) / `system-selected` (carousel: when it stops, and on coming back); free when no script listens | `screens.cpp`, `oscontract::selected_listening` |
| `CheckBiosesAtLaunch`: `rocknix-systems --filter <system>`, YES/NO question when a BIOS file is MISSING | `launchcheck.*`, called by the list and the manager |
| `--theme` optional (from `ThemeSet`), `--log-path`, layout ratio follows the real output | `bootargs.*`, `main.cpp`, `sdlr.cpp` |
| saved Wi-Fi networks = iwd profiles (`/storage/.cache/iwd`), joined/forgotten with `iwctl`; open networks ask for a key like ES | `menu_network*.cpp` |

Not ported on purpose: `ShowControllerActivity` / `ShowControllerBattery` (ES draws a pad indicator in its top bar, zfront has none).

## Harness (identical to the Rust one)

    zfront --theme theme-dark --size 720x720 --preview out.png [--script right,A,down] [--cursor N]
           [--demo list] [--systems es_systems.cfg] [--roms ROOT] [--dump] [--conf system.cfg]
           [--status ..] [--cpu-filter triangle] [--fullscreen]

`cpp/tests/make_fixture.sh DIR` builds a fake ES tree (`DIR/es_systems.cfg`, `DIR/roms`, `DIR/system.cfg`).
`--dump` prints the launch command through `launch::build_command`: it differs from the Rust skeleton on purpose (ES escaping).

## Architecture

    main --> App (app.cpp): screen stack, transitions, Ctx (assets, theme, systems, pad, settings, status)
              |-- screens.cpp: CarouselScreen, ListScreen  (Screen::on_action / ops)
              |-- scene.cpp:   pure functions that turn state into `Op`s (system_view, list_view, status_bar, help_bar)
              |-- gfx.cpp:     Op (Rect/Image/Clip/Unclip) + Assets (LRU, pin, file_fit, text, svg)
              |-- cpu.cpp:     Op -> RGBA -> PNG (harness, tests)      sdlr.cpp: Op -> SDL textures (device, aarch64 only)

The device loop (`sdl_run`) draws only when something changed and then blocks in `SDL_WaitEvent*` until the next
deadline (status clock, pad repeat): no polling. Textures live per `ImgId`; ids are never reused.
Memory: `Assets::end_frame(ops, 24 MB)` evicts least-recently-drawn images that are neither pinned nor in the frame.

## Modules and owners (each agent edits only its own files)

| module | files | state in the skeleton |
|---|---|---|
| pad | `pad.*` | `Pad` interface with default virtuals, `make_pad` returns a null pad |
| status | `status.*` | `StatusSource`, `FixedStatusSource`, `parse_override` |
| gamelist | `gamelist.*` | `GameMeta`, `GameDetails`, DOM parse, `gamelist_details`; update functions return false |
| detailed | `detailed.*` | done: cover, name, meta rows, 5 stars, ES-style autoscroll description; 120 ms settle timer (`Screen::wake_in`, no polling); `--bench N`, `--advance S` |
| scan | `scan.*` | readdir + d_type, `scan_count_games`, `scan_list_games`, `scan_count_many` |
| launch | `launch.*` | escape/command/exit rules done (ported from `zf/launch`); `run` is plain `sh -c`; hooks are empty |
| core (shared) | `app`, `gfx`, `scene`, `screens`, `theme`, `systems`, `settings`, `input`, `util`, `text`, `cpu`, `sdlr` | ask before changing signatures |

Counts that arrive late (scan agent): call `App::set_count(system_index, n)`; the carousel redraws.
`System::count` comes from `load_systems_index` (no gamelist read); `System::open()` loads games lazily.

## Debts module (input, idle, covers, game data, memory)

| module | files | notes |
|---|---|---|
| pad (SDL fallback) | `pad.*`, `sdlr.cpp` | A pad with no `es_input.cfg` entry is mapped from its SDL_GameController mapping string (`PadConfig::from_sdl_mapping`, pure and tested). The log says `mapping source`: cfg / SDL / none. L2/R2/L3/R3 are real actions; in the carousel L2/R2 act as left/right; `InvertButtons` swaps A/B as for cfg pads. |
| idle | `idlewait.*`, `sdlwl.*` | `IdleWaiter` polls evdev fds + an eventfd (worker wake-ups) + the Wayland display fd (found by dlopen/dlsym of `wl_display_get_fd`, no libwayland link). With the display fd the idle cap is 1 s, without it 100 ms, with a key held 30 ms. The fd is never read or closed; a spin guard (64 wakes without a real event) drops it for 5 s. |
| covers | `covers.*` | Decoded on `cx.sv->jobs` (one low-priority worker); last request wins; a failed path is not retried; the result is added in `done` on the main thread. `Ctx::async_covers` is off for `--preview`/`--dump`, which stay deterministic. |
| game data | `gamemeta.*`, `gamelist.*` | `GameMeta::gametime` + `gamelist_update_after_play(..., edit, add_secs)`. `game_played` / `game_set_favorite` update memory at once, write gamelist.xml on a job and apply the span shift in `done`; a write failure toasts and reverts the favourite. |
| launch errors | `launch.*` | `launch::error_toast` for exit 200-205/250/299 (first line of `/tmp/launch_error.log`) shown through `cx.sv->toasts`. |
| memory | `gfx.*`, `sdlr.cpp` | After a texture is uploaded the CPU pixels are released (`Assets::release_pixels`, pinned images keep theirs). The budget still counts them (UMA). `forget_released` runs when the window or the render device is recreated, so the scenes rebuild by key. `text_wrap` no longer caches its line images. |

Decision: after a game the frontend does NOT reload gamelists from disk like ES does. It already knows what changed
(last played, play count, game time) and applies it in memory, then writes it on a job. A reload would redo the scan
and drop the cursor and cached covers for nothing. Edits made by other programs while a game runs are picked up on
the next start.

Memory figure (`--demo list --bench 240`, 720x720): process heap in use 25.2 MB -> 2.2 MB when textures are uploaded
(`--sim-gpu` simulates the upload; the real device does the same through `sdlr.cpp`).

Hardware checks pending (nothing here touches the console): a physical pad with no es_input.cfg entry (buttons,
d-pad, sticks, L2/R2/L3/R3, InvertButtons); idle wake-ups on Wayland (key press, worker finishing a cover, ~2 ticks per
10 s with nothing happening); a game that exits with an error (toast text); after a game: last played / count /
game time in gamelist.xml; a window or GPU reset (textures rebuilt, no black images).

## Translations

Every text the UI shows goes through `tr("ENGLISH KEY")` (`ntr` for plurals, `ctr` for a gettext context); a literal stored in a table or a
title is marked `N_("..")` / `NC_("ctx", "..")` and translated where it is shown. The key is the English text exactly as ES writes it, so
the catalogs ES already ships (`emulationstation2.mo`, 35 languages) translate everything they know. What ES does not have (about 350
texts: the scraper, controllers, system menus, error messages...) lives in zfront's own catalog, domain `zfront`, consulted first:
`locale/<lang>/LC_MESSAGES/zfront.po` -> `<build>/locale/<lang>/LC_MESSAGES/zfront.mo` (cmake runs `msgfmt`; installed to
`share/zfront/locale`, i.e. `/usr/share/zfront/locale` on the console; `ZF_LOCALE_DIR` overrides the search path). It is memory-mapped
lazily like ES's, so `tr()` stays allocation-free (+7 ns per lookup with the second catalog). Only Spanish is written; any other
language shows these texts in English (sentence case) while ES's own texts are still translated.

* New text in the code: write `tr("NEW TEXT")`, then run `tools/update-locale.py` (needs gettext): it refreshes `locale/zfront.pot`,
  appends the missing keys with an empty `msgstr` to every `zfront.po` and rewrites `locale/es-inherited.txt` (the keys ES translates in
  Spanish). `test_i18n_keys` scans `src/` and fails when a key has neither a zfront nor an ES Spanish translation, when a placeholder or
  line break is lost, when zfront.po repeats an ES translation or keeps an entry the code no longer uses (`--prune` removes them).
* New language: `tools/update-locale.py --new fr` (the folder name ES uses: `fr`, `de`, `pt_BR`...), translate the empty entries of
  `locale/fr/LC_MESSAGES/zfront.po`, rebuild. Keep the `%s`/`%d` placeholders and line breaks of the English text. An all-caps
  translation is shown in sentence case by `tr()`, as for ES.
* Fixed on purpose (not translated): brand and system names, file and protocol names, the language names, debug/log lines and the
  harness protocol.

## Adding a module or a screen

1. Header with the contract (small interface, plain structs), `.cpp` with a working stub.
2. New files in `src/` are picked up by the glob; add `tests/test_<name>.cpp` (picked up too).
3. A screen: subclass `Screen` in `screens.cpp` (or its own file), build ops with functions from `scene`, push it from
   `Ctx`/`Outcome`. Screens never touch SDL or the filesystem directly.
4. Compare with the oracle before and after: render the same `--theme/--size/--script` with both binaries and diff the PNGs.

## Pixel notes

Text uses ES's own layout (`Font.cpp`): line box = tallest ASCII glyph, baseline centred on 'S', glyph origin rounded.
This is deliberately closer to the console than the Rust prototype's fontdue, so diffs against the Rust oracle are
dominated by text, not geometry. Image rects are rounded like ES (`x0 = round(x)`, `x1 = round(x+w)`).
