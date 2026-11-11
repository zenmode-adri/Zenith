# R36 Tuner Next on Zenith

SDL2 tuner by zenmode-adri. **Do not edit `sources/` here**: the source is https://github.com/zenmode-adri/r36-tuner-next
(`tools/release/sync_zenith.sh <this folder>` copies it). The same program runs on dArkOSRE; what differs is in `plat.c`.

Zenith specifics: runs as root; `/flash` stays read-only except inside `r36-dtb`; the factory device tree is never modified
(`/flash/tuned.dtb` is a copy, the `FDT` line of `extlinux.conf` picks it); governors go through `system.cpugovernor`,
`system.gpuperf` and `global.perfmode`, which `runemu.sh` reads around every game; clock limits come back at boot through
`/usr/lib/autostart/common/096-r36-tuner-limits`.

Safety net (`r36-dtb`, `r36-dtb-boot.service`, called from `autostart`): `pending` -> `booting` -> `trial` -> `ok`. It undoes a failed
boot and a crash during the stability test. A crash later, while playing, is not undone: `Undo everything`. A device tree that
hangs before init cannot undo itself: edit `extlinux.conf` from a PC (steps in the program).
