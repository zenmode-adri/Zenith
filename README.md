# Zenith

**A fast, polished operating system for RK3326 handhelds.** Zenith is its own system image: Linux 6.12 mainline,
EmulationStation with a custom light/dark theme, and a boot and suspend path rebuilt from the ground up. It is an
**unofficial fork** of [AURKNIX](https://github.com/AveyondFly/distribution_rocknix), which is a fork of
[ROCKNIX](https://github.com/ROCKNIX), and it is developed and measured on the **R36 Ultra**.

- **Boots in about 15 s instead of 33 s**, suspends in 2.3 s and wakes in 4.3 s.
- **Opens games in 3.85 s** instead of 6.29 s, and the menus answer on the first button press (11 ms).
- **Own interface**: light and dark theme, redesigned dialogs, keyboard, sliders, a clean Tools menu, first-boot screens.
- **Mainline kernel**: Linux 6.12 with upstream drivers, plus a patch set for the RK3326.
- **Measured, not guessed**: newer emulator builds, PPSSPP versions and GPU clocks were benchmarked on the device, and what
  did not help was left out (see [Tested and not shipped](#tested-and-not-shipped)).

Maintained by [zenmode-adri](https://github.com/zenmode-adri). See [`CREDITS.md`](CREDITS.md) for the people and projects
this work stands on, and [`CHANGELOG.md`](CHANGELOG.md) for every change and its measurement.

> **Tested only on the R36 Ultra.** Other RK3326 handhelds use the same image and device-tree selector but have not been
> checked with these changes. Targets other than RK3326 are inherited from upstream and were not built or tested.
> Zenith is not endorsed by, or affiliated with, ROCKNIX, AURKNIX or their authors. Please do not report Zenith problems
> to them.

## Screenshots

R36 Ultra, 720x720. Dark and light variants of the Zenith theme.

| Start-up | Main menu (dark) | Main menu (light) |
|:---:|:---:|:---:|
| <img src="docs/images/splash.png" width="230"> | <img src="docs/images/main-dark.png" width="230"> | <img src="docs/images/main-light.png" width="230"> |
| **Game grid (dark)** | **Game grid (light)** | **Settings menu** |
| <img src="docs/images/grid-dark.png" width="230"> | <img src="docs/images/grid-light.png" width="230"> | <img src="docs/images/menu-dark.png" width="230"> |

The game grids use a small demo set of Game Boy Color games with box art; Zenith does not ship any games or covers. The grid shows the `thumbnail` (box art) that the EmulationStation scraper stores for each game.

## What's different from AURKNIX

Measured on an **R36 Ultra**. Details and the git tag of every change are in [`CHANGELOG.md`](CHANGELOG.md).

| | Before | Zenith |
|---|---:|---:|
| Power-on to menu | 33 s | **15 s** |
| Opening a game | 6.29 s | **3.85 s** |
| Suspend | 3.5 s | **2.3 s** |
| Resume | 8 s | **4.3 s** |
| First button press after idle | 225-245 ms | **11 ms** |

*Before* is stock AURKNIX for boot (stopwatch) and for opening a game; for suspend, resume and the first button press it is
an earlier Zenith build, because stock AURKNIX was not measured for those. Zenith's boot time comes from the boot log plus
the bootloader (3.11 s -> 2.11 s).

**Boot and power**
- GPU, display, Sway and EmulationStation start in parallel as soon as storage is ready; u-boot `BOOTDELAY=0`; faster SD
  probing in the initramfs.
- Suspend runs Wi-Fi shutdown, panel and the rest in parallel; governors are restored first on wake-up.
- Silent shutdown (no red message) and quiet boot.

**Interface**
- Own light and dark EmulationStation theme, readable card labels and higher contrast.
- Redesigned switches, sliders, keyboard, dialogs, scrollbars and Information screen (ES patches 0001-0054).
- System carousel with equal 22 px gaps, no jumping neighbours and a steady speed when holding a direction.
- System / Network Settings open at once and fill their lists in the background; faster Wi-Fi menu.
- Fewer confirmation prompts in the quit menu.
- Tools menu with an icon and a description for every tool; 15 dead entries removed (ES warnings per boot 15 -> 0).
- Own first-boot screens, language question and boot banner.

**R36 Ultra hardware**
- RK915 Wi-Fi that survives reboots and real suspend, joystick RGB LED driver, RK817 battery gauge, CPU/GPU voltage bins
  L0-L3 through PVTM, GPU table 200-600 MHz.
- **R36 Tuner Next** built in: CPU governor and frequencies, turbo steps, benchmarks, monitor.

**Not the goal:** Zenith adds no new emulators and does not update cores; those come from AURKNIX. The work is on boot,
suspend, responsiveness and the interface.

## Under the hood

**Kernel.** Zenith runs **Linux 6.12 mainline** (6.12.79 in the current image), with upstream drivers for the SoC, storage and
power management, on a long-term-support base. The 6.12 base comes from AURKNIX/ROCKNIX. The patch set on top of it is in
`projects/ROCKNIX/devices/RK3326/patches/linux/` (part inherited, part Zenith's own, see the changelog) and includes, among
others:

- CPU/GPU voltage bins L0-L3 read through PVTM, an RK3326 OPP table (CPU 1296 MHz by default, with 1416 and 1512 MHz as an
  opt-in turbo; GPU 200-600 MHz) and memory devfreq.
- Stable RK915 Wi-Fi across reboots and real suspend, and the RK817 battery gauge fixes.
- The R36 Ultra joystick RGB LED driver and a panel brightness control.
- An exFAT directory-readahead limit.

The kernel configuration was reviewed for performance: it is already lightweight (`-O2`, no debugging or tracing,
zram and MGLRU enabled), and the Cortex-A35 reports "not affected" for almost every speculative-execution mitigation, so
there is no security tax to remove.

**Boot.** A small initramfs probes the SD card every 50 ms and hands over to systemd with `exec`; GPU, display, Sway and
EmulationStation then start in parallel as soon as storage is ready, and the early start disables itself if EmulationStation
is not ready in time. u-boot runs with `BOOTDELAY=0`.

**EmulationStation.** 54 patches on top of `ROCKNIX/emulationstation-next` (`projects/ROCKNIX/packages/ui/emulationstation/patches/RK3326/`)
cover the interface, the carousel, audio wake-up, the quit menu and the Wi-Fi and settings menus.

## Tested and not shipped

Every claim below was measured on the R36 Ultra. The point of Zenith is speed that you can feel, so a change that did not
help was not shipped.

**Newer emulator cores (ROCKNIX `next` branch).** 19 libretro cores were rebuilt from the `next` branch and compared with
the installed ones. The main comparisons used 32 ROMs with 4 repetitions each (RetroArch 1.22.2, same governor, temperature
capped at 68 °C); only CPU time was measured, not audio or save states.

| Core | CPU per frame, old -> new | Verdict |
|---|---|---|
| snes9x, genesis_plus_gx, picodrive, gambatte, vba_next, quicknes | -1.4 % to +1.1 % | Same speed, so no reason to change |
| nestopia (default NES core) | +19 % | More CPU (on all 6 ROMs), so not updated |
| fceumm | +13 % | More CPU, so not updated |
| gearsystem (Game Gear / Master System) | -26 % | The only clear gain |
| mGBA new vs mGBA old | -42 % | Better than before; the default gpSP (GBA) and gambatte (GB/GBC) still use less CPU, so the defaults stay |

In these tests newer builds did not bring more performance in general: most are equivalent, two use more CPU, and mGBA
improves on its older version but not on the default cores. So Zenith keeps the cores that AURKNIX ships; gearsystem is the
one candidate worth updating in a future image.

**PPSSPP.** Versions 1.12.3 and 1.20 were compared on three games, and 12 settings were tried one at a time on a set of
ten games run from save states. There is no winning version (1.20 wins in some games, 1.12.3 in others), profile-guided optimisation gives
+2.5 % rather than the hoped 15 %, and no setting is free: the one that raises speed (`SkipBufferEffects`, up to +77 % in
God of War) breaks the picture. The stock `FrameSkip = 3` stays because it is faster in heavy games.

**GPU clock.** A fixed 520 or 560 MHz freezes the console within a second or two, whatever the voltage; 200, 300, 400 and
600 MHz are stable. The frequency table is therefore 200/300/400/480/600 MHz.

**CPU turbo.** 1416 MHz at 1350 mV is stable on four cores; 1512 MHz on four cores rebooted the console, so it is not
used by default.

## Install

1. Download `AURKNIX-RK3326.aarch64-<date>.img.gz` from the Releases page and check the `.sha256`.
2. Decompress it and write it to an SD card with an imaging tool (Balena Etcher, Raspberry Pi Imager, `dd`).
   Double-check the target disk: writing an image overwrites everything on it.
3. For a device other than the R36 Ultra, pick its device tree with `/flash/dtbselect` (or `DtbselectWin64.exe` on the boot
   partition).
4. Updating an installed system: copy the matching `.tar` to the update share and reboot.

## Building

```
PROJECT=ROCKNIX DEVICE=RK3326 ARCH=aarch64 ./scripts/build_distro
```

A full build needs several hours and about 100 GB of free disk. Keep at least 6 GB free on the drive that holds the
images, or the last step (copying the image) can fail.

---

## About this fork

Zenith is an unofficial fork. The device and emulator lists, features and manuals of the base distribution are
documented upstream, not here:

- [AURKNIX](https://github.com/AveyondFly/distribution_rocknix) by **lcdyk** and **KK**: the fork Zenith is built on.
- [ROCKNIX](https://github.com/ROCKNIX): the distribution AURKNIX is forked from.

Everything else in the repository comes from them. What Zenith adds or changes is listed in
[`CHANGELOG.md`](CHANGELOG.md) and [`ZENITH.md`](ZENITH.md).

## Licenses

**AURKNIX** is a fork of **ROCKNIX**; ROCKNIX derives from [JELOS](https://github.com/JustEnoughLinuxOS/distribution/). All applicable licenses apply and credit to the JELOS and ROCKNIX teams. 

You are free to:

- Share: copy and redistribute the material in any medium or format
- Adapt: remix, transform, and build upon the material

Under the following terms:

- Attribution: You must give appropriate credit, provide a link to the license, and indicate if changes were made. You may do so in any reasonable manner, but not in any way that suggests the licensor endorses you or your use.
- NonCommercial: You may not use the material for commercial purposes.
- ShareAlike: If you remix, transform, or build upon the material, you must distribute your contributions under the same license as the original.

### Bundled Works
All other software is provided under each component's respective license. These licenses can be found in the software sources or in this project's licenses folder. Modifications to bundled software and scripts by the JELOS and ROCKNIX teams are licensed under the terms of the software being modified.

## Credits

Zenith exists thanks to **lcdyk** and **KK** (AURKNIX, whose RK3326 device trees, emulators and tooling this fork is
built on) and to **AveyondFly**, who hosts the AURKNIX repository. Thank you.

Thanks also to the **ROCKNIX** and **JELOS** teams, and to CoreELEC, LibreELEC and the many open source developers
whose work makes a distribution like this possible. See [`CREDITS.md`](CREDITS.md) for the full list.

If you would like to support **lcdyk**, maintainer of AURKNIX, you can use Ko-fi: [https://ko-fi.com/lcdyk](https://ko-fi.com/lcdyk).
