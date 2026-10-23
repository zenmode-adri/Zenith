# Zenith DTB Selector

Small cross-platform tool (Windows, Linux, macOS) that picks the right device tree (DTB) for your RK3326 handheld
and rewrites `extlinux/extlinux.conf` on the SD card of a Zenith image. Interface in 中文, English and Español.

* Finds the SD card by itself (a FAT volume that has `KERNEL` and `extlinux/extlinux.conf`); a path can also be typed.
* 86 models, grouped by brand. The R36S clone families keep their exact variants ("type 3 panel 2", "Sauce panel 4", ...).
* Shows the exact change (word-level diff) before writing, and saves a timestamped backup in `extlinux/backups/`
  (the 30 newest are kept). Any backup can be restored from the tool.
* Edits the card's current `extlinux.conf` instead of regenerating it from a template, so your own options survive.
  It writes LF line endings and removes trailing whitespace (u-boot refuses to boot if anything follows `.dtb`).
* Your own DTB: "Import my own DTB…" copies a `.dtb` from the PC to the card root (header and size are validated,
  names are reduced to a safe base name, an existing file is never replaced without asking) and selects it.
* Advanced: show boot messages, debug UART per model, any `.dtb` present on the card. It never adds the `ssh` flag
  (SSH stays opt-in from the system menu); an `ssh` flag that is already in the file is left as it is.
* No network access: it only listens on `127.0.0.1` with a random per-run token and exits when its window closes.

## Use
Windows: double-click `ZenithDtbSelector.exe` in the root of the card (or from anywhere). Linux/macOS: run the binary.
Command line: `--list` lists the models; `--boot E:\ --apply <id> [--show-boot]` applies one without the window.

## Build
`./build.sh` (needs Go >= 1.22). The Windows exe goes to `../prebuilt/ZenithDtbSelector-win64.exe`, which IS committed
(rebuild and commit it whenever the sources change) so every image build ships it: `package.mk` installs it and
`mkimage` copies it to the FAT root as `ZenithDtbSelector.exe`. Linux/macOS binaries land in `dist/`.
The catalog (`web/catalog.json`) is generated from the legacy `../dtb_selector.py` by `tools/gen_catalog.py`;
edit that script to add models.
