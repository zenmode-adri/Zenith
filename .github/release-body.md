# Zenith v0.2.1 (beta)

Unofficial fork of AURKNIX/ROCKNIX for **RK3326** handhelds, developed and measured on the **R36 Ultra** (also tested on the
**R36S**). A small update on v0.2.0.
Everything that changed, with the measurements: [CHANGELOG.md](https://github.com/zenmode-adri/Zenith/blob/main/CHANGELOG.md).

> [!WARNING]
> **Beta.** Tested on the **R36 Ultra** and the **R36S**. I am one person, so bugs can slip through: if you find
> one, please [open an issue](https://github.com/zenmode-adri/Zenith/issues). Other RK3326 models use the same image but have not been tried.

**R36 Tuner Next:** the image includes a new version that is not released yet (the public repository is still at v2.0):
a rewritten interface with live graphs, one voltage-curve editor per component, a stability test and a boot safety net for
voltage changes. Screenshots in the [README](https://github.com/zenmode-adri/Zenith#r36-tuner-next).

## What's new since v0.2.0
- **Appearance options** (selector style, title alignment, list text case) apply as soon as you pick one, without
  restarting the front-end.
- **R36 Tuner Next** sits with the performance rows in `Battery & performance` and, with Turbo on, its CPU limit can
  reach the Turbo steps (1416 and 1512 MHz on the R36 Ultra).

For everything in v0.2.0 (zfront, patched Panfrost, performance mode, R36 Tuner Next, phone file transfer) see its
[release](https://github.com/zenmode-adri/Zenith/releases/tag/20261111) and the changelog.

## Downloads

| File | What it is |
|------|------------|
| `AURKNIX-RK3326.aarch64-20261112.img.gz` | Full image for a new SD card |
| `AURKNIX-RK3326.aarch64-20261112.tar` | Update package, used by the built-in updater |
| `*.sha256` | SHA-256 checksums: verify them before writing anything |

## Installation
* **Which file?** New install: download the `.img.gz`. Already running Zenith: update from the console with the built-in updater; the `.tar` is only used by the updater, you don't need to download it.
* Download the image and check its `.sha256`.
* Decompress it and write it to an SD card (Rufus on Windows, Balena Etcher, Raspberry Pi Imager, `dd`).
  Check the target disk first: everything on it is erased.
* RK3326 consoles: choose your device tree with **Zenith DTB Selector** on the boot partition
  (see [docs/DTB-SELECTOR.md](https://github.com/zenmode-adri/Zenith/blob/main/docs/DTB-SELECTOR.md)).
* **Coming from v0.2.0:** update from the console (`System > Updates & downloads > Software updates`). The updater is meant
  to offer this release.

## Default settings
* Wi-Fi, SSH, Samba and the phone file transfer are **off**: enable them from the Network menu.
* The root password is the factory one, `rocknix`. Change it before turning SSH or Samba on.

## Known issues
* The cover scraper (ScreenScraper) does not work in this image: no developer credentials are built in.
* Performance mode: heat and battery use in long sessions are not measured.

## Licenses
Zenith is made of many open-source components, each under its own license; some are for non-commercial use only.
See [LICENSE.md](https://github.com/zenmode-adri/Zenith/blob/main/LICENSE.md),
[CREDITS.md](https://github.com/zenmode-adri/Zenith/blob/main/CREDITS.md) and the `licenses/` folder.
