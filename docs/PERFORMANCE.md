# Performance of Zenith v0.3.0 (PSP, Vulkan)

Figures and conditions only: in these scenes, on these consoles. "Arm" only identifies the driver.

## Main table (17 Nov) - R36S, 10 PSP games, player conditions (fps; target 30, Tekken 60)
**Conditions: PLAYER.** R36S (with a fan), 640x480, the image's own launcher (only `--state` and a frame-logging layer/preload added), factory governors **with nothing fixed** (CPU `ondemand`, GPU and memory `simple_ondemand`), the user's `ppsspp.ini` as is (1x, `FrameSkip 0`; only `GraphicsBackend` changes between runs), no key presses, zfront stopped during the run. Same saved scene per game as R26. Each run starts at 55 C or lower, 18 s of loading, 30 s window; "fps" = presents with 3 or more draw calls over 20 s from when the state has loaded (Ghost of Sparta: only the live part, 471 frames). Arm columns: two runs per game; PanVK column: two runs in the five games that separate the engines and one in the other five. "Stalls" = intervals of 45 ms or more per minute (fewer is smoother). Kernel: 0 bytes of GPU errors in the 55 runs.
Images: the Arm columns and the PanVK column come from two different pre-release test builds (Arm was not repeated on the second: assumed to make no difference). Arm Vulkan with the `nobgra` wait (the tearing fix).

| Game | OpenGL (Arm) | Vulkan (Arm) | PanVK | Stalls >=45 ms/min GL / VK Arm / PanVK |
|---|---|---|---|---|
| God of War: Chains of Olympus | 18.2 | 27.5 | 27.8 | 1089 / 75 / 42 |
| God of War: Ghost of Sparta (live part) | 25.8 | 30.3 | 30.3 | 179 / 23 / 33 |
| F1 2009 | 29.7 | 36.7 | **39.5** | 15 / 6 / 3 |
| Tekken: Dark Resurrection (target 60) | 55.3 | 59.7 | 59.7 | 9 / 3 / 2 |
| FlatOut: Head On | 28.1 | 29.2 | 28.9 | 63 / 27 / 26 |
| Assassin's Creed: Bloodlines (at cap) | 29.6 | 30.2 | 30.2 | 11 / 8 / 0 |
| GTA: Vice City Stories (at cap) | 30.4 | 30.1 | 30.1 | 0 / 2 / 0 |
| Daxter (at cap) | 30.3 | 30.2 | 30.1 | 5 / 0 / 6 |
| Castlevania: Dracula X Chronicles (at cap) | 30.1 | 30.1 | 30.1 | 3 / 0 / 0 |
| Tenchu: Shadow Assassins (at cap) | 30.4 | 30.2 | 30.1 | 5 / 3 / 12 |

Mean of two runs (between runs: Vulkan Arm CoO 28.3 / 26.6, F1 36.2 / 37.1; PanVK CoO 27.7 / 27.8, F1 39.4 / 39.6); PanVK has a single run in the five games at the cap.
**Factual reading (the headline):** in these scenes **the two Vulkan drivers are on par** (within 1 fps in 9 of 10 games); **PanVK is ahead in F1 2009** (39.5 against 36.7, +8 %; both its runs sit above both Arm runs); **both are well above the OpenGL of Arm** in the five games that separate the engines (Chains of Olympus +51 % Arm / +53 % PanVK, Ghost of Sparta +17 % / +17 %, F1 +24 % / +33 %, Tekken +8 %, FlatOut +4 % / +3 %) with far fewer stalls (CoO 1089 to 75 / 42; GoS 179 to 23 / 33). The five games at the cap cannot tell them apart. The stall differences between the two Vulkan drivers (GoS 23 vs 33, CoO 75 vs 42) are inconclusive (few runs, overlapping ranges).
Against the lab table (Table 1): the order and size hold with factory governors; only F1 2009 on Arm Vulkan falls (36.7 against 39.5; ASSUMPTION: CPU `ondemand` instead of fixed at 1296 and/or the `nobgra` wait, not separated). Arm OpenGL comes out a little better than in Table 1 (+3-6 % in CoO, GoS and F1; unexplained).
**Limits:** one scene per game, standing, no key presses; one R36S with a fan (max T 51-60 C); OpenGL measured with a preloaded logger and Vulkan with a logging layer (each may cost a little); no warm-up run (shader caches mixed cold and warm); per-present metric of the game, not per screen refresh; PanVK has one run in the five games at the cap; Arm and PanVK on different images. There is no Panfrost GL column in these conditions. Not human play and not a long session.



## Method (applies to all three tables)
- One saved scene per game, launched through the image's own PPSSPP launcher (`start_ppsspp.sh`), 1x internal resolution, no frameskip. No keys pressed, except in the unlocked-FPS test (Table 2), where a virtual keyboard attacks from 15 s after launch.
- Fixed clocks: CPU 1296 MHz, GPU 600 MHz, RAM 666 MHz (not the stock governors). Each run starts at 55 C or lower (R36S) or 53 C or lower (Ultra). In a separate test the starting temperature changed results by 14-23 %, so only runs with a similar starting temperature are compared.
- "fps" = new frames presented per second, counted by a frame-logging layer inside the process. R36S: 20 s after the saved state loads (the notes for the Panfrost extension say 30 s; this could not be reconciled from the notes). Ultra: the whole fight, 959 frames.
- Runs: R36S libmali/Arm columns, 2 runs per engine; Panfrost columns, 1 discarded warm-up and 2 interleaved runs per engine (1 run in the 5 games that sit at the 30 fps cap). Ultra: 5 runs per path over 2 boots. Measured noise: 0.3-0.6 fps within a path, up to 1.3 % drift between boots (Ultra); at most 0.2 fps between runs on the R36S (0.7 in one case).

## Limits (please read before the tables)
- **One scene per game**, chosen by us, not a full playthrough. 10 PSP games on an R36S, one game (God of War: Ghost of Sparta) on an R36 Ultra.
- One unit of each console. The R36S used has a fan that stock units do not have, so its temperatures are not a reference.
- (Tables 1-3, LAB) Fixed clocks, not the factory governors: real play can differ; the main table at the top uses factory governors.
- 5 of the 10 games sit at the game's own 30 fps cap in the chosen scene and cannot tell one driver from another (they only show that the game starts and looks right).
- libmali GL and Arm Vulkan were measured in a different boot (another time of day, another temperature) than the Mesa engines.
- Ghost of Sparta (R36S table): only the live part of the scene (471 frames); Kratos dies after ~20 s.
- The "Mesa 26.0.4 upstream" columns are our own builds of Mesa 26.0.4 **without the Zenith patches, without PGO and without Zenith's drirc configuration** (glthread and other settings). Part of the difference to the Zenith columns may come from that configuration rather than from the patches; an upstream build with Zenith's drirc was not measured (assumption). For PanVK the difference also mixes patch 020 (`PANVK_OPT=4`, `PANVK_CPU_INDEX_MINMAX=1`), compiler patches and PGO, not separated.
- Final screenshots were compared by eye (no pixel diff): no visible graphical faults in any engine.
- Kernel messages `Failed to map memory on GPU` with Arm Vulkan: 6 lines in about 41 runs, no visible effect.

> **Note:** Tables 1-3 and the 16 November section below are earlier lab or indicative measurements of the same v0.3.0 work (fixed clocks, earlier pre-release test builds). Where they differ from the main table at the top (player conditions), the main table is the reference.

## Table 1 - R36S, 10 PSP games (fps; target 30, Tekken 60)
**Conditions: LAB.** R36S (with a fan), image `93107dae`, CPU 1296 / GPU 600 / RAM 666 fixed, saved scene with no key presses, 20 s after the state loads, start at 55 C or lower, 1x, `FrameSkip = 0` set by hand. 2 runs per engine.
Columns: **libmali GL** (OpenGL, Arm driver g24p0, the default up to v0.2.x) · **Arm Vulkan** (libmali g29p1) · **Panfrost GL (Zenith)** (Mesa 26.0.4 with Zenith patches and PGO) · **PanVK (Zenith)** (Mesa 26.0.4 with patches and PGO, `PANVK_OPT=4`, `PANVK_CPU_INDEX_MINMAX=1`) · **Panfrost GL (Mesa 26.0.4 upstream)** (no Zenith patches, no PGO, no Zenith drirc) · **PanVK (Mesa 26.0.4 upstream)** (same, no variables).

| Game | libmali GL | Arm Vulkan | Panfrost GL (Zenith) | PanVK (Zenith) | Panfrost GL (upstream) | PanVK (upstream) |
|---|---|---|---|---|---|---|
| God of War: Chains of Olympus | 17.1 | 26.9 | 23.3 | 27.4 | 11.5 | 11.8 |
| God of War: Ghost of Sparta (live part) | 24.3 | 30.3 | 30.2 | 30.1 | 15.1 | 18.2 |
| F1 2009 | 28.9 | 39.5 | 24.8 | 39.5 | 12.8 | 9.8 |
| Tekken: Dark Resurrection (target 60) | 56.0 | 59.4 | 56.5 | 60.1 | 36.7 | 33.1 |
| FlatOut: Head On | 27.7 | 29.2 | 28.3 | 29.5 | 17.2 | 28.0 |
| GTA: Vice City Stories (at cap) | 30.4 | 30.3 | 30.3 | 30.1 | 30.4 | 30.1 |
| Daxter (at cap) | 30.4 | 30.3 | 30.4 | 30.2 | 30.4 | 30.2 |
| Castlevania: Dracula X Chronicles (at cap) | 30.2 | 30.3 | 30.4 | 30.1 | 30.4 | 30.1 |
| Assassin's Creed: Bloodlines (at cap) | 29.8 | 30.3 | 30.3 | 30.1 | 27.1 | 30.1 |
| Tenchu: Shadow Assassins (at cap) | 30.4 | 30.2 | 30.4 | 30.2 | 30.4 | 30.2 |

In the five scenes that separate the engines (CoO, GoS, F1, Tekken, FlatOut), Arm Vulkan and PanVK (Zenith) are within 1 fps of each other in CoO, GoS, Tekken and FlatOut, and equal (39.5) in F1. Panfrost GL (Zenith) is below libmali GL in F1 (24.8 vs 28.9) and above it in CoO (23.3 vs 17.1) and GoS (30.2 vs 24.3). The upstream columns are lower than the Zenith ones in those five scenes (with the drirc caveat above). The eight small PSN games (Angry Birds, Age of Zombies, Brainpipe, Cubixx, Fly Fu, Blast Off, Alien Zombie Death, Dr. Mini Games) were only checked for starting and still running at 38 s on Arm Vulkan; their fps was not measured.

## Table 2 - R36 Ultra, Ghost of Sparta without the 30 fps cap (1x, with effects)
**Conditions: LAB.** Ultra, image `93107dae`, CPU 1296 / GPU 600 / RAM 666 fixed, saved fight of 959 frames with a virtual keyboard from 15 s, start at 53 C or lower, without the 30 fps hack, `FrameSkip = 0` by hand, 5 runs per path. A **worst case with no cap**, not how the game normally runs.
Fight of 959 frames; new frames per second, mean of 5 runs over 2 boots (min-max); start at 53 C or lower; without the 30 fps hack (`ppsspp.gos30 0`).

| Path | fps (min-max) | p95 ms | PPSSPP counter (% of 60) |
|---|---|---|---|
| PanVK (Zenith) | 35.39 (35.09-35.75) | 32.8 | 55-63 (mean 59) |
| Arm Vulkan (libmali g29p1) | 33.95 (33.84-34.10) | 34.2 | 55-58 (mean 56) |
| Panfrost GL (Zenith) | 32.42 (32.22-32.58) | 35.8 | 52-56 (mean 54) |
| libmali GL | 24.08 (23.83-24.43) | 53.5 | 38-42 (mean 40) |

With buffer effects off (`SkipBufferEffects`, 2 runs per path, one boot): PanVK 38.19; Arm Vulkan 37.53; Panfrost GL 37.13; libmali GL 36.91. Differences within the same boot: PanVK vs Panfrost GL +9.2 %; PanVK vs Arm Vulkan +4.2 % (different boots, measured drift up to 1.3 %).
With the factory 30 fps hack (how the image runs Ghost of Sparta, 664 frames of combat): Arm Vulkan 29.9, PanVK 30.05, Panfrost GL 29.4, libmali GL 20.6-22.9.

## Table 3 - Audio through native PipeWire (R36 Ultra, image `920f7184`)
**Conditions: LAB.** Ultra, image `920f7184`, title/intro scenes (not play), nothing audible (the sink monitor is measured), 2 runs per mode, clocks fixed in the runs that report fps.
A = `SDL_AUDIODRIVER=pulseaudio` (up to v0.2.x); B = `pipewire,pulseaudio` (v0.3.0: SDL talks to PipeWire and falls back to PulseAudio if it cannot). Mean of 2 runs per mode. "Sound CPU" = pipewire + pipewire-pulse + wireplumber, % of one core (the SoC has 4). "ms at zero" = milliseconds of exact digital silence in a 20 s recording of the sink monitor; "gaps" = stretches of 20 ms or more.

| Emulator (scene) | Path A to B | fps A to B | Sound CPU A to B | ms at zero A to B | gaps >=20 ms A to B |
|---|---|---|---|---|---|
| PPSSPP, GoW Vulkan (no 30 fps cap) | Pulse client to native | 29.97 to 33.58 | 42.3 % to 4.3 % | 122 to 48 | 0 to 0 |
| Mupen64Plus standalone (MK64, title) | Pulse to native | no fps | 18.9 % to 1.0 % | 11 to 21 | 1 (in 3 of 4 runs, also in A) to 1 |
| DraStic (Mario Kart DS, intro) | Pulse to native | no fps | 24.2 % to 1.3 % | 55 to 75 | 0 to 0 |
| RetroArch N64 (MK64) | ALSA via pipewire-alsa in A and B | 45.3 to 46.9 | 4.2 % to 4.3 % | 1307 to 3361 (1 run) | 6 to 6 |
| RetroArch mGBA | same | 59.99 to 59.98 | 4.5 % to 4.4 % | 3159 to 94 (1 run) | 1 to 1 |
| Flycast (Soul Reaver, intro) | libpulse in A and B | no fps | 12.5 % to 12.7 % | 8753 to 9686 | 152 to 184 |
| Mednafen PCE | ALSA via pipewire-alsa in A and B | no fps | 12.6 % to 13.0 % | 1 to 2 | 0 to 0 |

Emulators that go through SDL and Pulse (PPSSPP, standalone Mupen64Plus, DraStic) become native PipeWire clients and the sound system's CPU use drops from 19-42 % to 1-4 % of a core; RetroArch, Flycast and Mednafen do not use that variable and are unchanged. No new gaps appear with B. The +12 % fps in PPSSPP is larger than the +3 to +5 % seen in earlier runs (windows here ran at 70-78 C; the cause was not isolated). In a separate run (Ultra, R27), pulseaudio gave one 25 ms gap on libmali GL and 2-20 times more ms at zero than pipewire on the same path. The interface (zfront), tested with a temporary override, gave the same 11 sound bursts and the same peak with A and B, also after a cold boot.
**Audio limits:** this measures what PipeWire sees on the sink monitor, not the analogue output; **nobody has listened to the result** (volume was kept at zero in every test); title/intro scenes, not real play; DuckStation and ports with their own SDL were not tested; the raw recordings were not kept (analysis ran on the console).

## User conditions, Ultra and R36S (16 Nov 2026; automatic governors; earlier than the main table)
**New** figures from the night of 15-16 November, on pre-release test builds (Arm Vulkan was the default in those builds), **without fixing the clocks**. They show how an ordinary console behaves; they do not replace the lab tables above and are not comparable with them (different method, different saved states, automatic key presses). Sources: `EVAL-ULTRA-1.md`, `TERMICO-ULTRA.md`, `ULTRA-NOCHE-3.md`, `R36S-NOCHE-2.md`.

**What "automatic" means (important).** Until these tests the launcher kept the CPU and the memory controller (DMC) on `performance` while a game ran and only the GPU was automatic. **In v0.3.0 the default changes:** new installs (and updated consoles with no setting) get `psp.cpugovernor=ondemand`, which puts the CPU on `ondemand` and the memory on automatic (the GPU already was); a value the user already had is never changed. "Automatic" in the tables below = CPU `ondemand` + GPU and DMC `simple_ondemand` = **the v0.3.0 default**. "Previous default (CPU and DMC `performance`)" = what used to be the factory setting and no longer is. On the R36S the CPU was already on `ondemand` when measured. These Ultra figures were taken before the tearing fix (the `nobgra` layer with its wait) and were not repeated; on the R36S the wait cost no performance (28.5 to 29.4 fps, `datos\R38-tearing-arm`).

### R36 Ultra, 60 s of play, Arm Vulkan, automatic governors (n = 1 per game; GoS n = 2)
Own saved states (the R26 ones came from the R36S) and a virtual keyboard; new frames = presents with 20 or more draw calls. A guard of ours at 79 C stopped PPSSPP several times, so some windows are shorter and the hot figures are **indicative**.

| Game (cap) | fps | p95 ms | stalls >50 ms per minute | max T C |
|---|---|---|---|---|
| Ghost of Sparta, combat (30), run 1 | 29.40 | 39.9 | 6 | 72.9 |
| Ghost of Sparta, combat (30), run 2 (2 guard stops) | 28.01 | 41.9 | 7.7 | 81.5 |
| Chains of Olympus (30) | 26.97 | 43.8 | 9 | 77.3 |
| F1 2009 (60) | 52.64 | 26.5 | 1 | 73.3 |
| Tekken: Dark Resurrection (60) | 56.16 | 20.5 | 5 | 78.1 |

- Ghost of Sparta from the start (logos, title, New Game, cutscene, first fight; about 530 s): 29.9-30.0 fps in 20 of 26 twenty-second slices, no gaps over 100 ms except loads (2.0 s at start, 0.7-0.9 s at a scene change).
- Previous default (CPU and DMC on `performance`, automatic GPU) in Ghost of Sparta: 29.97 fps; performance mode (`perfmode`, GPU at 600): GoS 29.04, CoO 28.36, F1 42.37, Tekken 56.85. These runs were done back to back, starting at 56-65 C, with windows cut to 33-41 s: **indicative, not clean**.
- OpenGL (libmali g24p0, same conditions): GoS 22.45, CoO 16.90, F1 47.72 fps (47 s windows, with guard stops).
- Kernel: 0 error lines and no `Failed to map memory on GPU` in 18 runs (over 25 minutes of play, 270-540 MB available). 0 audio gaps attributable to audio (the gaps match the guard stops); this is the sink monitor, **nobody listened to it**.
- Limits: 1 run per condition; automatic key presses, not human play; 6 of the 10 games in Table 1 are not on the Ultra and were not tested there.

### R36 Ultra, 10 minutes in a row, Ghost of Sparta (real combat only for the first ~4 min), Arm Vulkan except the last row (n = 1-2)
Cold start (42-45 C settled) unless noted. The 70/85 C minutes are counted from the first monitor sample.

| Configuration | fps 0-2 min / 2-5 min | T at 5 min (C) | kernel throttles |
|---|---|---|---|
| Previous default (CPU and DMC `performance`, GPU auto) | 29.3 / 29.2 | 82.7 (86.2 in another report: different time origin) | yes, at ~4.8 min (CPU to 1200 MHz) |
| CPU and GPU automatic (n = 2) | 28.9-29.5 / 28.1-29.3 | 76.5-79.2 | not within 10 min |
| CPU automatic + GPU fixed at 600 | 28.9 / 28.8 | 76.2 | no |
| Performance mode | 29.5 / 28.3 | 84.2 | yes, at 3.2 min (CPU down to 1008 MHz; our own guard at 88 C after 5.2 min) |
| PanVK, previous default | 29.6 / 29.3 | 85.8 | yes, at ~3 min (CPU 1200, GPU 400; our own guard at 88 C after 6.9 min) |

FACT: below ~85 C the kernel limited nothing; the slow frames match heavy scene stretches and the initial load, not heat, except in the runs where the kernel did throttle. Limits: n = 1-2, the combat lasts about 4 minutes and the scene then stands still, and fps dips mix scene and heat. Other cases or ventilation were not tested.

### R36S, 60 s of play, Arm Vulkan, governors as the console had them (CPU `ondemand`, GPU and DMC `simple_ondemand`) (n = 1-2)
The R36S used has a fan that stock units lack. Keys were mapped by hand in `controls.ini` for this test. A complete Ghost of Sparta ISO was uploaded for the test and removed afterwards (the user's copy was cut short: 1,011,613,696 of 1,787,363,328 bytes).

| Scene | fps | p95 ms | stalls >50 ms per minute |
|---|---|---|---|
| Ghost of Sparta from scratch, cutscene (62 s) | 29.96 | 33.9 | 0 |
| Ghost of Sparta from scratch, first fight (63.5 s, start at 71 C) | 29.26 | 39.3 | 7.6 |
| Ghost of Sparta, saved state, complete ISO (2 runs) | 29.23 and 28.89 | 40.0 and 40.4 | 15.0 and 11.3 |
| Chains of Olympus, automatic (2 runs) | 29.36 and 29.06 | 43.4 and 43.7 | 21.9 and 15.9 |
| Chains of Olympus, GPU fixed at 600 | 31.43 | 42.4 | 12.9 |
| Chains of Olympus, CPU/GPU/DMC `performance` | 31.62 | 41.3 | 15.0 |

- Most of the ">50 ms stalls" in Chains of Olympus are an uneven 33/50 ms rhythm (one extra screen refresh), not real stalls. Stalls over 66 ms: 4-5 per minute, **always in the first ~13 s** after loading the state, and also with everything fixed; a run with an orderly PPSSPP exit (shader cache) gave 2-6 per minute (one run per case, inconclusive).
- On this console the automatic GPU governor cost about 2 fps in Chains of Olympus (29.1-29.4 against 31.4 with the GPU fixed); on the Ultra fixing the GPU gained nothing in Ghost of Sparta. Do not extrapolate from one console to the other.
- Ghost of Sparta has a fixed 0.6-0.8 s stall and an audio cut about 9-12 s after loading the combat state on both consoles, with the cut-short or the complete ISO (ASSUMPTION: an event of the game itself).
- Limits: 1-2 runs per case, only two games measured with real key presses, 60 s, the audio was not listened to.
