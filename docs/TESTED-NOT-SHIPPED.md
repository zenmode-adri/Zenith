# Tested and not shipped

Newer emulator builds, PPSSPP versions and GPU clocks were benchmarked on the R36 Ultra, and a change that did not help
was not shipped. Everything below was measured on the device.

## Newer emulator cores (ROCKNIX `next` branch)

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

## PPSSPP

Versions 1.12.3 and 1.20 were compared on three games, and 12 settings were tried one at a time on a set of ten games run
from save states.

| Test | Result |
|---|---|
| Best version | No winner: 1.20 wins in some games, 1.12.3 in others |
| Profile-guided optimisation | **+2.5 %**, not the hoped 15 % |
| `SkipBufferEffects` | Up to **+77 %** in God of War, but it breaks the picture ⚠️ |
| `FrameSkip = 3` (stock) | Stays: faster in heavy games |

## GPU clock and CPU turbo

| Setting | Result |
|---|---|
| GPU fixed at 520 or 560 MHz | ⚠️ Freezes the console within a second or two, whatever the voltage |
| GPU 200 / 300 / 400 / 600 MHz | ✅ Stable (frequency table 200/300/400/480/600 MHz) |
| CPU 1416 MHz at 1350 mV | ✅ Stable on four cores |
| CPU 1512 MHz on four cores | ⚠️ Rebooted the console, not used by default |

## Panfrost, uncapped

The five N64 games of the README's "At real speed" table on the R36S, with the same settings but uncapped (fast-forward,
no vsync, no audio sync), to see how much headroom each setting leaves (fps):

| Game (N64) | libmali auto | libmali RAM fixed | libmali GPU + RAM fixed | Panfrost auto | Panfrost GPU fixed | Panfrost RAM fixed | Panfrost GPU + RAM fixed |
|---|---:|---:|---:|---:|---:|---:|---:|
| Mario Tennis | 20.9 | 26.8 | 29.4 | 15.7 | 25.9 | 28.6 | **29.3** |
| Wave Race 64 | 40.8 | 45.0 | 47.4 | 47.5 | 59.9 | 65.0 | **65.7** |
| Mario Kart 64 (ParaLLEl) | 25.5 | 30.4 | 33.2 | 26.8 | 32.0 | 31.0 | **37.5** |
| GoldenEye 007 | 41.4 | 35.5 | 37.0 | 38.8 | 39.3 | **43.1** | 42.8 |
| Zelda: Ocarina of Time | 21.1 | 24.7 | 24.7 | 24.7 | 24.6 | **28.6** | **28.6** |
