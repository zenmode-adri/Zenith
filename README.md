<p align="center">
  <img src="docs/images/banner.png" alt="Zenith" width="560"/>
</p>

<h3 align="center">A fast, polished operating system for RK3326 handhelds</h3>

<p align="center">
  <a href="https://github.com/zenmode-adri/Zenith/releases"><img src="https://img.shields.io/github/v/release/zenmode-adri/Zenith?style=flat-square" alt="Release"/></a>
  <a href="https://github.com/zenmode-adri/Zenith/releases"><img src="https://img.shields.io/github/downloads/zenmode-adri/Zenith/total?style=flat-square" alt="Downloads"/></a>
  <img src="https://img.shields.io/badge/SoC-RK3326-blue?style=flat-square" alt="RK3326"/>
  <img src="https://img.shields.io/badge/kernel-Linux%206.12-orange?style=flat-square" alt="Linux 6.12"/>
  <img src="https://img.shields.io/badge/tested%20on-R36%20Ultra-success?style=flat-square" alt="Tested on R36 Ultra"/>
  <a href="https://ko-fi.com/zenmodeadri"><img src="https://img.shields.io/badge/Ko--fi-Buy%20me%20a%20coffee-FF5E5B?logo=ko-fi&style=flat-square" alt="Ko-fi"/></a>
</p>

Zenith is a system image for RK3326 handhelds: Linux 6.12 mainline, its own front-end (**zfront**), a boot and suspend
path tuned for speed, and a patched open GPU driver. It is an **unofficial fork** of
[AURKNIX](https://github.com/AveyondFly/distribution_rocknix), which is a fork of [ROCKNIX](https://github.com/ROCKNIX),
developed and measured on the **R36 Ultra**. Every number below was measured on that console; how, and how often, is
said next to it.

> [!WARNING]
> **Tested only on the R36 Ultra.** Other RK3326 handhelds use the same image and device-tree selector but have not been
> checked with these changes: treat them as beta. The R36S was tested only on an older development image. Targets other than RK3326 are inherited from upstream and were not built or tested.
> Zenith is not endorsed by, or affiliated with, ROCKNIX, AURKNIX or their authors; please do not report Zenith problems
> to them.

## Features

- **zfront, a front-end written for these consoles, is the default.** On the R36 Ultra it shows its first screen in
  **1.0 s** (EmulationStation: 4.1 s), starts a RetroArch game **0.27 s sooner** (0.91 s vs 1.18 s) and stays at about a
  quarter of the memory in the menus, so a running game has **about 40-45 MB more free RAM**. It reads the same settings,
  game lists and scripts as EmulationStation, which stays installed one menu away (`Tools > Switch frontend`).
  See [zfront and EmulationStation](#zfront-and-emulationstation).
- **Six themes on one grid:** Zenith Minimal (default) and Zenith Next, each light and dark, plus XMB, Art Book, Iconic
  and TechDweeb remade for zfront, laid out on square cells and checked by a test at nine screen sizes.
- **Fast boot and suspend:** about **15 s** from power-on to the menu (33 s on the base image), sleep in **2.3 s**, wake
  in **4.3 s** (measured on v0.1.0 with EmulationStation; zfront is ready sooner, see below).
- **Patched Panfrost (open GPU driver) in the image:** +39 % over stock Panfrost and close to Arm's libmali when you play
  (from -7 % to +15 % in five N64 games at real speed). libmali stays the default. See
  [Panfrost vs libmali](#panfrost-vs-libmali).
- **Performance mode** (off by default): GPU and RAM at full speed while a game runs. Mario Tennis 19.3 -> 25.0 fps
  (libmali, R36S). **RAM at 786 MHz** on the models that have that step.
- **R36 Tuner Next** built in: CPU governor and frequencies, turbo steps, undervolt, benchmarks, a stability test and a
  monitor.
- **Send files from your phone:** a web page with a QR code (off by default) to upload games, folders, BIOS and saves.
- **Zenith DTB Selector** to pick the right device tree for your console from Windows, Linux or macOS. See
  [`docs/DTB-SELECTOR.md`](docs/DTB-SELECTOR.md).

## Screenshots

Taken on an R36 Ultra (720x720) running v0.2.0, with the small test library of that console.

**zfront, the default front-end: Zenith Minimal and Zenith Next**

<table>
<tr>
<td align="center"><img src="docs/images/zfront-minimal-light.png" width="280"/><br/><sub>Zenith Minimal (default)</sub></td>
<td align="center"><img src="docs/images/zfront-minimal-dark.png" width="280"/><br/><sub>Zenith Minimal, dark</sub></td>
<td align="center"><img src="docs/images/zfront-next-dark.png" width="280"/><br/><sub>Zenith Next, with the continue-playing row</sub></td>
</tr>
<tr>
<td align="center"><img src="docs/images/zfront-menu.png" width="280"/><br/><sub>Main menu (Zenith Next)</sub></td>
<td align="center"><img src="docs/images/zfront-battery.png" width="280"/><br/><sub>Battery and power</sub></td>
<td align="center"><img src="docs/images/zfront-next-light.png" width="280"/><br/><sub>Zenith Next, light</sub></td>
</tr>
</table>

**The other themes, remade for zfront**

<table>
<tr>
<td align="center"><img src="docs/images/zfront-xmb.png" width="200"/><br/><sub>XMB</sub></td>
<td align="center"><img src="docs/images/zfront-artbook.png" width="200"/><br/><sub>Art Book</sub></td>
<td align="center"><img src="docs/images/zfront-iconic.png" width="200"/><br/><sub>Iconic</sub></td>
<td align="center"><img src="docs/images/zfront-techdweeb.png" width="200"/><br/><sub>TechDweeb</sub></td>
</tr>
</table>

**EmulationStation, still installed** (`Tools > Switch frontend`), with the Zenith theme (from v0.1.0)

<table>
<tr>
<td align="center"><img src="docs/images/splash.png" width="280"/><br/><sub>Start-up</sub></td>
<td align="center"><img src="docs/images/es-main-dark.png" width="280"/><br/><sub>System carousel, dark</sub></td>
<td align="center"><img src="docs/images/es-grid-dark.png" width="280"/><br/><sub>Game grid with box art</sub></td>
</tr>
</table>

---

## What changed

Measured on an **R36 Ultra**. Details and the git tag of every change are in [`CHANGELOG.md`](CHANGELOG.md). More
measurements, with charts: [`docs/COMPILER-STUDY.md`](docs/COMPILER-STUDY.md) (does compiling the emulators differently
make them faster? Mostly no) and [`docs/DTB-SELECTOR.md`](docs/DTB-SELECTOR.md).

| | AURKNIX (base) | Zenith | Change |
|---|---:|---:|---:|
| Power-on to menu | 33 s | **15 s** | **-53 %** |
| Opening a game (EmulationStation) | 6.29 s | **3.85 s** | **-39 %** |
| Suspend | 3.5 s | **2.3 s** | **-34 %** |
| Resume | 8 s | **4.3 s** | **-46 %** |
| First button press after idle (EmulationStation) | 225-245 ms | **11 ms** | **-95 %** |

> AURKNIX is the base image Zenith started from. Zenith's boot time comes from the boot log plus the bootloader
> (3.11 s -> 2.11 s). These times were measured on the first release, with EmulationStation as the front-end.
>
> Thanks to AURKNIX and ROCKNIX for the base these numbers start from.

### zfront and EmulationStation

EmulationStation (ES) is a complete, mature front-end, and Zenith keeps it installed, patched and themed: everything zfront
reads (settings, game lists, collections, themes, scripts) is ES's own format, so you can switch back and forth from
`Tools > Switch frontend` without setting anything up twice, and a console that already uses ES keeps it after an
update. zfront is a smaller program written from scratch in C++17 for these 1 GB consoles; on a fresh install it is the
default.

Measured on an **R36 Ultra**, one or two runs per point (not a statistical benchmark):

| | EmulationStation | zfront |
|---|---:|---:|
| First screen after start (large library) | 4.1 s | **1.0 s** |
| Ready after a restart of the front-end | 5-6 s | **3-4 s** |
| Start a RetroArch game from the front-end | 1.18 s | **0.91 s** |
| Front-end memory in the menus | 143 MB | **36 MB** |
| Front-end memory while a game runs (God of War, PSP) | 90-100 MB | **10-11 MB** |
| RAM the game could still use (God of War / Dead or Alive 2) | 456-472 / 480-488 MB | **504 / 528 MB** |
| CPU when idle on the menu (ticks per 10 s) | 35 | **0-2** |

- It does **not** make games run faster: in God of War (the only game compared) the frame rate is the same with both.
- What it gives a game is room: before a launch zfront drops every picture it can rebuild and hands the memory back.
- It does what ES does on this image (game lists and keys, collections, metadata editor, scrapers, music, video previews,
  manuals, netplay, Kiosk and Kid modes) and adds a continue-playing row, save backups, a library check, play
  statistics, a battery screen and the phone file transfer. Details in [`docs/ZFRONT.md`](docs/ZFRONT.md).

### Boot and power
- GPU, display, Sway and the front-end start in parallel as soon as storage is ready; u-boot `BOOTDELAY=0`; faster SD
  probing in the initramfs.
- Suspend runs Wi-Fi shutdown, panel and the rest in parallel; governors are restored first on wake-up.
- Silent shutdown (no red message) and quiet boot.

### Interface
- zfront: **Zenith Minimal** and **Zenith Next** (light and dark), plus XMB, Art Book, Iconic and TechDweeb, on one grid of
  square cells for every resolution; menus reordered by use (Games, Battery & performance, Screen & sound, Network,
  Library, Controller & Bluetooth, Appearance, System, Quit); every text in English and Spanish.
- EmulationStation keeps the Zenith light and dark theme and its redesigned dialogs, keyboard and sliders (ES patches
  0001-0054).
- Own first-boot screens, language question and boot banner; a Tools menu with an icon and a description for every tool.

### Opening games and graphics
- RetroArch games start 0.2-0.5 s faster: the script that writes the per-game configuration went from about 220 processes to
  about 40 and produces byte-identical files (compared on 14 systems).
- **The open-source GPU driver (Panfrost) now works on the RK3326**, so the `GPU DRIVER` choice in the menu is usable (kernel
  patch `036-opp-single-voltage-is-minimum`; before it, choosing Panfrost left the console on software rendering). If it
  fails to bind the GPU at boot, the system saves libmali and reboots once.
- **libmali stays the default, for safety and compatibility.** It is the driver this image has been tested with the
  longest and it offers OpenGL ES 3.2 (Panfrost 3.1). The Panfrost of the `GPU DRIVER` menu is our patched driver
  (below); Mesa's stock Panfrost is about -24 % slower than libmali on average over the 20 games of the study.
- PSP: PPSSPP 1.20.2 is the default on the RK3326 (the 2021 build stays selectable); measured with God of War only.

### GPU driver: our patched Panfrost (in the image since v0.2.0)
Panfrost is the open-source driver for the Mali-G31. With our 19 patches to Mesa 26.0.4 (17 active), plus a build with
profile-guided optimisation (PGO) and link-time optimisation (LTO), it is 39 % faster than stock Panfrost with the
games running free (God of War at real speed: 8.2 -> 8.4 fps). **Against libmali, at real speed, the two are close**:
from −7 % to +15 % in five N64 games with the default clocks, and God of War (with PPSSPP's frame skipping) 7.8 vs
8.4 fps, limited by the CPU (see [At real speed](#at-real-speed)). With the games running free and fixed clocks it is
+4.4 % (R36S) and +6.5 % (Ultra) on average over 20 games, and it scores 434 vs 380 (R36S) and 414 vs 358 (Ultra) in
glmark2 (a synthetic benchmark). Zero GPU faults in every test. Full study with screenshots of the same frame on all
three drivers:
[`docs/PANFROST-STUDY.md`](docs/PANFROST-STUDY.md). To use it, pick `GPU DRIVER > Panfrost` in the menu, or run
`gpudriver panfrost` and reboot; `gpudriver libmali` and a reboot go back.

What the patches do (all general, nothing per game). Each gain was measured on its own, on the game named, with the
game running free and no frame skipping, so they do not add up and are smaller when you play:
- **Index data read from cached memory** (`003`): the min/max scan of index buffers used to read write-combined GPU
  memory at about 5 MB/s; it now reads a cached copy.
- **Buffer mappings kept and a cheaper buffer path** (`001`, `002`): buffers that go back to the cache keep their CPU
  mapping, and tracing and labelling work is gone from the hot path.
- **Mesa's GL thread (glthread) allowed** (`004`, `005`): Panfrost now declares what Mesa needs to run OpenGL calls on
  a second thread, with the locking that makes it safe. Mario Kart 64 (ParaLLEl) +23 %.
- **Shader compiler tuned for the emulators' ubershaders** (`008`-`013`, `019`): uniforms that decide branches and
  texture maths become constants so the compiler removes code and registers, plus folded half-precision conversions
  and better instruction scheduling on Bifrost. GoldenEye 23.5 -> 32 fps on the Ultra with `008`-`013`, then +4-6 %
  with `019`.
- **Up to three frames in flight instead of one** (`014`): the driver no longer waits for the previous frame at every
  swap. GoldenEye +7 % (Ultra) and +13 % (R36S) with two frames. Cost: up to two frames of extra input delay (not
  measured).
- **CPU readbacks from cached memory** (`017`): reading the picture back (what ParaLLEl does at the `veryhigh` accuracy of
  a fresh install) goes through cached memory instead of write-combined memory. Mario Kart 64 (ParaLLEl) +24 %.
- **Tile CRC only on buffers that reach the screen** (`018`).
- **A Panfrost bug fixed** (`015`): the varying linkage between shaders was reused with the wrong fragment-shader
  variant, so the GPU read the wrong descriptors (page faults and GPU resets). Now 0 faults.
- **Everything on by default** (`016`), no environment variables. `006` and `007` are built in but off (no gain).
- **PGO and LTO build:** 0 to +3 % in most games.
- **Per-program settings** (`/usr/share/drirc.d/01-zenith-panfrost.conf`, read only by Mesa, ignored by libmali):
  glthread on for RetroArch and PPSSPP, and PPSSPP also hides `GL_OES_copy_image` so it uses a faster blit instead.
  Stand-alone emulators (Flycast, Mupen64Plus) get no per-program settings yet.

### Hardware, battery and tuning
- **R36 Tuner Next** is built in, with undervolt through the device tree and a boot safety net. Voltage changes were
  tested on an R36 Ultra only in part, and other models only in simulation.
- GPU clock fix (patch `037`): stepping the GPU limit through 480 MHz froze an R36 Ultra and an R36S within about a minute
  (6 of 6) before it; with it, more than 25,000 steps per console without a freeze.
- Chip-bin voltage columns are chosen after correcting the chip measurement for temperature (6 boots between 40 and 52.5 C
  all picked the same column on an R36 Ultra).
- Low battery closes the game and syncs before powering off; volume keys repeat faster when held (about 0.19 s per step
  instead of about 0.34 s) and show the volume bar at 0 % and 100 %; Samba asks for the root password.
- Battery gauge tables for the R36S panel 4 family, R36XX Pro Max, R40XX and R45H/R46H follow the maker's, checked only by
  building the device trees.

### Choosing the right device tree
- **Zenith DTB Selector** (Windows, Linux, macOS; window in English, Spanish and Chinese): finds the SD card, lists 86 models,
  shows what will change before writing, keeps dated backups of `extlinux.conf` and can import your own `.dtb`.
- **Test mode** for consoles whose DTB cannot be guessed: on each boot it tries the next candidate until you hold a button.
  **Not yet tested on hardware.**

### R36 Ultra hardware
- RK915 Wi-Fi driver fixes, joystick RGB LED driver, RK817 battery gauge, CPU/GPU voltage bins
  L0-L3 through PVTM, GPU table 200-600 MHz.

> [!NOTE]
> Zenith adds no new emulators and does not update cores (apart from PPSSPP 1.20.2); those come from AURKNIX. The work is
> on boot, suspend, responsiveness, the front-end and the GPU.

> [!IMPORTANT]
> **Known issues.** The cover scraper (ScreenScraper) does **not** work in this image: no developer credentials are built
> in. The root password is still `rocknix` (SSH and Samba are off by default; change it before turning them on).
> Only the R36 Ultra has been tested.

## Panfrost vs libmali

Two tests. **At real speed** is how a game feels when you play it. The **free-running test** lets every game run as fast
as it can with fixed clocks: it shows how much work each driver gets through, but most of its differences shrink or
disappear at real speed.

### At real speed

Measured on the **R36S** the way you play: vsync and audio sync on (sound muted), the same save states, the CPU
at 1296 MHz in every column, one run per point, so differences under 5 % may be noise. The core settings are not those
of the free-running test, so these numbers cannot be compared with the tables below. "Auto" is how the console ships
(GPU and RAM on their automatic governors); "RAM fixed" pins only the RAM at 666 MHz; "GPU + RAM fixed" pins the GPU at
600 MHz and the RAM at 666 MHz, which is what the new performance mode does on models without the 786 MHz step. Panfrost is our patched one.

fps at real speed:

| Game (N64) | libmali auto | libmali RAM fixed | libmali GPU + RAM fixed | Panfrost auto | Panfrost RAM fixed | Panfrost GPU + RAM fixed |
|---|---:|---:|---:|---:|---:|---:|
| Mario Tennis | 19.3 | 22.7 | 25.0 | 18.0 | 22.4 | **26.6** |
| Wave Race 64 | 35.1 | 37.6 | 38.9 | 40.5 | **44.9** | 44.7 |
| Mario Kart 64 (ParaLLEl) | 21.3 | 23.8 | 24.5 | 22.3 | 24.5 | **24.8** |
| GoldenEye 007 | 39.6 | 36.9 | 37.8 | 38.1 | **40.8** | 40.7 |
| Zelda: Ocarina of Time | 15.5 | **16.6** | 16.5 | 16.1 | **16.7** | 16.6 |

The same games with the same settings but uncapped (fast-forward, no vsync, no audio sync), to see how much headroom
each setting leaves:

| Game (N64) | libmali auto | libmali RAM fixed | libmali GPU + RAM fixed | Panfrost auto | Panfrost GPU fixed | Panfrost RAM fixed | Panfrost GPU + RAM fixed |
|---|---:|---:|---:|---:|---:|---:|---:|
| Mario Tennis | 20.9 | 26.8 | 29.4 | 15.7 | 25.9 | 28.6 | **29.3** |
| Wave Race 64 | 40.8 | 45.0 | 47.4 | 47.5 | 59.9 | 65.0 | **65.7** |
| Mario Kart 64 (ParaLLEl) | 25.5 | 30.4 | 33.2 | 26.8 | 32.0 | 31.0 | **37.5** |
| GoldenEye 007 | 41.4 | 35.5 | 37.0 | 38.8 | 39.3 | **43.1** | 42.8 |
| Zelda: Ocarina of Time | 21.1 | 24.7 | 24.7 | 24.7 | 24.6 | **28.6** | **28.6** |

- **With the default automatic clocks the two drivers are close**: at real speed from −7 % (Mario Tennis) to +15 %
  (Wave Race 64).
- **The automatic clocks are the main brake, with either driver.** During Mario Kart 64 and Zelda the GPU spent most of
  the time at 200 MHz (its lowest step) and the RAM at 328 MHz; in Mario Tennis the GPU went up to 400 MHz. The
  governors only count busy time and bandwidth (the GPU jumps to its top speed above 45 % busy with Panfrost and 60 %
  with libmali, the RAM above 40 %), and an emulator uses the GPU in short bursts while the CPU waits for it, so the
  load they see stays low.
- **The RAM matters as much as the GPU:** pinning only the RAM already gives a large part of the gain
  (Panfrost at real speed: Mario Tennis 18.0 -> 22.4, Wave Race 64 40.5 -> 44.9).
- **The performance mode** (in the image, off by default) pins the GPU at 600 MHz and the RAM at its top step (786 MHz on the models that have it, 666 MHz on the rest) while a game runs and puts
  both back on automatic when it ends. It is off by default and can be turned on for all games, per system or per
  game. Best results: Mario Tennis 19.3 -> 25.0 fps with libmali and 18.0 -> 26.6 with Panfrost; Wave Race 64 35.1 ->
  38.9 (libmali) and 40.5 -> 44.7 (Panfrost). Heat and battery use in long sessions are not measured yet (the R36 Ultra
  shut down with the GPU pinned at 600 MHz in long God of War sessions).
- **Zelda** barely moves (15.5 to 16.7 fps in every column): at real speed it is held back by something else.
- **GoldenEye with libmali** was slower with fixed clocks than on auto, in both tests; not explained.
- **God of War** (PSP, PPSSPP with its default frame skipping): 7.8 fps with libmali, 8.2 with stock Panfrost and 8.4
  with patched Panfrost. The emulator thread uses a whole CPU core, so neither the driver nor the GPU clock changes
  much there.

**R36 Ultra**, patched Panfrost, same method and games, plus RAM at 786 MHz. The 786 MHz step is **in the image** on the same boards [dArkOSen](https://github.com/djparentx/dArkOSen-R36S) enables it on: the original R36S (V12/V20/V21/V22/Y02, Plus, R36H, R45H, R46H),
the R36S Sauce and the R36 Ultra. Clones and other models stay at 666 MHz. The RAM still starts at 666 MHz and only
switches to 786 while running (starting the RAM at 786 was not reliable on these boards), at 1.1 V like dArkOSen. "Auto" lets the governors pick, with the RAM allowed up to 666 or 786 MHz; "fixed"
pins the GPU at 600 MHz and the RAM at 666 or 786 MHz.

fps at real speed:

| Game (N64) | Auto, RAM up to 666 | Auto, RAM up to 786 | Fixed GPU 600 + RAM 666 | Fixed GPU 600 + RAM 786 (performance mode) |
|---|---:|---:|---:|---:|
| Mario Tennis | 15.7 | 19.8 | 27.2 | **29.0** |
| Wave Race 64 | 41.9 | 46.4 | 48.4 | **49.0** |
| GoldenEye 007 | 34.8 | 35.6 | 40.2 | **41.1** |
| Mario Kart 64 (ParaLLEl) | 24.8 | **25.0** | 24.8 | 24.8 |
| Zelda: Ocarina of Time | **16.8** | 16.6 | 16.5 | 16.6 |

- **Fixing the clocks is the big step on the Ultra too:** Mario Tennis +74 %, GoldenEye +16 %, Wave Race 64 +15 %.
  Mario Kart 64 and Zelda do not move with any setting.
- **RAM at 786 MHz adds little on top:** Mario Tennis +6 %, GoldenEye +2 %, Wave Race 64 +1 % (one run, so the
  smaller ones may be noise). On automatic clocks the governor almost never takes the RAM to 786.
- **Stable in our tests:** no crash in these runs (67 °C at most), and `memtester` (300 MB) ran its 6 full passes in
  57 minutes at 786 MHz with no errors (65 °C at most), on one R36 Ultra. The original R36S boards get it because
  dArkOSen ships it on them; we have not tested those ourselves.

### Free-running test (20 games)

20 games on an **R36S** and an **R36 Ultra**, three drivers: Arm's **libmali** (the Zenith default), **stock Panfrost**
(Mesa 26.0.4 as shipped) and **our patched Panfrost** (19 patches, PGO + LTO). Every game starts from the same save
state and the fps are counted over the same frames, in uncapped fast-forward, with the CPU at 1296 MHz, the GPU at
600 MHz and the RAM at 666 MHz, two runs each. Full tables, method and screenshots of all 20 games:
[`docs/PANFROST-STUDY.md`](docs/PANFROST-STUDY.md). This shows how much work each driver gets through, not how a game
feels when you play it: most of these differences shrink or disappear at real speed (see above).

| | R36S | R36 Ultra |
|---|---:|---:|
| Patched Panfrost vs libmali: wins / ties (±5 %) / losses, out of 20 | **7 / 12 / 1** | **8 / 12 / 0** |
| Patched Panfrost vs libmali, geometric mean of the 20 games | **+4.4 %** | **+6.5 %** |
| Patched vs stock Panfrost, geometric mean | **+38.9 %** | **+38.9 %** |
| Stock Panfrost vs libmali, geometric mean | −24.8 % | −23.3 % |
| glmark2 score: libmali / stock / patched | 380 / 386 / **434** | 358 / 368 / **414** |

Biggest differences in this test (fps, libmali -> patched Panfrost):

| Game | System · core | R36S | R36 Ultra |
|---|---|---:|---:|
| Wave Race 64 | N64 · Mupen64Plus-Next | 57.0 -> **65.4** (+15 %) | 53.6 -> **62.9** (+17 %) |
| Zelda: Ocarina of Time | N64 · ParaLLEl N64 | 25.1 -> **28.7** (+15 %) | 24.2 -> **28.1** (+16 %) |
| Mario Kart 64 | N64 · ParaLLEl N64 | 33.6 -> **38.4** (+14 %) | 32.2 -> **37.1** (+15 %) |
| Mega Man 64 | N64 · Mupen64Plus-Next | 127.8 -> **139.7** (+9 %) | 123.8 -> **134.8** (+9 %) |
| GoldenEye 007 | N64 · Mupen64Plus-Next | **44.0** -> 41.3 (−6 %) | **42.0** -> 40.9 (−3 %) |

God of War is left out on purpose: this test ran it without frame skipping, which is not how anyone plays it on these
consoles; with PPSSPP's default frame skipping it is 7.8 fps with libmali and 8.4 with patched Panfrost. GoldenEye on
the R36S is the only game where libmali stays ahead by more than 5 %. No GPU faults in any run, and stock
and patched Panfrost draw the same picture in 18 of the 19 games compared pixel by pixel (the patches do not change what
is drawn).

<table>
<tr>
<td align="center"><img src="docs/panfrost-study/s_mvc2.jpg" width="420"/><br/><sub>Marvel vs. Capcom 2 (Dreamcast), R36S: same frame on the three drivers (fps of the free-running test)</sub></td>
<td align="center"><img src="docs/panfrost-study/s_mk.jpg" width="420"/><br/><sub>Mario Kart 64 (N64), R36S: same frame on the three drivers (fps of the free-running test)</sub></td>
</tr>
</table>

> [!NOTE]
> Under RetroArch in **fullscreen, libmali never goes above 60 fps**: it accepts the request for no vsync, but nothing
> changes. Windowed it runs free, so libmali was measured both ways and every game uses its best result. Not measured:
> battery use and input latency (Panfrost keeps up to three frames in flight, which can add up to two frames of delay).
> Five of the games were also used to train the PGO profile.

---

## Under the hood

**Kernel.** Zenith runs **Linux 6.12 mainline** (6.12.79 in the previous release), with upstream drivers for the SoC, storage and
power management, on a long-term-support base. The 6.12 base comes from AURKNIX/ROCKNIX. The patch set on top of it is in
`projects/ROCKNIX/devices/RK3326/patches/linux/` (part inherited, part Zenith's own, see the changelog) and includes, among
others:

- CPU/GPU voltage bins L0-L3 read through PVTM and an RK3326 OPP table (CPU 1296 MHz by default, with 1416 and 1512 MHz as an
  opt-in turbo; GPU 200-600 MHz).
- Memory (DMC) frequency scaling, ported from the Rockchip 6.1 BSP by KK (AveyondFly) of AURKNIX: the DDR clock follows the load between
  194 and 666 MHz.
- RK915 Wi-Fi driver fixes and the RK817 battery gauge fixes.
- The R36 Ultra joystick RGB LED driver and a panel brightness control.
- An exFAT directory-readahead limit.

**Suspend to RAM.** Suspending with the power button is a real suspend-to-RAM (PSCI `mem`/deep sleep): the CPU rail is powered
off and the memory rail is lowered while the console sleeps (about 20-25 mA, estimated from the battery counter). Making it
reliable on these devices relies on upstream fixes that Zenith carries:

- SD card resume retried after a power cycle, and USB (dwc2) suspend/resume fixes: ROCKNIX pull requests #3304 and #3303,
  by Jacob Cook.
- RK817 battery gauge corrected across sleep, and the `SYS_CAN_SD` register cleared by default, as the vendor kernel does:
  ROCKNIX pull request #3305, by Chris Morgan.

On top of that, Zenith adds the RK915 Wi-Fi unload/reload around sleep, suspend steps that run in parallel,
CPU/GPU/memory governors restored first on wake-up, a "Suspending..." notice
that stays on screen for at least one second, and a fix that makes sure the backlight is always on after waking.

The kernel configuration was reviewed for performance: it is already lightweight (`-O2`, no debugging or tracing,
zram and MGLRU enabled), and the Cortex-A35 reports "not affected" for almost every speculative-execution mitigation, so
there is no security tax to remove.

**Boot.** A small initramfs probes the SD card every 50 ms and hands over to systemd with `exec`; GPU, display, Sway and
the front-end (zfront or EmulationStation) then start in parallel as soon as storage is ready, and the early start disables
itself if the front-end is not ready in time. u-boot runs with `BOOTDELAY=0`.

**Front-ends.** zfront is the default; the notes below describe the EmulationStation that stays installed next to it. 54 patches on top of `ROCKNIX/emulationstation-next` (`projects/ROCKNIX/packages/ui/emulationstation/patches/RK3326/`)
cover the interface, the carousel, audio wake-up, the quit menu and the Wi-Fi and settings menus.

## Tested and not shipped

Newer emulator builds, PPSSPP versions and GPU clocks were benchmarked on the R36 Ultra, and a change that did not help
was not shipped. Everything below was measured on the device.

### Newer emulator cores (ROCKNIX `next` branch)

19 libretro cores were rebuilt from the `next` branch and compared with the installed ones. The main comparisons used 32
ROMs with 4 repetitions each (RetroArch 1.22.2, same governor, temperature capped at 68 °C); only CPU time was measured,
not audio or save states.

| Core | CPU per frame, old -> new | Result |
|---|---:|---|
| snes9x, genesis_plus_gx, picodrive, gambatte, vba_next, quicknes | -1.4 % to +1.1 % | ✅ Same speed, no reason to change |
| nestopia (default NES core) | **+19 %** | ⚠️ More CPU on all 6 ROMs, not updated |
| fceumm | **+13 %** | ⚠️ More CPU, not updated |
| gearsystem (Game Gear / Master System) | **-26 %** | ✅ The only clear gain, candidate for a future image |
| mGBA, new vs old | **-42 %** | ⚠️ Better than before, but the default gpSP (GBA) and gambatte (GB/GBC) still use less CPU |

In these tests newer builds did not bring more performance in general: most are equivalent, two use more CPU, and mGBA
improves on its older version but not on the default cores. So Zenith keeps the cores that AURKNIX ships.

### PPSSPP

Versions 1.12.3 and 1.20 were compared on three games, and 12 settings were tried one at a time on a set of ten games run
from save states.

| Test | Result |
|---|---|
| Best version | No winner: 1.20 wins in some games, 1.12.3 in others |
| Profile-guided optimisation | **+2.5 %**, not the hoped 15 % |
| `SkipBufferEffects` | Up to **+77 %** in God of War, but it breaks the picture ⚠️ |
| `FrameSkip = 3` (stock) | Stays: faster in heavy games |

### GPU clock and CPU turbo

| Setting | Result |
|---|---|
| GPU fixed at 520 or 560 MHz | ⚠️ Freezes the console within a second or two, whatever the voltage |
| GPU 200 / 300 / 400 / 600 MHz | ✅ Stable (frequency table 200/300/400/480/600 MHz) |
| CPU 1416 MHz at 1350 mV | ✅ Stable on four cores |
| CPU 1512 MHz on four cores | ⚠️ Rebooted the console, not used by default |

---

## Install

1. Download `AURKNIX-RK3326.aarch64-<date>.img.gz` from the [Releases](https://github.com/zenmode-adri/Zenith/releases) page
   and check the `.sha256`. Coming from v0.1.0 you must write the image again: its update menu looked at the original
   AURKNIX repository and does not see Zenith's releases. From v0.2.0 on, the built-in updater offers later releases
   (it downloads the `AURKNIX-RK3326.aarch64-<date>.tar` of the release and checks its `.sha256`).
2. Decompress it and write it to an SD card with an imaging tool (Balena Etcher, Raspberry Pi Imager, `dd`).
   Double-check the target disk: writing an image overwrites everything on it.
3. For a device other than the R36 Ultra, pick its device tree with **Zenith DTB Selector** (`ZenithDtbSelector.exe` on the
   root of the boot partition, also for Linux and macOS): see [`docs/DTB-SELECTOR.md`](docs/DTB-SELECTOR.md). If your
   console is not on the list, its test mode can try the candidates for you (not yet tested on hardware).

## Building

```bash
PROJECT=ROCKNIX DEVICE=RK3326 ARCH=aarch64 ./scripts/build_distro
```

A full build needs several hours and about 100 GB of free disk. Keep at least 6 GB free on the drive that holds the
images, or the last step (copying the image) can fail.

---

## About this fork

Zenith is an unofficial fork. The device and emulator lists, features and manuals of the base distribution are
documented upstream, not here:

- [AURKNIX](https://github.com/AveyondFly/distribution_rocknix) by **lcdyk** and **KK** (GitHub: AveyondFly): the fork Zenith is built on.
- [ROCKNIX](https://github.com/ROCKNIX): the distribution AURKNIX is forked from.

Everything else in the repository comes from them. What Zenith adds or changes is listed in
[`CHANGELOG.md`](CHANGELOG.md) and [`ZENITH.md`](ZENITH.md). Maintained by
[zenmode-adri](https://github.com/zenmode-adri); see [`CREDITS.md`](CREDITS.md) for the people and projects this work
stands on.

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

Zenith exists thanks to **lcdyk** and **KK** (GitHub: **AveyondFly**), the authors of AURKNIX, whose RK3326 device trees,
emulators and tooling this fork is built on. Thank you.

Thanks also to the **ROCKNIX** and **JELOS** teams, and to CoreELEC, LibreELEC and the many open source developers
whose work makes a distribution like this possible. See [`CREDITS.md`](CREDITS.md) for the full list.

If you would like to support **lcdyk**, maintainer of AURKNIX, you can use Ko-fi: [https://ko-fi.com/lcdyk](https://ko-fi.com/lcdyk).
