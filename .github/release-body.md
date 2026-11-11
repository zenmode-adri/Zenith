# Zenith v0.2.0

Unofficial fork of AURKNIX/ROCKNIX for **RK3326** handhelds, developed and measured on the **R36 Ultra**.
Everything that changed, with the measurements: [CHANGELOG.md](https://github.com/zenmode-adri/Zenith/blob/main/CHANGELOG.md).

> [!WARNING]
> **Tested only on the R36 Ultra.** Other RK3326 models use the same image but have not been tried with this release:
> treat them as beta.

## Highlights
- **zfront, Zenith's own front-end, is now the default**: first screen in 1.0 s instead of 4.1 s and about a quarter of
  EmulationStation's memory. Two new themes, **Zenith Minimal** (light) and **Zenith Next** (dark); XMB, Art Book,
  Iconic and TechDweeb remade on the same grid. EmulationStation stays installed (`Tools > Switch frontend`).
- **Patched Panfrost** (open GPU driver) in the image: +38.9 % over stock Panfrost and +4.4 % / +6.5 % over libmali
  (R36S / R36 Ultra, 20 games, free-running test). libmali stays the default.
- **Performance mode** (off by default): GPU and RAM at full speed while a game runs (Mario Tennis 19.3 -> 25.0 fps with
  libmali on the R36S). **RAM at 786 MHz** on the models that have that step.
- **R36 Tuner Next** built in, with undervolt and a fix for the GPU freezes around 480 MHz.
- **Send files from your phone** through a web page with a QR code (off by default).
- **Updates from Zenith's own releases**: from this version on, the built-in updater offers new releases.
- Faster volume keys, controller mapping that can always be cancelled, Spanish everywhere, and many fixes.

## Downloads

| File | What it is |
|------|------------|
| `AURKNIX-RK3326.aarch64-20261111.img.gz` | Full image for a new SD card |
| `AURKNIX-RK3326.aarch64-20261111.tar` | Update package, used by the built-in updater of future releases |
| `*.sha256` | SHA-256 checksums: verify them before writing anything |

## Installation
* Download the image and check its `.sha256`.
* Decompress it and write it to an SD card (Balena Etcher, Raspberry Pi Imager, `dd`).
  Check the target disk first: everything on it is erased.
* Other RK3326 devices: choose the device tree with **Zenith DTB Selector** on the boot partition
  (see [docs/DTB-SELECTOR.md](https://github.com/zenmode-adri/Zenith/blob/main/docs/DTB-SELECTOR.md)).
* **Coming from v0.1.0:** write the image again (back up your saves first). The update menu of v0.1.0 looked at the
  original AURKNIX repository and does not see this release.

## Default settings
* Wi-Fi, SSH, Samba and the phone file transfer are **off**: enable them from the Network menu.
* The root password is the factory one, `rocknix`. Change it before turning SSH or Samba on.

## Known issues
* Only the R36 Ultra has been tested; the R36S only on an older development image.
* The cover scraper (ScreenScraper) does not work in this image: no developer credentials are built in.
* Performance mode: heat and battery use in long sessions are not measured.

## Licenses
Zenith is made of many open-source components, each under its own license; some are for non-commercial use only.
See [LICENSE.md](https://github.com/zenmode-adri/Zenith/blob/main/LICENSE.md),
[CREDITS.md](https://github.com/zenmode-adri/Zenith/blob/main/CREDITS.md) and the `licenses/` folder.
