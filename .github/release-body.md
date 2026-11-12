# Zenith v0.2.1 (beta)

Unofficial fork of AURKNIX/ROCKNIX for **RK3326** handhelds, developed and measured on the **R36 Ultra** (also tested on the
**R36S**). A small update on v0.2.0.
Everything that changed, with the measurements: [CHANGELOG.md](https://github.com/zenmode-adri/Zenith/blob/main/CHANGELOG.md).

> [!WARNING]
> **Beta.** Tested on the **R36 Ultra** and the **R36S**, the two consoles we have. Other RK3326 models use the same
> image but have not been tried: treat them as beta too.

## What's new since v0.2.0
- **Appearance options apply at once:** selector style, title alignment and list text case change the screen when you
  pick a value and you stay in the menu (before, the front-end restarted).
- **R36 Tuner Next** sits with the performance rows in `Battery & performance`, starts directly (no terminal window in
  between) and, with Turbo on, its CPU limit can reach the Turbo steps (1416 and 1512 MHz on the R36 Ultra).
- Text fixes: letters that hang below the line are no longer cut (a "g" in the Iconic theme); "microSD" keeps its spelling.
- Checked on screen only for the Appearance change (R36 Ultra, test build). The R36S ran the previous build, not this
  exact image. The Tuner changes have automated tests only.

For everything in v0.2.0 (zfront, patched Panfrost, performance mode, R36 Tuner Next, phone file transfer) see its
[release](https://github.com/zenmode-adri/Zenith/releases/tag/20261111) and the changelog.

## Downloads

| File | What it is |
|------|------------|
| `AURKNIX-RK3326.aarch64-20261112.img.gz` | Full image for a new SD card |
| `AURKNIX-RK3326.aarch64-20261112.tar` | Update package, used by the built-in updater |
| `*.sha256` | SHA-256 checksums: verify them before writing anything |

## Installation
* Download the image and check its `.sha256`.
* Decompress it and write it to an SD card (Balena Etcher, Raspberry Pi Imager, `dd`).
  Check the target disk first: everything on it is erased.
* Other RK3326 devices: choose the device tree with **Zenith DTB Selector** on the boot partition
  (see [docs/DTB-SELECTOR.md](https://github.com/zenmode-adri/Zenith/blob/main/docs/DTB-SELECTOR.md)).
* **Coming from v0.2.0:** the built-in updater is meant to offer this release (not tested end to end from a v0.2.0 SD).
  Coming from v0.1.0, write the image (back up your saves first): v0.1.0's update menu does not see releases of this repository.

## Default settings
* Wi-Fi, SSH, Samba and the phone file transfer are **off**: enable them from the Network menu.
* The root password is the factory one, `rocknix`. Change it before turning SSH or Samba on.

## Known issues
* Beta: only the R36 Ultra and the R36S have been tested.
* The cover scraper (ScreenScraper) does not work in this image: no developer credentials are built in.
* Performance mode: heat and battery use in long sessions are not measured.

## Licenses
Zenith is made of many open-source components, each under its own license; some are for non-commercial use only.
See [LICENSE.md](https://github.com/zenmode-adri/Zenith/blob/main/LICENSE.md),
[CREDITS.md](https://github.com/zenmode-adri/Zenith/blob/main/CREDITS.md) and the `licenses/` folder.
