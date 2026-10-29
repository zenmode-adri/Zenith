# Zenith changelog

Zenith is an unofficial fork of [AURKNIX](https://github.com/AveyondFly/distribution_rocknix) (itself a fork of
[ROCKNIX](https://github.com/ROCKNIX)) focused on fast boot, a polished EmulationStation interface and fast
suspend on **RK3326** handhelds. Numbers below were measured on a **R36 Ultra**; other RK3326 models are untested.
Dates are the build dates. Every entry has a git tag (named in `code`) and details in the commit it points to.

## Next image (contents tested in the DEBUG build `d955804`, installed on an R36 Ultra; the clean image built from the same code has NOT been booted: try it on a spare SD card first)

### Choosing the right device tree (DTB)
- **Zenith DTB Selector replaces the old DTB picker.** One small program for Windows, Linux and macOS (window in
  Chinese, English and Spanish) that finds the SD card by itself, lists 86 models grouped by brand, shows exactly what
  will change before writing, keeps a dated backup of `extlinux.conf` (the 30 newest, restorable from the tool) and
  never adds the `ssh` flag. It can also import your own `.dtb`. The Windows version is copied to the root of the
  card as `ZenithDtbSelector.exe`. Tested on a copy of the real card of an R36 Ultra (all command-line cases and the
  web interface) and on the console itself: import, apply, reboot with the chosen DTB, restore.
- **DTB test mode ("try the next one") for clone consoles whose DTB cannot be guessed.** From the selector on a PC
  you start the mode; on every boot the console shows a test card and waits 12 s for a button (any pad button or
  VOL+/VOL-, never the power button) to be HELD: if you hold one, that DTB stays; if not, the next candidate is
  written and the console restarts. Candidates are ordered so the likeliest come first (18 of the 77 DTB). When they
  run out the original `extlinux.conf` comes back. Only the `FDT` line is ever touched, writes are atomic, and with
  no `/flash/dtbprobe/state` the script does nothing. **Not yet tested on hardware.**

### zfront, a new optional front-end
- **zfront is a second front-end written from scratch in C++17 (SDL2), installed next to EmulationStation.**
  EmulationStation stays the default; `Tools > Switch frontend` changes between the two and restarts the front-end
  without waiting (setting `system.frontend`). zfront reads the same `es_settings.cfg`, the same gamelists, the same
  Zenith theme and the same scripts, so nothing has to be set up twice. `zfront-88212b3`
- **Lighter and faster to come up** (R36 Ultra, large library): first frame 1.0 s instead of 4.1 s, 36 MB of its own
  memory instead of 143 MB, and it sleeps between events instead of polling (2 CPU ticks per 10 s idle). Starting
  RetroArch from the front-end takes 0.91 s instead of 1.18 s and 40-100 MB more RAM stay free while a game runs. Real gameplay is the same: God of War (PPSSPP) shows no frame-rate difference between the two
  front-ends (the only game measured).
- **Menus grouped by intention** instead of one long "System" list (about 50 rows): 8 groups, an Inactivity block, the
  panel adjustment and language/time as sub-screens, a floating help pill shared by the carousel, the lists and the
  menus. Brightness and volume keep their own buttons.
- **Motion tuned for a small screen:** exponential carousel glide that follows the EmulationStation curve, a slower
  repeat when the d-pad is held (it used to race through the carousel), windows that fade in and out, toast messages
  above the help bar, and the main menu and five sub-menus are built in idle time after start so their first open no
  longer stutters (main menu and Games: 1.5 ms instead of 50-90 ms; Display and Battery: worst frame 10-12 ms;
  Controllers and Network are not pre-built because opening them starts Bluetooth/Wi-Fi).
- **"Continue playing" row** under the carousel: Down gives it focus, A resumes from the last save state without going
  through the save-state manager; games whose ROM no longer exists are hidden. **Not tried on the console with real
  ROMs yet**, and the 4:3 (640x480) layout that makes room for it has only been checked with the test harness.
- **Navigation sounds** (scroll, back, menu, launch) with the Zenith theme sounds, only when `EnableSounds` is on,
  mixed to avoid clicks when sounds overlap (still to be confirmed by ear). The audio device is closed 4 s after the last sound, so no stream stays
  open while idle. If you hear nothing, check that `audio.device` is `speakers` (it can be left on `headphone`).
- **Systems reload live:** toggling "Show empty systems" no longer asks to restart the front-end.
- **Suspend from the menu and as a screensaver type** are offered now (they were hidden because zfront looked for a
  `pm-suspend` binary that Zenith does not ship; it checks the real sleep hook and runs `systemctl suspend`). The
  power button is unchanged. Not tried on the console yet.
- Included: `LICENSE` and `NOTICE.md` for zfront. `runemu --prepare` prepares the RetroArch configuration while idle
  and the launch reuses it if nothing changed (start to exec 830 -> about 510 ms). `zfront-88212b3`
- Theme: new menu icons (display, battery, library) and 16 text-only logos of the dark theme redrawn in clean white
  without the white outline. `zfront-88212b3`

### zfront: safety review (28 Oct)
- **Game names can no longer run commands.** `escape_path` (a port of the EmulationStation one) did not escape the backtick, `|`,
  tab and newline, so a ROM called like that could run a command when launched; `%GAMENAME%`/`%SYSTEMNAME%` were not quoted
  either. Both fixed. EmulationStation probably has the same flaw (not checked).
- **User hooks run without a shell** (arguments passed as an array, `game-start` gets the real ROM path); synchronous `quit`
  hooks are killed after 30 s.
- **Broken data no longer crashes it:** an `<image>` that points to a folder, pictures larger than 16 Mpx, XML nested more
  than 64 levels, line breaks inside `system.cfg` values, and a `<game>` bigger than 16 MiB in a gamelist (that entry is
  skipped instead of losing the rest of the list; gamelists with a skipped entry are never rewritten).
- **Long system names end in an ellipsis** instead of covering the game counter, Wi-Fi, battery and clock.
- The HTTP API on port 1234 listens on 127.0.0.1 only. Licences of zfront and of the libraries compiled into it are installed
  in `/usr/share/zfront/licenses`. 58 test programs pass on a PC (also under ASan/UBSan). **Not seen on the console yet:** the
  look of the long-title ellipsis, launching a game with an odd name, and a huge gamelist.

### Opening a game
- **RetroArch games start 0.2-0.5 s faster.** `setsettings.sh` (the script that writes the per-game RetroArch
  configuration right before launch) started about 220 processes per game; it now starts about 40 and produces
  byte-identical `retroarch.cfg` and `/tmp/.retroarch.cfg` files (compared on 14 systems). Time spent in that
  script: 0.70 s -> 0.47 s with logging off (the default), 1.01 s -> 0.51 s with `system.loglevel=verbose`.
  Whole launch, averaged over 7 games (Famicom, Game Boy, SNES, Mega Drive, GBA, N64, PlayStation): 3.54 s -> 3.33 s
  with logging off, 3.90 s -> 3.41 s with verbose logging. `launch-opt-2`
- Not affected: standalone emulators (PPSSPP, Flycast, DraStic, DuckStation, ScummVM...) do not go through
  `setsettings.sh`.

### GPU driver
- **The open-source GPU driver (Panfrost) now works on the RK3326, so the driver choice in the menu is usable.** Until
  now selecting it left the console on software rendering: the GPU and the RAM share one voltage rail (`vdd_logic`),
  a single-value `opp-microvolt` is read as "exactly X mV", and Panfrost's exact request clashed with the RAM's
  minimum (1050 mV at 666 MHz) -> `Restricting voltage`, error -22, and the GPU never probed. The Mali driver (libmali)
  was not affected because its own patch already asks for a minimum. New kernel patch
  `036-opp-single-voltage-is-minimum`: when a table gives one voltage the OPP core now asks the regulator for "at least
  X", so the highest minimum of GPU and RAM wins. It works with any device tree (also imported ones) and changes
  nothing on the CPU rail (same latency, same voltages: 1125 mV at 1008 MHz, 1300 mV at 1296 MHz). Changing the driver
  still needs a reboot.
- **Safety net for the GPU driver choice.** If Panfrost is selected but no GPU gets bound to it at boot (the module
  loaded but the probe failed), `gpudriver --start` saves libmali as the driver and reboots once, instead of leaving the
  desktop on software rendering. It reboots rather than swapping drivers in place because, on the Ultra, loading
  `mali_kbase` after Panfrost had taken the GPU in the same boot hangs the kernel (measured). It only reboots when the
  setting was really saved, so it cannot loop. Logged with `logger -t gpudriver`. `gpudriver-fallback-reboot`
- Measured on an R36 Ultra (God of War: Chains of Olympus, same save state, 1x): Panfrost 6.9-7.0 fps vs libmali
  7.2-7.3 fps with the governors on automatic (-5 %); with GPU 600 MHz and RAM 666 MHz fixed, 7.7 vs 7.4 fps. With
  Panfrost the RAM now reaches 666 MHz while the GPU sits at 200-400 MHz and the kernel log shows no voltage
  warnings. glmark2: 414 vs 419. **Panfrost is not faster: that game is limited by the emulation thread and memory
  latency, not by the GPU** (the GPU is about 20 % busy), so libmali stays the default. Only this game and glmark2
  were measured. `opp-single-voltage-minimum`
- When the GPU is the bottleneck (God of War with frame skip off at 2x/3x/4x) libmali is faster: 10.2/6.0/3.7 fps vs
  6.9/4.9/3.5 (+48 %/+24 %/+7 %). Mario Kart 64 in a race is not GPU-bound (same fps at 640x480 and 1920x1440) but has
  many draw calls per frame (~460) and Panfrost's per-call cost shows: 36 vs 32 fps. Power draw is similar. libmali
  stays the default.

## v0.1.0 - 2026-10-23 (first public release, pre-release)

Image `AURKNIX-RK3326.aarch64-20261023.img.gz`. Tag `v0.1.0`. No update package yet: nobody has Zenith installed, so
the first release is the full image only.
Same sources as the development image `44d6084` below, built as a **clean release image**:

- No Wi-Fi network, SSH, Samba or verbose logging is enabled by default: turn them on from the menu when you need them.
  The development builds enabled all of them.
- `sshd` no longer starts just because the kernel command line contains `ssh` (the AURKNIX `extlinux.conf` had it, so
  SSH was always on, with the factory password). SSH is now opt-in through the `ssh.enabled` setting.
- Checked on the files: no saved Wi-Fi name or key anywhere in the image, no SSH keys.
- **Not yet checked:** the first boot of this exact image from a freshly written SD card. The same sources run on an
  R36 Ultra as a development image.

## 2026-10-23 - image `44d6084` (first public candidate)

- **Carousel cards with an icon** (Music, Tools, Favorites, All games, Last played, Never played, Collections, Books):
  the name is now real EmulationStation text (about 19 px) instead of a word baked into a 360x300 bitmap that was
  scaled down 4x (about 11 px). `zenith-77-etiquetas-contraste`
- **More contrast on secondary text** in both themes (dark: 4.5:1 -> about 8:1, light: 4.5-5.3:1 -> about 7:1).

## 2026-10-22 - images `4868372` and `8fef418`

### Suspend and resume
- "Suspending..." notice stays on screen at least 1 s, then the panel turns off; it no longer reappears on wake-up
  (ES patch 0047). `fast-10-aviso-suspension`
- Suspend takes 3.5 s -> 2.3 s and resume 8 s -> 4.3 s: Wi-Fi shutdown, panel and the rest run in parallel, panel
  colour settings are restored all at once. `fast-11-suspension-paralela`
- CPU/GPU/DMC governors are restored first on wake-up (they were left in powersave for 3.5 s, which made the first
  button press stutter). `fast-12-gobernadores-primero`
- The panel backlight is always switched on when EmulationStation starts and on wake-up: if ES was restarted while
  the screensaver was active the screen could stay black with full brightness. `fast-17-luz-siempre`

### EmulationStation
- First button press after idle: 225-245 ms -> 11 ms. The audio device that ES closes after 5 s of silence is now
  reopened on a worker thread (ES patch 0049; the one click that wakes it up is silent). `fast-13-primer-boton`
- System carousel: the selected card no longer touches its neighbours (all four gaps are 22 px), the neighbours no
  longer jump when moving, and holding a direction keeps a steady speed (1.96-10.62 -> 4.74-6.59 positions/s).
  ES patches 0052 and 0054, theme `scaledLogoSpacing`. `zenith-76-huecos-iguales`, `fast-18-carrusel-suave`,
  `fast-20-clamp-carrusel`, `fast-21-carrusel-mantener`
- Side drawer (GO TO MANUFACTURER / LETTER...): opens above the Z logo and its help bar is centred on it, also on small
  screens (ES patches 0050, 0051). `fast-14-cajon-sobre-z`, `fast-15-ayuda-en-cajon`, `fast-16-ayuda-cajon-pequenas`
- Quit menu: restart, suspend and shutdown no longer ask for confirmation (fast shutdown, which skips saving
  metadata, still does; ES patch 0053). `fast-19-salir-sin-confirmar`

### Tools menu
- R36 Tuner and EKA2L1 entries now have a description and an icon; the `MOD_TOOLS` folder has a description.
- 15 dead entries of apps that this build does not ship are gone (ES logged 15 warnings per boot, now 0).
- Every tool has its own icon, and the icons are visible on the dark theme (lighter background).
- Baidu Sync is hidden, and the Duckstation and PortMaster scripts are fixed. `tools-1-limpieza`, `tools-2-imagenes`

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

## 2026-10-19 - EmulationStation interface (patches 0017-0043)
- Switches, sliders, on-screen keyboard, search field, dialogs, tabs, scrollbar, header alignment, battery text,
  theme downloader, game options, scraper fixes, window sizes relative to the status bar. `zenith-54-contraste` .. `zenith-86-led-sin-irq-off`
- New LED driver for the R36 Ultra joysticks. `zenith-74-led-joysticks`

## 2026-10-15 - Wi-Fi and PPSSPP
- Faster Wi-Fi menu (busctl, prescan; ES patch 0016). `zenith-53-wifi-fast`
- PPSSPP `RenderDuplicateFrames = False`.

## 2026-10-08..10-11 - Zenith first release work
- Own first-boot screens, boot banner, language question and credits. `zenith-1-firstboot`..`zenith-13-debug`
- Own light and dark EmulationStation theme ("Zenith"), volume OSD, bundled themes, brightness notice, R36 Tuner Next
  (CPU governor and frequencies, turbo steps, benchmarks, monitor). `zenith-14-tema-es` .. `zenith-52-bigger-logos`
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
- Tested only on the **R36 Ultra**. The other RK3326 devices, and every non-RK3326 target inherited from upstream,
  have not been tested with these changes.
- The Zenith early-boot mode, the Wi-Fi suspend fix (RK915 chip) and the LED driver are device specific.
- A fixed GPU clock of 560 MHz freezes the console; do not add it back to the frequency table.
- The eMMC tools (backup, install, restore) come from upstream and have not been tested with Zenith.
