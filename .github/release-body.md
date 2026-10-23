# Zenith v0.1.0

Unofficial fork of AURKNIX/ROCKNIX for **RK3326** handhelds (tested on the **R36 Ultra** only).
What changed and the measurements: [CHANGELOG.md](https://github.com/zenmode-adri/Zenith/blob/main/CHANGELOG.md).

> [!WARNING]
> Pre-release. The files were built and inspected, but the first boot of this exact clean image from a freshly written
> SD card has not been confirmed yet. Only the R36 Ultra has been tested.

## Downloads

| File | What it is |
|------|------------|
| `AURKNIX-RK3326.aarch64-20261023.img.gz` | Full image for a new SD card |
| `*.sha256` | SHA-256 checksum: verify it before writing anything |

## Installation
* Download the image and check its `.sha256`.
* Decompress the image and write it to an SD card with an imaging tool such as Balena Etcher.
  Check the target disk first: everything on it is erased.
* Other RK3326 devices: choose the device tree with `/flash/dtbselect` or `DtbselectWin64.exe` on the boot partition.

## Default settings
* Wi-Fi, SSH and Samba are **off**: enable them from the network settings of the menu.
* If you turn SSH on, the root password is the factory one, `rocknix`. Change it, and turn SSH off when you do not need it.
* `systemd.debug_shell=ttyS2` gives a root shell on the serial port; it needs physical access to the board.

## Known limitations
* Only the R36 Ultra has been tested. Other devices may not boot with the fast-boot changes; if the early boot mode
  does not reach EmulationStation in time it disables itself.
* A fixed 560 MHz GPU clock freezes the console and is not offered.

## Licenses
Zenith is made of many open-source components, each under its own license; some are for non-commercial use only.
See [LICENSE.md](https://github.com/zenmode-adri/Zenith/blob/main/LICENSE.md),
[CREDITS.md](https://github.com/zenmode-adri/Zenith/blob/main/CREDITS.md) and the `licenses/` folder.
