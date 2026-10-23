# Zenith <TAG>

Unofficial fork of AURKNIX/ROCKNIX for **RK3326** handhelds (tested on the **R36 Ultra** only).
What changed and the measurements: [CHANGELOG.md](https://github.com/<OWNER>/<REPO>/blob/main/CHANGELOG.md).

## Downloads

| File | What it is |
|------|------------|
| `AURKNIX-RK3326.aarch64-<DATE>.img.gz` | Full image for a new SD card |
| `AURKNIX-RK3326.aarch64-<DATE>.tar` | Update package for an installed system |
| `*.sha256` | SHA-256 checksums: verify them before writing anything |

## Installation
* Download the image and check its `.sha256`.
* Decompress the image and write it to an SD card with an imaging tool such as Balena Etcher.
  Check the target disk first: everything on it is erased.
* Other RK3326 devices: choose the device tree with `/flash/dtbselect` or `DtbselectWin64.exe` on the boot partition.

## Upgrading
* Copy the `.tar` to the device update share over the network and reboot; the update starts automatically.

## Known limitations
* Only the R36 Ultra has been tested. Other devices may not boot with the fast-boot changes; if the early boot mode
  does not reach EmulationStation in time it disables itself.
* A fixed 560 MHz GPU clock freezes the console and is not offered.

## Licenses
Zenith is made of many open-source components, each under its own license; some are for non-commercial use only.
See [LICENSE.md](https://github.com/<OWNER>/<REPO>/blob/main/LICENSE.md),
[CREDITS.md](https://github.com/<OWNER>/<REPO>/blob/main/CREDITS.md) and the `licenses/` folder.
