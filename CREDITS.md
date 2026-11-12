# Zenith - credits and notices

Zenith is an unofficial fork of AURKNIX, which is itself a fork of ROCKNIX, which descends from JELOS and EmuELEC.
Zenith would not exist without them.

## Thanks

- **lcdyk** and **KK** (GitHub: AveyondFly), authors and maintainers of AURKNIX: the RK3326 device trees, emulators, tools and the whole
  base this fork stands on (https://github.com/AveyondFly/distribution_rocknix, upstream work by lcdyk0517).
  Support lcdyk on Ko-fi: https://ko-fi.com/lcdyk
- The **ROCKNIX** team (https://github.com/ROCKNIX) and the **JELOS** and **EmuELEC** teams before them.
- Everyone who builds and shares open source for these handhelds: emulator authors, kernel and driver developers,
  EmulationStation contributors, testers.

- **Jacob Cook** (SD resume retry and dwc2 suspend/resume fixes, ROCKNIX #3303/#3304) and **Chris Morgan** (RK817 battery
  gauge and sleep fixes, ROCKNIX #3305), whose kernel patches Zenith carries for suspend and resume.
- **KK** (AveyondFly), who ported the PX30 memory (DMC) frequency scaling from the Rockchip BSP
  (AveyondFly/distribution_rocknix commit `2c34156`, "rk3326: add dmc").
- **djparentx** ([dArkOSen-R36S](https://github.com/djparentx/dArkOSen-R36S), built from dArkOS): which R36S boards get the
  786 MHz memory step, and its 1.1 V, were read from its device trees.

## Maintainer

- zenmode-adri (https://github.com/zenmode-adri)

## Licenses

- Original ROCKNIX software and scripts: GNU GPL v2 (see `LICENSE.md` and `licenses/`).
  Copyright notices and license headers in the source files are kept as they are.
- Bundled components: each under its own license (see `licenses/` and each package).
  Some bundled components are licensed for non-commercial use only (see `LICENSE.md`).
- ROCKNIX branding and images: CC BY-NC-SA 4.0 (see `LICENSE.md`).
  The first-start screens and the boot banner of Zenith are Zenith's own.
  The early boot logo still comes from the upstream `rocknix-splash` package.
- Bundled EmulationStation themes: **Art Book Next** by Anthony Caccese (CC BY-NC-SA 2.0; the stock theme, unchanged except for
  the splash screen), **iconic-es** by Delgan (CC0), **es-theme-xmb-fcamod** by jovemlcxx (CC BY-NC-SA 4.0, with small layout
  fixes by zenmode-adri) and **techdweeb-es** by Anthony Caccese (CC BY-NC-SA 2.0). Each theme keeps its license (and a
  `NOTICE.md` where it is not stock) in `/usr/share/themes/<theme>/`. The Zenith splash screen used by all of them is
  Zenith's own.
- Zenith's own changes are published under the same licenses as the files they modify.

## Zenith's own work

All of it by zenmode-adri, on top of the base listed above:

- **zfront**, the default front-end: written from scratch in C++17 (about 28,000 lines of its own code), GPL v2. It reads
  the same settings, game lists and themes as EmulationStation and adds the continue-playing row, save backups, library
  check, play statistics, the battery screen and the phone file transfer. See `docs/ZFRONT.md` and the `NOTICE.md` of its
  sources, which lists what it owes to other projects.
- **Themes:** Zenith Minimal and Zenith Next for zfront, and the **Zenith** EmulationStation theme (light and dark), with
  the Tools icons, first-boot screens and boot banner. The EmulationStation theme uses the **Rubik** font (SIL Open Font
  License 1.1) and system logos from **iconic-es** by Delgan (CC0); the icon-only versions of eight of those logos were
  derived by removing the baked-in label.
- **R36 Tuner Next** (`projects/ROCKNIX/packages/apps/r36-tuner`): CPU, GPU and RAM clocks, voltage curves, profiles,
  benchmarks and a stability test.
- **Zenith DTB Selector** for Windows, Linux and macOS.
- **Boot and suspend:** the shorter boot (initramfs handing over to systemd, front-end and GPU starting in parallel) and
  the faster suspend (parallel steps, Wi-Fi reload around sleep).
- **Panfrost:** the 19 patches to Mesa 26.0.4 and the optimised build, with the tests in `docs/PANFROST-STUDY.md` and
  `docs/COMPILER-STUDY.md`. Mesa itself is under its own license.
- **Performance mode**, the low-battery shutdown, and everything else listed in `CHANGELOG.md`.
- EmulationStation patches (`.../emulationstation/patches/RK3326/`) and the shell scripts changed by Zenith are
  published under the license of the files they modify (GPL v2).

Zenith is not endorsed by, or affiliated with, ROCKNIX, AURKNIX or their authors.
