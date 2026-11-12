# Zenith changelog

Zenith is an unofficial fork of [AURKNIX](https://github.com/AveyondFly/distribution_rocknix) (itself a fork of
[ROCKNIX](https://github.com/ROCKNIX)) focused on fast boot, its own front-end (zfront) and fast suspend on
**RK3326** handhelds. Numbers below were measured on a **R36 Ultra** (some GPU numbers on an **R36S**); other RK3326 models are untested.
Every release so far is a **beta**. Dates are the build dates.

## v0.2.1 — 2026-11-12 (beta)

Small update on v0.2.0. Its release tag is the build date, `20261112`, which is what the built-in updater looks for.

> [!WARNING]
> **Beta.** Tested on the **R36 Ultra** and the **R36S**, the two consoles we have. Other RK3326 models use the same image
> but have not been tried: treat them as beta too.

### Changes

- The image includes a **new version of R36 Tuner Next that is not released yet** (the public repository is still at
  v2.0; the interface was rewritten since). The Tuner screenshots in the README are from this build.
- **Appearance options** (selector style, title alignment, list text case) apply as soon as you pick one, without
  restarting the front-end.
- **R36 Tuner Next** is listed with the performance rows in `Battery & performance` (when the Tuner is installed), and
  its "Overclock & monitoring" duplicate is hidden then, because both switched the same turbo.
- **R36 Tuner Next:** with Turbo on, the CPU limit can now be set to the Turbo steps (1416 and 1512 MHz on the R36 Ultra);
  they were missing from the list because the kernel lists them apart.

Checked on screen, on an R36 Ultra with a test build: the Appearance change. The R36S ran the build before this one
(with the Tuner changes), not this exact image. The new Tuner row order and the Turbo CPU range are covered by
automated tests only.

## v0.2.0 — 2026-11-11

Second public release (the first was v0.1.0, on 23 October 2026). Its release tag is the build date, `20261111`, which is
what the built-in updater looks for.

> [!WARNING]
> **Beta.** Tested on the **R36 Ultra** (720x720) and the **R36S**. The other RK3326 models use the same image but have
> **not** been tried: treat them as beta too.

### Highlights

- **zfront is now the default front-end** on new installs: Zenith's own front-end, written in C++ for these 1 GB consoles,
  with two new themes, **Zenith Minimal** and **Zenith Next**. On the R36 Ultra it shows its first screen in 1.0 s
  (EmulationStation: 4.1 s), starts a RetroArch game 0.27 s sooner (0.91 s vs 1.18 s) and leaves a running game 32-48 MB
  more RAM (God of War and Dead or Alive 2, two runs each). EmulationStation stays installed and themed (`Tools > Switch frontend`); an update never
  switches someone who already uses it.
- **Patched Panfrost (open GPU driver) is in the image.** libmali remains the default driver; Panfrost is a choice
  (`gpudriver panfrost` and a reboot, or the GPU DRIVER menu). If Panfrost cannot start, the system goes back to libmali
  by itself.
- **Performance mode** (GPU and RAM at full speed while a game runs) is in the image, **off by default**.
- **RAM at 786 MHz** on the models that have that step (R36 Ultra and the boards listed below).
- **R36 Tuner Next** built in, in a **new version that is not released yet** (the public repository is at v2.0), with
  undervolt and a fix for the hard freezes around a 480 MHz GPU clock.
- **Send files from your phone** through a web page (off by default, 4-digit PIN, QR code to scan).
- **Zenith DTB Selector** to pick the right screen/device tree for your console from Windows, Linux or macOS.

### zfront front-end

What it is: a second front-end written from scratch (C++17, SDL2) that reads the same settings, gamelists, theme and
scripts as EmulationStation, so nothing has to be set up twice.

Measured on an R36 Ultra with a large library (one run each, not a statistical benchmark):

| | EmulationStation | zfront |
|---|---:|---:|
| First screen after start | 4.1 s | **1.0 s** |
| Front-end memory in the menus | 143 MB | **36 MB** |
| Start RetroArch from the front-end | 1.18 s | **0.91 s** |

It does **not** make games faster: in God of War (the only game measured) the frame rate is the same with both.

**Choice and safety**
- A fresh install starts zfront. Switching is written down (`system.frontend`) and nothing else changes it. A console that
  was set up before keeps its front-end.
- If zfront fails before drawing its first screen, it is started again; only after three failures in a row does
  EmulationStation take over until the next boot, and your choice stays zfront.
- Game names can no longer run commands when a game is launched (a name with a backtick, `|`, tab or line break could);
  user hooks run without a shell; broken data (huge images, deeply nested XML, an oversized gamelist entry) no longer
  crashes it; the HTTP control API listens on 127.0.0.1 only.

**Everything EmulationStation does, on this image**
- Game list: sort and filter (name, rating, times played, last played, players, release date, genre, game time; favourites
  and genre filters), keys (X options, Y search with the on-screen keyboard, L2/R2 jump to the previous/next letter, Start
  main menu), Left/Right to change system, remembered cursor per system, whole covers, "ago" times.
- Game options: edit a game's metadata, delete a game (only files inside the system's ROM folder and the media its entry
  names), random game, collections, launch a game at startup (once per boot), per-system and per-game options, player
  assignments (players 1-8).
- Collections as carousel systems (all games, favourites, last played, never played, smart collections and custom ones).
- Scrapers menu with ScreenScraper, IGDB and ArcadeDB (see Known issues), cancelling a scrape with B, a rescan of the game
  folders without restarting.
- Background music, video previews, slideshow and random-video screensavers, game manuals and magazines (page by page,
  zoom), netplay, Moonlight, The Bezel Project installer, theme configuration for themes that have it.
- User interface modes **Full / Kiosk / Kid** (a console locked in EmulationStation stays locked), with the unlock code
  typed with the pad.
- Suspend from the menu and as a screensaver type; text size and high contrast.
- RetroAchievements in the main menu when it is on and an account is set.

**New things only zfront has**
- **Continue playing** row under the carousel: one resume tile per recent game; A resumes from the last save state.
- **Resume the last game at startup** (off by default): a 3 s countdown with the game's picture, B cancels.
- **Library tools:** save backups (back up, restore with an automatic backup first, delete; saves only, no ROMs), a
  read-only library check (missing BIOS, empty or fake archives, duplicates, games without a picture), play statistics,
  and a battery estimate per game from recent sessions.
- **Battery screen:** charge, current use, history, power saver and what used the most.
- **Performance suggestions** after a game and automatic covers in idle time (both off by default; they never enable
  anything by themselves).
- An in-game HOME menu (experimental, off by default).
- **Status bar** in every theme with icons and the battery percent as tall as the clock digits; charging shown as a bolt
  inside the battery.
- A fresh grid for every screen: square cells scaled with the screen, equal margins on all four sides, twelve columns.
  Layout checks cover all six themes at nine screen sizes, in English and Spanish, at normal and large text.
- **Menus reordered by use.** Main menu: Games, RetroAchievements (when on), Battery & performance, Screen & sound,
  Network, Library, Controller & Bluetooth, Appearance, System, Quit. Network has Wi-Fi first, then Transfer files and a
  Services screen (SSH, HTTP, USB gadget, cloud, VPN, host name). A game's own menu starts with Favourite and Scrape.
  Quit starts with Suspend. Settings keys and scripts are unchanged.
- **Spanish:** every text translated (597 keys); the web page is also in Spanish.

### Themes

- **Zenith Minimal** (light) is the default; **Zenith Next** (dark) is the new one, with a big card, a soft light in the
  colour of each system, a continue-playing row, and game lists with the whole cover, its details and the description.
  Next has no boxes in its menus (flat), a cyan frame on the chosen row, and its gradient takes the strongest colour of the
  chosen game's cover. `menuCards=on` brings the cards back.
- **XMB, Art Book, Iconic and TechDweeb** were remade on the same grid for every resolution, each with its own status bar
  icons. XMB is drawn as a cross with slowly drifting waves, and its battery is the three-cell one of the PSP era.
- **Smoother:** logos, covers and illustrations are decoded off the drawing thread, and game lists slide instead of
  jumping. Measured on an R36 Ultra, first time a list opens, key to first new frame: TechDweeb 400 to 111 ms, Iconic 329
  to 74, XMB 229 to 130, Zenith Next 121 to about 85. Moving through 30 systems in Zenith Next: 59 fps (the classic
  theme: 60).

### Performance and GPU

**Panfrost, the open GPU driver (Mali-G31)**
- It now works on the RK3326 (kernel patch `036-opp-single-voltage-is-minimum`; before, choosing it left the console on
  software rendering). Changing the driver needs a reboot. If no GPU binds to Panfrost at boot, the system saves libmali
  and reboots once.
- The image carries Mesa 26.0.4 with 19 Zenith patches (17 active) and a build with profile-guided (PGO) and link-time
  (LTO) optimisation. Per-program settings (`/usr/share/drirc.d/01-zenith-panfrost.conf`) turn on threaded GL for RetroArch
  and PPSSPP; libmali ignores them.
- **libmali stays the default**: it is the driver this image has been tested with the longest, and it offers OpenGL ES 3.2
  (Panfrost 3.1).
- **Measured** (R36S and R36 Ultra, 20 games, same save state, uncapped, CPU 1296 MHz, GPU 600 MHz, RAM 666 MHz; full
  tables in [`docs/PANFROST-STUDY.md`](docs/PANFROST-STUDY.md)):
  - Patched Panfrost vs libmali, geometric mean over the 20 games: **+4.4 %** (R36S) and **+6.5 %** (R36 Ultra). Wins /
    ties (within 5 %) / losses: 7 / 12 / 1 (R36S) and 8 / 12 / 0 (Ultra).
  - Patched vs stock Panfrost: **+38.9 %** on both consoles. Stock Panfrost vs libmali: -24.8 % (R36S) and -23.3 % (Ultra).
  - glmark2 (libmali / stock / patched): 380 / 386 / **434** on the R36S, 358 / 368 / **414** on the Ultra.
  - No GPU faults in any run.
- **At real speed the two drivers are close** (R36S, vsync and audio sync on, default clocks, one run per game, so under
  5 % may be noise): from -7 % (Mario Tennis) to +15 % (Wave Race 64) in five N64 games. God of War (PSP, PPSSPP with its
  default frame skipping): 7.8 fps with libmali, 8.2 with stock Panfrost and 8.4 with patched Panfrost; it is limited by
  the CPU.
- Not measured: battery use, and input latency (Panfrost keeps up to three frames in flight, which can add up to two
  frames of delay).

**Performance mode (off by default)**
- Pins the GPU at 600 MHz and the RAM at its top step (786 MHz where the model has it, 666 MHz on the rest) **while a game
  runs**, and puts both back on automatic when it ends. Why: the automatic governors only count busy time and bandwidth,
  and an emulator uses the GPU in short bursts, so they keep the clocks low (in N64 games the GPU sat at 200 MHz and the
  RAM at 328 MHz).
- Set it globally, per system or per game: AUTO / ON / OFF (`perfmode` in `system.cfg`). It overrides the GPU scaling
  governor of that game, in EmulationStation and in zfront.
- Measured on the R36S at real speed, one run per game (libmali auto -> performance mode with libmali; Panfrost auto ->
  performance mode with Panfrost): Mario Tennis 19.3 -> 25.0 and 18.0 -> 26.6 fps; Wave Race 64 35.1 -> 38.9 and
  40.5 -> 44.7. On the R36 Ultra with Panfrost: Mario Tennis 15.7 -> 27.2 fps (RAM up to 666 on auto, vs GPU 600 + RAM 666
  pinned), GoldenEye 34.8 -> 40.2, Wave Race 64 41.9 -> 48.4. God of War, Mario Kart 64 and Zelda: Ocarina of Time barely
  move (limited by the CPU or something else).
- **Not measured: heat and battery use in long sessions.** The R36 Ultra shut down with the GPU pinned at 600 MHz in long
  God of War sessions: that is why it is off by default.

**RAM at 786 MHz**
- A new top step of the memory table (was 666 MHz), at 1.1 V, on the same boards that dArkOSen enables it on (original
  R36S V12/V20/V21/V22/Y02, R36S Plus, R36H, R45H, R46H, the R36S Sauce) and on the R36 Ultra. Clones, R36 Ultra V2 / X and
  every other model stay at 666 MHz. The boot loader still starts the RAM at 666 MHz; the step is used only at run time.
- Tested only on an R36 Ultra: `memtester` over 300 MB ran 6 full passes in 57 minutes at 786 MHz with no errors (65 C at
  most). The other boards follow dArkOSen and were **not** tested here. On top of fixed GPU/RAM it adds little (Mario
  Tennis +6 %, GoldenEye +2 %, Wave Race 64 +1 %, one run). In R36 Tuner it is marked experimental.

**GPU clock freezes fixed**
- Patch `037-px30-gpu-clock-no-half-divider`: the GPU clock is built like in Rockchip's kernels. Without it, stepping the
  GPU limit through 480 MHz froze an R36 Ultra and an R36S within about a minute (6 of 6), and a reboot from 480 MHz did
  not come back (2 of 2). With the patch (kernel alone, both consoles): more than 25,000 steps per console with libmali,
  three runs with Panfrost, full load, suspend and reboots at 480 MHz, no freeze. Not tested: other models, a real game
  crossing 480 MHz.

**R36 Tuner Next (built in) and undervolt**
- CPU governor and frequencies, turbo steps (1416 MHz at 1350 mV was stable on four cores; 1512 MHz on four cores
  rebooted the console and is not used by default), benchmarks, a monitor, profiles, clock limits and a stability test that
  gives each changed frequency its turn under load and waits for the chip to cool at 88 C.
- Undervolting goes through the device tree with a boot safety net. The kernel now corrects the chip measurement for
  temperature before choosing the voltage column, like Rockchip's own kernel (6 boots between 40 and 52.5 C all chose the
  same column; without the correction 5 of them would have chosen another one). R36 Tuner keeps one voltage table, so
  what passed the test is what runs on every boot. After this update an earlier `tuned.dtb` is dropped on the first boot:
  make the change again.
- **Voltage changes and the safety net have been tested on an R36 Ultra only in part** (the stability test passed with one
  voltage table, peak 88 C); other models were tested only in simulation.

**Games**
- RetroArch games start 0.2-0.5 s faster: the script that writes the per-game configuration went from about 220 processes
  to about 40, with byte-identical files (compared on 14 systems). Whole launch averaged over 7 games: 3.54 s -> 3.33 s.
- **PSP:** PPSSPP 1.20.2 is now the default on RK3326 (the 2021 build stays selectable). Measured on an R36 Ultra with God
  of War only: with the same settings both run the same; with buffer effects skipped 1.20.2 is faster (25.7 vs 21.9 fps).

### System: boot, suspend, battery, updates, volume, controllers

- **Boot:** with zfront the console is ready 12.8-13.4 s after power-on; with EmulationStation, on the same image and
  console, 15.6-18.2 s (R36 Ultra, three boots each: first frame of the front-end on the kernel clock plus 2.1 s of
  bootloader). Suspend in 2.3 s and resume in 4.3 s (carried over from v0.1.0).
- **Updater:** it asks `zenmode-adri/Zenith`, never the original AURKNIX (whose package has the same file name as ours and
  could have replaced Zenith). A release whose tag is not a build date, like `v0.1.0`, is not offered as an update. Help,
  home and bug-report links point at Zenith.
- **Low battery:** the running game is closed with SIGTERM and the system waits up to 15 s and syncs before powering off, so
  RetroArch can write its autosave and SRAM. The 3.2 V threshold is the same. Tested with stubs only.
- **Battery gauge** for the R36S panel 4, R36XX Pro Max, R40XX, R40XX Pro Max, R45H and R46H: the voltage-to-charge table now
  matches the maker's (4.020 V is 95 %, 4.045 V is 100 %), and empty is 3.3 V instead of 3.0 V, so the gauge can reach 0 %.
  Checked only by building the device trees; **not booted on those consoles**.
- Samba, SSH and the stored Wi-Fi are **off by default** in the release image.

### Network and file transfer

- **Web file transfer from a phone or PC** (off by default; its own listener on port 8080; the PIN has **4 digits** and is
  shown in the Network menu). Five wrong PINs lock PIN and pairing for 30 s, and a phone already paired is not locked out.
  - **Scan, allow, done:** `Network > Show QR code` draws a QR code with the address and the PIN. The phone opens the page,
    the console asks "Allow this device?" (No is selected) and a "yes" remembers the phone, with no PIN next time. Up to ten
    phones; `Paired devices` lists them and forgets one or all.
  - **Folders and big files:** upload a whole folder (multi-disc games), files go in 8 MB slices and a cut connection
    resumes where it stopped; partial files are hidden and removed after two days. The game list is read again after each
    batch.
  - **BIOS tab:** which BIOS files are missing, wrong (md5) or fine for the systems you have, with an upload button on each.
  - **Saves tab:** back up now, download or upload a backup (checked before it is kept: only save files, no links, no paths
    outside the save folders), restore one (the current saves are backed up first; not while a game runs).
  - **Manage games:** launch, rename (saves, save states and pictures follow), change the picture, find a picture online
    (needs a scraper account, see Known issues), move to the trash, restore or empty it after a confirmation.
  - **Phone remote:** browse, launch and close games.
  - The page is served entirely by the console (it fetches nothing from the internet) and is also in Spanish.
- **`Network > Transfer files`** groups the web page and Samba.
- **Samba asks for a password:** the shares no longer let anyone in as root without one. Log in as `root` with the root
  password (the one of SSH). Windows 11 already refused the old guest login, so Samba did not work from it at all. Samba
  stays off by default.
- A network drive (WebDAV) for Windows Explorer and Finder is **not** in this release.
- **Choosing the right device tree:** **Zenith DTB Selector** (Windows, Linux, macOS; English, Spanish and Chinese) finds
  the SD card, lists 86 models, shows what will change before writing, keeps dated backups of `extlinux.conf` and can
  import your own `.dtb` (tested on a copy of the real card and on an R36 Ultra). Its **test mode** for clones whose model
  cannot be guessed is **not yet tested on hardware**. See [`docs/DTB-SELECTOR.md`](docs/DTB-SELECTOR.md).

### Fixes

- A game gets more free RAM: before launching, zfront drops every picture it can rebuild and returns the memory.
- Release safety: SSH, Samba, a stored Wi-Fi and verbose logging used to be applied to any image built from a
  work branch; they now exist only in the debug build.

### Known issues

- **Beta: tested only on the R36 Ultra and the R36S.** Other RK3326 models (clones) share the image but are untested.
- **The cover scraper (ScreenScraper) does not work in this image:** no developer credentials are built in. IGDB needs
  your own client id and secret; ArcadeDB worked in a test. "Find a picture online" in the phone page uses the same
  scraper account.
- **The root password is still `rocknix`.** SSH and Samba are off by default; change the password before turning them on.
- libmali is the default GPU driver; the patched Panfrost is a choice and is less compatible (OpenGL ES 3.1, not 3.2).
- Performance mode: heat and battery use are not measured, and long sessions with the GPU pinned shut down one R36 Ultra.
- Panfrost can add up to two frames of input delay (not measured). GoldenEye is slower with libmali when the clocks are
  fixed than on automatic; not explained.
- The battery tables for the six models above, the DTB Selector test mode and the low-battery shutdown have not been
  tried on real consoles in every case (see the entries above).
- Zenith adds no new emulators and does not update cores (those come from AURKNIX), apart from PPSSPP 1.20.2.

### Upgrading

- **From v0.1.0 or a development image:** write the new image to an SD card (back up your saves first; writing the image
  erases the card). The update menu of v0.1.0 looked at the original AURKNIX repository, so it does not see this release.
- Check the `.sha256` after downloading. Pick your RK3326 console's device tree with Zenith DTB
  Selector (see [`docs/DTB-SELECTOR.md`](docs/DTB-SELECTOR.md)).
- A fresh install starts zfront; `Tools > Switch frontend` changes between zfront and EmulationStation at any time.
- If you tuned the device tree with R36 Tuner on an earlier development image and kept your settings, an earlier
  `tuned.dtb` is dropped on the first boot: make the change again.

## v0.1.0 - 2026-10-23 (first public release, pre-release)

Image `AURKNIX-RK3326.aarch64-20261023.img.gz`. Tag `v0.1.0`. No update package yet: nobody has Zenith installed, so
the first release is the full image only.
Built as a **clean release image**:

- No Wi-Fi network, SSH, Samba or verbose logging is enabled by default: turn them on from the menu when you need them.
  The development builds enabled all of them.
- `sshd` no longer starts just because the kernel command line contains `ssh` (the AURKNIX `extlinux.conf` had it, so
  SSH was always on, with the factory password). SSH is now opt-in through the `ssh.enabled` setting.
- Checked on the files: no saved Wi-Fi name or key anywhere in the image, no SSH keys.
- **Not yet checked:** the first boot of this exact image from a freshly written SD card. The same sources run on an
  R36 Ultra as a development image.

## 2026-10-22 - images `4868372` and `8fef418`

### Suspend and resume
- Suspend takes 3.5 s -> 2.3 s and resume 8 s -> 4.3 s: Wi-Fi shutdown, panel and the rest run in parallel, panel
  colour settings are restored all at once. `fast-11-suspension-paralela`
- CPU/GPU/DMC governors are restored first on wake-up (they were left in powersave for 3.5 s, which made the first
  button press stutter). `fast-12-gobernadores-primero`

### EmulationStation
- First button press after idle: 225-245 ms -> 11 ms. The audio device that ES closes after 5 s of silence is now
  reopened on a worker thread (ES patch 0049; the one click that wakes it up is silent). `fast-13-primer-boton`

## 2026-10-21 - fast boot (`bareboot-4s` branch)

- **Early boot**: GPU, display, Sway and EmulationStation start in parallel as soon as storage is ready; the UI is
  requested at about 5.8 s instead of 10.1 s and ES is ready at about 13.3 s of kernel clock instead of 14.5 s.
  It disables itself if ES is not ready in time. `fast-1-led-rk817-tarde` .. `fast-9-seguro`
- **u-boot** `CONFIG_BOOTDELAY=0`: bootloader 3.11 s -> 2.11 s (R36 Ultra). Total from power-on to ES ready:
  about 17.6 s -> 15.4 s. `uboot-bootdelay0`
- initramfs probes the SD card every 50 ms and hands over to systemd with `exec`. `zinit-oneshot`, `fast-3a-fsck-sondeo`
- ES waits for the Wayland socket and for the ROM partition mount (patches 0044, 0045) and no longer blocks on
  PulseAudio at start-up (patch 0046).

## 2026-10-20 - GPU
- GPU frequency table 200/300/400/480/600 MHz (600 MHz is stable; 520 and 560 MHz were removed because a fixed 560 MHz
  clock freezes the console, cause unknown). simple_ondemand up-threshold set to 60. `zenith-87-gpu-tabla-600`,
  `zenith-91-gpu-umbral-60`

## 2026-10-19 - R36 Ultra joystick LEDs
- New LED driver for the R36 Ultra joysticks. `zenith-74-led-joysticks`

## 2026-10-15 - PPSSPP
- PPSSPP `RenderDuplicateFrames = False`.

## 2026-10-08..10-11 - Zenith first release work
- Own first-boot screens, boot banner, language question and credits. `zenith-1-firstboot`..`zenith-13-debug`
- Own light and dark EmulationStation theme ("Zenith"), bundled themes, R36 Tuner Next (CPU governor and frequencies,
  turbo steps, benchmarks, monitor). `zenith-14-tema-es` .. `zenith-52-bigger-logos`
- Opening a game from the menu: 6.29 s -> 3.85 s (average of 8 games). `launch-iter1`..`launch-iter4`

## Boot time summary (R36 Ultra)
- Stock AURKNIX: about 33 s from power-on to the menu (stopwatch). Zenith: about 15.4 s (boot log, ES ready at 13.3 s of
  kernel clock plus 2.1 s of bootloader). The 17.6 s -> 15.4 s step in the 2026-10-21 entry starts from an already
  optimised Zenith build, not from stock AURKNIX.
- In the first optimisation round (2026-10-05, R36S with the full 134-system list) the stopwatch already went 33 s -> about 17-19 s.

## 2026-10-07 - base work
- Silent power-off (no red message), silent console output at boot, RK915 Wi-Fi fixes (boot, reboot and suspend),
  CPU/GPU voltage bins L0-L3 through PVTM, RK817 battery gauge. `img-20261007-*`

## Known limitations
- Tested only on the **R36 Ultra** and the **R36S**. The other RK3326 devices, and every non-RK3326 target inherited from
  upstream, have not been tested with these changes.
- The Zenith early-boot mode, the Wi-Fi suspend fix (RK915 chip) and the LED driver are device specific.
- A fixed GPU clock of 560 MHz freezes the console; do not add it back to the frequency table.
- The eMMC tools (backup, install, restore) come from upstream and have not been tested with Zenith.
