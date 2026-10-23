# Zenith

**Zenith** is an **unofficial** fork of [AURKNIX](https://github.com/AveyondFly/distribution_rocknix), which is a fork
of [ROCKNIX](https://github.com/ROCKNIX), for **RK3326** handheld consoles. It adds a faster boot, faster and more
reliable suspend, its own light/dark EmulationStation theme and a cleaner Tools menu.

- Maintained by [zenmode-adri](https://github.com/zenmode-adri). See [`CREDITS.md`](CREDITS.md) for the people and
  projects this work stands on, and [`CHANGELOG.md`](CHANGELOG.md) for what changed and the measurements.
- **Tested only on the R36 Ultra.** Other RK3326 handhelds use the same image and device-tree selector but have not
  been checked with these changes. Targets other than RK3326 are inherited from upstream and were not built or tested.
- Zenith is not endorsed by, or affiliated with, ROCKNIX, AURKNIX or their authors. Please do not report Zenith problems
  to them.

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
