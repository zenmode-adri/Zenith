# Zenith v0.3.0 (beta)

Unofficial fork of AURKNIX/ROCKNIX for **RK3326** handhelds, developed and measured on the **R36 Ultra** (also tested on the **R36S**, much less).
Everything that changed, with the measurements: [CHANGELOG.md](https://github.com/zenmode-adri/Zenith/blob/main/CHANGELOG.md). Performance tables and method: [docs/PERFORMANCE.md](https://github.com/zenmode-adri/Zenith/blob/main/docs/PERFORMANCE.md).

> [!WARNING]
> **Beta.** Tested only on the **R36 Ultra** and the **R36S** (the R36S much less: it boots, the interface and PPSSPP start, and 10 PSP games were measured in one scene each). I am one person, so bugs can slip through: if you find one, please [open an issue](https://github.com/zenmode-adri/Zenith/issues). Other RK3326 models use the same image but have not been tried.

## Headline: Vulkan for PSP games (EXPERIMENTAL, off by default: opt-in)

PPSSPP now runs on **Vulkan** on the Mali-G31, with two drivers:

- **Vulkan 1.3 from Arm** (libmali g29p1, Vulkan 1.3.303) when the **Arm (official)** GPU driver is loaded (the default).
- **PanVK** (Mesa 26.0.4 with Zenith patches) when you switch the GPU driver to **Open source (Panfrost / PanVK)**. PanVK is Vulkan 1.0 and **not conformant**.

**PPSSPP still starts on OpenGL by default. Vulkan is EXPERIMENTAL and you turn it on yourself.** To turn it on or off:
- **In PPSSPP itself:** Settings > Graphics > **Backend**: OpenGL or Vulkan, like on any other device. PPSSPP restarts and goes back to its own main menu (not to the game); the choice is kept for next time. The launcher no longer overwrites it. (Seen on the R36 Ultra, both ways.)
- **Updating from an earlier test build** that had `ppsspp.vulkan` set: the launcher converts it once into PPSSPP's own setting.
- **Automatic safety net:** if PPSSPP closes twice in a row before drawing its first frames, or crashes once while using Vulkan, the launcher switches PPSSPP back to OpenGL by itself (you can turn Vulkan on again in PPSSPP's menu). The reason is written to the system log (`journalctl -t start_ppsspp`); it is not shown on screen. A single early exit does not count (you may just have left the game).

**Which Vulkan you get** depends on the system GPU driver (System settings > GPU DRIVER, needs a reboot; it applies to the whole console):

| GPU DRIVER | PSP on OpenGL | PSP on Vulkan |
|---|---|---|
| **Arm (official)** (default) | libmali g24p0 | Vulkan 1.3 from Arm (g29p1) |
| **Open source (Panfrost / PanVK)** | Panfrost | PanVK |

Only PPSSPP sees Vulkan; the rest of the system (libmali GL, Panfrost GL, RetroArch, other emulators) does not change.

**What was measured.** Main table: **R36S, 10 PSP games, one saved scene each, in player conditions** (factory launcher, factory governors: CPU `ondemand`, GPU and memory `simple_ondemand`; nothing fixed; 1x, no frame skip; 30 s window, Arm columns: 2 runs per game; PanVK column: 2 runs in the five games that separate the engines, 1 in the five that sit at the 30 fps cap). New frames per second, target 30 (Tekken 60). "Stalls" = intervals of 45 ms or more per minute (fewer is smoother).

| Game | OpenGL (Arm) | Vulkan (Arm) | PanVK (open source) | Stalls OpenGL / Vulkan Arm / PanVK |
|---|---|---|---|---|
| God of War: Chains of Olympus | 18.2 | 27.5 | 27.8 | 1089 / 75 / 42 |
| God of War: Ghost of Sparta | 25.8 | 30.3 | 30.3 | 179 / 23 / 33 |
| F1 2009 | 29.7 | 36.7 | **39.5** | 15 / 6 / 3 |
| Tekken: Dark Resurrection (target 60) | 55.3 | 59.7 | 59.7 | 9 / 3 / 2 |
| FlatOut: Head On | 28.1 | 29.2 | 28.9 | 63 / 27 / 26 |
| Assassin's Creed: Bloodlines (at cap) | 29.6 | 30.2 | 30.2 | 11 / 8 / 0 |
| GTA: Vice City Stories (at cap) | 30.4 | 30.1 | 30.1 | 0 / 2 / 0 |
| Daxter (at cap) | 30.3 | 30.2 | 30.1 | 5 / 0 / 6 |
| Castlevania: Dracula X Chronicles (at cap) | 30.1 | 30.1 | 30.1 | 3 / 0 / 0 |
| Tenchu: Shadow Assassins (at cap) | 30.4 | 30.2 | 30.1 | 5 / 3 / 12 |

- **In these scenes the two Vulkan drivers are on par** (within 1 fps in 9 of 10 games). **PanVK is ahead in F1 2009** (39.5 against 36.7, +8 %, above the run-to-run noise). **Both are well above the OpenGL of Arm** in the five games that separate the engines (Chains of Olympus +51 % / +53 %, Ghost of Sparta +17 % / +17 %, F1 2009 +24 % / +33 %, Tekken +8 %, FlatOut +4 % / +3 %, Vulkan Arm / PanVK) and have far fewer stalls; the other five sit at the game's own 30 fps cap and cannot tell the engines apart.
- Full tables, method and the earlier lab figures: [docs/PERFORMANCE.md](https://github.com/zenmode-adri/Zenith/blob/main/docs/PERFORMANCE.md).
- **Limits:** one scene per game, no key presses; one R36S with a fan (stock units run hotter); the Arm columns and the PanVK column were taken on two different pre-release test builds (not repeated on the same image; assumed to make no difference); OpenGL was measured with a frame logger preloaded and Vulkan with a logging layer, each of which may cost a little; no warm-up pass (shader caches mixed cold and warm). Close to the lab figures below, except F1 2009 with Vulkan from Arm (36.7 here, 39.5 in the lab with fixed clocks; the cause was not separated).
- **Lab figures** (LAB: CPU 1296 / GPU 600 / RAM 666 MHz fixed, R36S, 10 games; earlier method, kept for reference in the performance document): Vulkan Arm and PanVK within 1 fps in 9 of 10 games and equal (39.5) in F1 2009; libmali GL 28.9, Panfrost GL 24.8 and the Mesa 26.0.4 upstream builds lower. R36 Ultra, Ghost of Sparta in combat with the game's 30 fps hack: Vulkan Arm 29.9, PanVK 30.05, Panfrost GL 29.4, libmali GL 20.6 - 22.9; with factory settings on the Ultra, Ghost of Sparta 28 - 29.4 of 30 and Chains of Olympus 27 (n = 1-2, automatic key presses, before the tearing fix below).
- 8 small PSN games (Angry Birds, Age of Zombies, Brainpipe, Cubixx, Fly Fu, Blast Off, Alien Zombie Death, Dr. Mini Games) were only checked for starting on Arm Vulkan (still running after 38 s, not black); they were not played. Cubixx was also used to try the Home menu on an R36S.

**Vulkan from Arm: tearing fixed.** With the Arm driver the picture could show blocks ("cubes") of a frame that was still being drawn, or a doubled-frame motion, because the image was handed to the compositor before the GPU had finished it. The Zenith layer that sits between the game and the compositor (`VK_LAYER_R36_nobgra`) now waits for the GPU before presenting. On an R36S, Ghost of Sparta in combat went from 28.5 to 29.4 fps (3 runs each) with fewer irregular frames (180 against 260 three-refresh intervals per window): no cost. The tearing itself was checked by eye on the R36 Ultra. On by default; `/storage/.config/r36vk.conf` with `waitidle=0` switches it off (debugging).

What else changed for PPSSPP users (even with Vulkan off):
- **Audio through native PipeWire** (see "System" below: it is no longer only PPSSPP).
- **God of War: Ghost of Sparta** gets the 30 fps hack automatically via `PSP/SYSTEM/compat.ini`, only if you have no `compat.ini` of your own (`set_setting ppsspp.gos30 0` removes it).
- **Frame skipping fixed (PSP games drew only 1 of every 4 frames).** The old PPSSPP config shipped on RK3326 had `FrameSkip = 3` with automatic skipping off, which in PPSSPP 1.20.2 means a **fixed** skip: it draws 1 frame and skips the next 3, always. It is now `FrameSkip = 0`. The launcher migrates an existing config **once** (marker file `.zenith-frameskip-migrated`; afterwards your own choice is respected). If you turn on automatic frame skip in the game options of zfront, the launcher sets a skip limit of 3 so that it actually works. Per-game configs (`<ID>_ppsspp.ini`) with a saved FrameSkip are not migrated.
- **PPSSPP now closes in an orderly way and keeps its shader cache** (fewer stutters when a game starts). PPSSPP only saves the shaders it prepared if it exits normally, and the system always killed it abruptly, so every session began cold. Leaving a game now asks PPSSPP to close (the physical exit combination, QUIT GAME in the Home menu, and the phone/web quit all go the same way) and, if it has not exited after about 7 seconds, closes it abruptly as before. **Leaving a PSP game now takes about 1-2 seconds instead of being instant.** On an R36S: exit in 1.0-1.8 s (about 2 s with the Home menu paused), and the next start showed 0-1 stalls above 66 ms in the first 15 seconds against 3-8 without cache (one run per case). With OpenGL on libmali PPSSPP writes no shader cache, so there is no gain there; other emulators exit as before.
- **Arm Mali kernel driver patch 007** (Zenith, not from Arm): with little free memory and the Arm Vulkan driver, the GPU driver could fail to place a memory region and log an avalanche of `Failed to map memory on GPU` lines (hundreds per second, enough to eat 19-46 % of a core in the log service). The warning is now rate-limited and the zone the driver reserves for that memory gets 1 GiB of extra address space. Tested on the R36 Ultra with the memory squeezed to 44-78 MB free: 3 runs out of 3 with 0 failures (before, about 500 failed attempts in 3 seconds).- **CPU governor for PSP: automatic by default.** New installs (and updated consoles with no setting yet) run PSP games with the CPU governor on `ondemand` instead of `performance`. On the R36 Ultra this gave the same fps in Ghost of Sparta and was the only setup where the kernel never throttled in 10 minutes of play (at maximum CPU it throttled at about 85 C after about 5 minutes). A governor you already had in `system.cfg` is never changed. Other systems are unchanged.
- **PPSSPP menus fit small screens** (RK3326 only): proportional side tabs and popups, pause menu, button mapping and file-browser toolbar at 640x480 and 720x720. The **pause menu was seen on an R36S at 640x480**.

The battery gauge now learns the real capacity of your battery after a full cycle (measured on an R36S: 3000 → 3300 mAh), so the percentage and the time estimates get more accurate. It needs a full cycle from empty, charging with the console on; it does not learn yet when charging with the console off (see "Battery").

## Other changes
**zfront and themes**
- **"Add games" card in the carousel when the library is empty:** the console starts with it selected; A opens the web transfer with a QR code (without a network it explains how to copy games with a microSD card). The carousel's help bar is shorter: Choose / Select / Menu. The Zenith themes have an `addgames` logo for it. Seen on an R36S.
- **Web transfer opened from that card or from the welcome is temporary:** not saved, and it turns itself off after about 20 minutes without requests (measured: closed after 21.5 min). Opened from Network > Transfer files it stays on, as before. **The PIN lock-out grows:** the first lock is 30 s and each next one doubles, up to 5 min (measured 29 s and 62 s).
- **Interface polish**: long lists no longer have their last row hidden behind the help bar; the battery page title matches its menu entry; the first letter after "¿" or "¡" is capitalised correctly (seen on an R36 Ultra); Zenith Minimal with very few systems no longer shows a repeated card.
- **First-boot welcome** on a new install: language, Wi-Fi, time zone (region, then zone, each row showing the resulting time) and a few hints; Start skips it. Seen on an R36 Ultra. Updating by OTA does not show it; open it any time in System > Language & time > **Run welcome again**.
- **In-game Home menu** (experimental, off by default; GAME SETTINGS > HOME MENU DURING GAMES): hold START + R1 (or R2 / L2, your choice) for one second while a game runs to pause it and open a side panel over the dimmed game with battery, brightness, volume, **Resume** and **Quit game** (it asks first). B or Start resumes. Save/Load state appear only for RetroArch with its network commands on, which they are not in this image. Seen on an R36S (640x480) with PPSSPP: the panel pauses and resumes the game, also after switching PPSSPP's engine from its own menu, and Quit game leaves in about 1-1.5 s. The panel takes 1-2 s to draw after the button combination.
- **Play time limit** (GAME SETTINGS > PLAY TIME LIMIT, off by default): daily limit for children; only games count. When the time is up the game is closed like QUIT GAME and no other game starts that day. Seen on an R36 Ultra. On RK3326 there is **no warning beforehand** (the 5-minute warning needs RetroArch network commands, which are off in this image; standalone emulators never get it).
- **Bedside clock while charging** (off by default; SCREENSAVER SETTINGS) and a **Charging** pill when the charger is plugged in (not shown while a game is open). The clock was seen on an R36 Ultra.
- Interface in **34 languages**, all precompiled in the image (36 locale codes): choosing a language no longer waits ~22 s for `localedef` on the next start; zfront shows its first frame in about 2 s (R36S: 24.7 s before, 1.8 s now).  English, Spanish, Brazilian and European Portuguese, French, German, Italian, Catalan, Czech, Welsh, Greek, British English, Mexican Spanish, Basque, Finnish, Galician, Hungarian, Indonesian, Japanese, Korean, Norwegian (Bokmal and Nynorsk), Dutch, Occitan, Polish, Romanian, Russian, Slovak, Swedish, Turkish, Ukrainian, Vietnamese, Simplified and Traditional Chinese. Corrections to any translation are welcome. Formats, plurals and the layout test (every screen, every supported resolution, every theme) pass in all 34. Arabic and Hebrew are not included (no right-to-left text yet).
- System menu: **GPU DRIVER** reads "Arm (official)" / "Open source (Panfrost / PanVK)", and "default" values are shown translated. System information has no stray error line and its labels are translated.
- Themes: **Aurora, Swiss and Industrial** (light and dark) polished, with readable text at every resolution (at 640x480 rows are 16-20 px instead of about 12); Drum, Deck, Mosaic, Magazine, Hero and Cinema were dropped. Selector style, title alignment and list text case are honoured by all themes. Unreadable selected row in dark Iconic fixed.
- Menus cleaned up: seven options that did nothing removed, "GAMES" is now "GAME SETTINGS", one screensaver setting ("Screensaver after"), new **DATE & TIME** (set the clock by hand, no Wi-Fi), duplicates removed, the power LED colour is kept across reboots, 
- **File transfer from a phone or PC:** resumable large uploads, cancel button, free space shown, game list refreshed after an upload, title-only edits, names like "Foo... (USA).nes" accepted; new **built-in file manager** (replaces Tools > File Manager: natural sort, system folders protected, warns before copying over 4 GB to FAT32) and a **Cloud** tab (WebDAV, SFTP, FTP, Google Drive and Dropbox; the last two take a code from `rclone authorize` on a PC) with **Syncthing** inside it (console code, add a device, 3 fixed folders: saves, states, screenshots). The Syncthing panel listens only on 127.0.0.1.
- Bluetooth icon fixed (seen on an R36S); Drastic keeps a copy of its config before a reset.

**Tools**: Duckstation and EKA2L1 visible; Drastic keeps a copy of its config before a reset; cleaner scripts; Offline Upgrade and scan scripts fixed.

**Lighter image (removed, not just hidden)**: Wine, Box86, Box64 and the Windows system; MAME (current) and MAME 2015; old PUAE and Amiberry (Amiga keeps PUAE 2021 and UAE4ARM); BaiduPCS; ZeroTier (Tailscale and WireGuard stay); a few Tools scripts.

**System**
- **Audio:** `SDL_AUDIODRIVER` is now `pipewire,pulseaudio` for the whole session (sway and weston profile, the interface, the EmulationStation service and the PPSSPP launcher): SDL talks to PipeWire directly and falls back to PulseAudio if that is not possible. It affects every SDL program (interface sounds, PPSSPP, standalone Mupen64Plus, DraStic); RetroArch, Flycast and Mednafen do not use that variable. Measured on the Ultra: the sound system's CPU use goes from 19-42 % to 1-4 % of one core in the SDL emulators, with no new audio gaps in the recordings (PPSSPP: 29.97 to 33.58 fps in one scene). The fallback was shown on the Ultra: with PipeWire made unreachable for the process, SDL loaded `libpulse` and the output signal stayed the same. **PortMaster ports are meant to keep `SDL_AUDIODRIVER=pulseaudio`** (set when PortMaster's `control.txt` loads; on a console updated by OTA that file is only refreshed the first time you open PortMaster, and PortMaster's own zip may overwrite it), because a port that ships its own old SDL2 (before 2.0.16) does not understand the list.- **Telemetry removed:** the image no longer sends usage statistics to a ROCKNIX server. (v0.2.0 and v0.2.1 still do.)
- **Time zone:** new installs default to **UTC** (it used to be New York); change it in the menus and it survives updates. **If you update by OTA from v0.2.x you keep the time zone already saved (New York in v0.2.0)**, so the clock may look hours off until you change it in the menus.
- Settings and the Wi-Fi key are written atomically, so a power cut no longer leaves empty files. The game card is synced on shutdown.
- Factory reset no longer mentions Amiberry.
- **Switching to EmulationStation keeps a working theme:** if the theme chosen in zfront does not exist for EmulationStation (the Aurora, Swiss and Industrial skins), EmulationStation starts with Zenith light or dark, according to the skin's mode, and zfront gets its skin back when you return to it. Measured on an R36S with the installed image: EmulationStation starts and draws with the factory theme.

**Battery**
- **New rk817 battery meter** (kernel patch 038): the percentage moves at most 1 % per 8 s, never against the charge direction, stays at 99 % until a real full is detected, and learns capacity between clean anchors; it detects the charger by current on boards whose PMIC cannot see it. On one R36S it detected charging and "Full" correctly. On an R36S, a full discharge (about 2 h 48 min of demanding play at maximum brightness, from 97 %) went down smoothly, about 1 % every 100 s, never jumping or rising. The meter starts by assuming 3000 mAh and this battery gave about 3.27 Ah, so it was pessimistic and held 1 % for the last ~14 minutes; the safety power-off acted at 3.2 V.
- **The meter learns the real capacity** (measured on 18 Nov on that R36S): after discharging until the console powered off, plugging in, switching on and charging **with the console on** up to Full without rebooting, the capacity went from 3000 to 3300 mAh (the counter reached 3361) and it was kept after a reboot. Each learning step is limited to +10 %, so a second cycle refines it. It only learns if the console starts from empty and charges switched on in that same boot; **charging with the console off does not teach it yet**. With a worn pack or in the cold the figure may be wrong.
- **Time left:** the battery screen shows an estimate "left, in the menus"; inside a game, the Home panel (START + R1) shows the time left "with this game" from the session's average use, after 3 minutes of play and never while charging.
- The safety power-off (`battery-guard`) now only acts on a 0 % reading when the voltage is also below 3.5 V (three readings in a row); a 0 % with normal voltage never powers off. The old criterion (below 3.2 V, three readings 10 s apart, not charging) stays.

## Known issues and limits (Limitaciones conocidas)
- **Vulkan is experimental.** PanVK is Vulkan 1.0 and not conformant. The Vulkan driver from Arm is a closed binary. If a game misbehaves with Vulkan on, switch back to OpenGL in PPSSPP's Settings > Graphics > Backend.
- With Vulkan, the kernel log could show `Failed to map memory on GPU` (6 lines in about 41 runs measured before patch 007, at close time or at load; no visible effect). Patch 007 (see above) removed the avalanche seen with very low free memory in 3 of 3 test runs. In the player-conditions runs on the R36S (55 runs, both Vulkan drivers and OpenGL) the kernel log showed no GPU error at all. On an R36S, `mali: Failed to allocate 513 pages` was seen once when closing PPSSPP (physical memory).
- **Updating from v0.2.x (OTA) needs about 3.5 GB free on the system storage** (the download plus its unpacked copy), although the updater only asks for 2 GB. With less, the download finishes but the update fails at reboot and the console simply starts v0.2 again (nothing is lost): free some space, or write the image to a card instead. The updater of v0.2 also shows an "unofficial modifications" warning before it starts; that is expected. The update does not add `!Zenith DTB Selector.exe` to the card (the older `ZenithDtbSelector.exe` stays and still works).
- **EmulationStation users** (if you switched the frontend): everything launches as before, but the new zfront features (welcome, Home panel, play-time limit, bedside clock, new languages) are not there, and the per-game "graphics backend" option of EmulationStation changes PPSSPP's engine for all games.
- **Updating from v0.2.x (OTA):** you keep the time zone already saved (New York in v0.2.0), so the clock may look hours off until you change it in the menus (DATE & TIME).
- **Some PPSSPP options are set by the launcher at every start** from the per-game options of zfront, so changing them inside PPSSPP's own menus is lost the next time: internal resolution, VSync, skip buffer effects, FPS counter and automatic frame skip. Change them in the game options of zfront. The graphics engine (Vulkan/OpenGL) is no longer one of them: choose it in PPSSPP. Everything else you change in PPSSPP is now saved when you leave.
- **R36S: the charging icon/status can flicker (oscillate) when the battery is almost full** (5 changes in 10 minutes were seen with the current near 0 mA; probably the driver deduces "charging" from the current when it has no real charger signal, but this was not confirmed). The percentage is not affected.
- **The battery meter does not learn the capacity while charging with the console off** (it needs a full cycle: start from empty and charge switched on in the same boot, see "Battery"). The "with this game" estimate appears after 3 minutes of play.
- **The notice "Web transfer turned off"** (after the 20-minute timeout) lasts 3.5 s, so it is rarely seen: the screen is usually off by then.
- **PortMaster ports should keep `pulseaudio`** as the audio driver, but after an OTA update that only holds once PortMaster has been opened once, and a port that ships its own old SDL2 (before 2.0.16) could fail to start its sound otherwise.
- The older tables in the performance document are lab figures (fixed clocks 1296/600/666 MHz); the R36 Ultra figures with factory settings (Ghost of Sparta 28-29.4 fps of 30 in combat, Chains of Olympus 27, F1 2009 52.6 of 60, Tekken 56.2 of 60; n = 1-2, automatic key presses) were taken before the tearing fix.
- Web transfer: two simultaneous uploads of the same name are not detected; the file manager still blocks while reading huge folders.
- Small zfront blemishes known: some words still come in English from the shared EmulationStation catalog (e.g. "Off" in Spanish); the welcome's Wi-Fi step opens the normal Wi-Fi menu. The Home panel takes about 1–2 s to appear after the game pauses. In some languages the "Controller & Bluetooth" page title is cut short (e.g. Spanish at 720x720).
- The cover scraper (ScreenScraper) does not work: no developer credentials are built in.
- Wi-Fi, SSH, Samba and the phone file transfer are off by default; the root password is `rocknix`: change it before turning SSH or Samba on.

## Downloads

| File | What it is |
|------|------------|
| `Zenith-v0.3.0-20261118-RK3326.img.gz` | Full image for a new SD card (write this one) |
| `AURKNIX-RK3326.aarch64-20261118.tar` | Update package, used by the built-in updater |
| `*.sha256` | SHA-256 checksums (one per file): verify them before writing anything |

## Installation
* **Which file?** New install: write `Zenith-v0.3.0-20261118-RK3326.img.gz`. Already running Zenith: update from the console (`System > Updates & downloads > Software updates`); the `.tar` is only used by the updater.
* Check the `.sha256`, decompress, and write it to an SD card (Rufus, Balena Etcher, Raspberry Pi Imager, `dd`). Check the target disk first: everything on it is erased.
* Choose your device tree with **Zenith DTB Selector** on the boot partition ([docs](https://github.com/zenmode-adri/Zenith/blob/main/docs/DTB-SELECTOR.md)).

## Licenses
Zenith is made of many components, each under its own license; some are for non-commercial use only.
- The Arm Mali userspace drivers (libmali g24p0 and the Vulkan driver g29p1, with its Wayland WSI layer) are **Arm's closed binaries**, redistributed under the Arm Mali end-user licence (`LES-PRE-20769`); its text is in [`licenses/Arm-Mali-EULA.txt`](https://github.com/zenmode-adri/Zenith/blob/main/licenses/Arm-Mali-EULA.txt) and next to the driver on the image (`/usr/lib/mali-vk/`). "Arm" is used here only to identify the driver. The g29p1 blob is downloaded at build time from a third-party mirror (JeffyCN/mirrors, commit `bf621d15`), with a SHA-256 check.
- The Rockchip `libmali-hook` that loads that driver is **GPL-2.0-or-later**; its source is included in the repository (`libmali-vk/sources/hook/`) and is built with the image.
- PanVK and Panfrost are part of Mesa (MIT). The private Vulkan loader is Apache-2.0 and the SDL2 with Vulkan is zlib.See [LICENSE.md](https://github.com/zenmode-adri/Zenith/blob/main/LICENSE.md), [CREDITS.md](https://github.com/zenmode-adri/Zenith/blob/main/CREDITS.md) and the `licenses/` folder.
