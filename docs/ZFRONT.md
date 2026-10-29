# zfront

> Part of [Zenith](../README.md). The code lives in [`projects/ROCKNIX/packages/ui/zfront/sources`](../projects/ROCKNIX/packages/ui/zfront/sources).

**A small, fast frontend for the [Zenith](https://github.com/zenmode-adri/Zenith) handheld OS.**
It replaces EmulationStation's interface on RK3326 consoles with a from-scratch C++17 program that starts sooner, uses a
fraction of the memory, and sits idle when nothing is happening, while reading the very same files as EmulationStation.

> [!NOTE]
> **Status: working, measured on one device.** Everything below was developed and measured on the **R36 Ultra**
> (RK3326, Mali-G31, 1 GB RAM). EmulationStation is still Zenith's default frontend and stays installed as a fallback;
> zfront is opt-in. See [What is not verified yet](#what-is-not-verified-yet).

## Why a new frontend?

EmulationStation is a complete, mature program (about 153,000 lines of C++), but on a 1 GB handheld its weight shows:
slow first screen, a large resident footprint that is still there while you play, and a measurable idle load. zfront does
the same job with less.

### Measured against EmulationStation

All numbers: R36 Ultra, same image and theme, measured on the console on 25-26 October 2026 (one run each, not a
statistical benchmark; the real-game rows use Pokémon Yellow on RetroArch). Lower is better.

| | EmulationStation | zfront |
|---|---|---|
| First screen after start (large library) | 4.1 s | **1.0 s** |
| Frontend memory in the menus (PSS) | 143 MB | **36 MB** |
| Frontend memory while a game runs | 170 MB | **22.5 MB** (about 97 MB more free RAM) |
| CPU when idle (ticks per 10 s) | 35 | **2** |
| CPU while navigating the same route (ticks) | 831-864 | **210** |
| Launch a game: frontend to RetroArch | 1.18 s | **0.91 s** |

What this means in practice: a snappier start, more RAM left for emulators, and less battery spent drawing a menu that
is not changing. zfront draws only when something changed and then sleeps in `poll(2)` on the input devices, so a
resting menu costs next to nothing.

**What it does not do:** it does not make games run faster. In God of War (PPSSPP, the heaviest case we tested) the frame
rate was the same with ES and with zfront: that game is limited by the emulation thread and memory latency, not by the
frontend. The gain is in the frontend itself and in the memory it leaves free.

## Advantages

- **Fast and light.** C++17, `-O2 -flto`, no exceptions, no RTTI. Only SDL2, FreeType and the C/C++ runtime are linked
  (all already in the Zenith image). Images and text are cached with a memory budget, and CPU pixels are released after
  they are uploaded to the GPU.
- **Drop-in compatible.** It reads `es_systems.cfg`, `gamelist.xml`, `es_settings.cfg`, `system.cfg`, `es_input.cfg`,
  savestates and the Zenith theme. You can switch between ES and zfront at any time without losing anything.
- **Same hooks, same API.** User scripts in `emulationstation/scripts/<event>/` (`game-start`, `game-selected`,
  `system-selected`, `quit`...) keep working. A small HTTP API on `127.0.0.1:1234` lets the OS talk to the frontend
  (notifications, running game, shutdown, readiness).
- **Safe by design.** If zfront dies before drawing its first frame, the boot selector falls back to ES by itself, so a
  broken build never leaves the console without an interface. Switching back is one command.
- **Hardened.** Game names are escaped before they reach a shell, user hooks run without a shell, oversized or malformed
  XML and images are rejected or skipped, and a gamelist with an oversized entry is never rewritten. 58 test programs
  pass on a PC, and the affected ones were also run under ASan/UBSan.
- **Complete enough to live with.** System carousel and game lists with covers and details, savestate manager with
  "continue playing" row, favourites, collections, scraper client (needs a ScreenScraper developer id), screensaver,
  first-boot screens, and menus for network and Wi-Fi, Bluetooth and controllers, sound, display and per-game options.
  Light and dark Zenith themes at 480x320, 640x480, 720x405 and 720x720. Navigation sounds, smooth motion that turns
  itself off in power-saver mode, live reload of the system list.
- **Translated.** Every text goes through gettext. Texts that EmulationStation already translates (35 languages) reuse its
  catalogs; zfront's own texts have a Spanish catalog and fall back to English elsewhere.
- **Reproducible output.** `-ffp-contract=off` gives identical pixels on x86_64 and aarch64, so the screens can be
  rendered and tested on a PC without a device (`--preview out.png`).

## Using it

On a Zenith image that includes zfront:

```sh
set_setting system.frontend zfront ; systemctl restart essway    # use zfront
del_setting system.frontend        ; systemctl restart essway    # back to EmulationStation
```

The binary is `/usr/bin/zfront`; logs go to `/var/log/zfront.log`. Arguments can be changed with
`set_setting zfront.args "..."`. Details, the OS patches and the fallback logic are in [`docs/install.md`](../projects/ROCKNIX/packages/ui/zfront/sources/docs/install.md)
and the contract with the OS in [`docs/os-contract.md`](../projects/ROCKNIX/packages/ui/zfront/sources/docs/os-contract.md).

## Building

```sh
# PC: harness, PNG previews and tests (no SDL needed)
cmake -S sources -B build/x86 -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/x86 && ctest --test-dir build/x86

# Preview a screen without a device
build/x86/zfront --theme theme-dark --size 720x720 --preview out.png --script right,A,down

# Console: needs the ROCKNIX toolchain
cmake -S sources -B build/aarch64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=<ROCKNIX toolchain file>
```

In Zenith zfront is built as the `ui/zfront` package (`projects/ROCKNIX/packages/ui/zfront`); the source tree is the `sources/` folder next to its `package.mk`. Architecture, modules and
conventions are in [`README.md`](../projects/ROCKNIX/packages/ui/zfront/sources/README.md).

## What is not verified yet

Honest list, so nobody assumes more than we know:

- Measured only on the R36 Ultra; other RK3326 handhelds are untested.
- Not yet tested on hardware: a physical gamepad with no `es_input.cfg` entry, real sound output, emulators other than
  RetroArch and PPSSPP (flycast, drastic, mupen...), suspend from the menu, the real screensaver.
- The scraper is present but disabled until it has its own ScreenScraper developer id.
- The graphics stack still runs on Sway (Wayland). Dropping it for direct KMS is a possible later step.
- Only the Zenith theme is supported; other EmulationStation themes are not rendered.

## Credits and license

zfront is licensed under the **GNU GPL, version 2** (see [`LICENSE`](../projects/ROCKNIX/packages/ui/zfront/sources/LICENSE)). Copyright (C) 2026 zenmode-adri.

It is written from scratch, but it is built to behave like EmulationStation and borrows its file formats, layout
rules and some tables, so it owes a lot to **EmulationStation** (MIT, Alec Lofquist and later Batocera/ROCKNIX authors).
Third-party code that is compiled in (stb_image, nanosvg, pugixml, a libwebp decoder) and the full attribution are in
[`NOTICE.md`](../projects/ROCKNIX/packages/ui/zfront/sources/NOTICE.md). Zenith is an unofficial fork of ROCKNIX/AURKNIX and is not affiliated with them.
