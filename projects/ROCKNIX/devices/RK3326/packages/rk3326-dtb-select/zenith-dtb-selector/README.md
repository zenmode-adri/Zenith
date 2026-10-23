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

## Test mode ("Not sure which model you have?")
For clone consoles whose DTB cannot be deduced (their boot ADC value is the same). Enable it in the tool, put the card in
the console and switch it on: a test card is shown and **one screen setting is tried at a time**. **Hold any button for
about 1 s** (pad, or VOL) when the picture looks right and that DTB stays; with no button the next candidate is written
to `extlinux.conf` and the console reboots. If every candidate fails, the original `extlinux.conf` is restored.
The POWER button is deliberately not used (it can trigger suspend). How long one try takes is an estimate, not measured.
* The tool only offers it when the card carries `zenith-features.txt` with `dtb-probe=1` (written by `mkimage`): an older
  image would keep booting candidate 1 forever because nothing there advances or confirms. `--probe-force` bypasses it (tests).
* Candidates: one DTB per distinct **screen set** among the clone families (`web/probe-order.json`, `tools/probe_order.py`):
  panel recipe + reset GPIO (with its bank) + backlight + panel supplies + DSI host, resolving phandles to node paths.
  The representative of each group is the one with the lowest charge current.
* Unplug the charger while testing (battery/charger values differ between DTBs).
* Console side: `autostart/sources/firstboot/dtb-probe.sh`, called by `autostart` right after it picks the legacy path
  (before waiting for multi-user/network). State lives in `/flash/dtbprobe/`; `/flash` is read-write only around writes.
  It only edits the `FDT` line, writes atomically, has an attempt cap, waits a bounded time for the pad and cancels itself
  if `extlinux.conf` was changed by hand. On exit it leaves `dtbprobe-last.txt` (result) and `dtbprobe-last.log`.
* CLI: `--probe-start [--probe-list a.dtb,b.dtb] [--probe-force]`, `--probe-stop`, `--probe-status` (with `--boot`).
* Tests: `autostart/tests/dtb-probe-test.sh` (script, simulated) and `tools/probe-integration-test.sh` (tool + script).
* **Not run on real hardware yet.** A DTB that hangs the kernel before userspace cannot advance by itself: restore the
  backup (the PC tool keeps `extlinux/backups/`, and `dtbprobe/extlinux.orig` holds the original).

## Build
`./build.sh` (needs Go >= 1.22). The Windows exe goes to `../prebuilt/ZenithDtbSelector-win64.exe`, which IS committed
(rebuild and commit it whenever the sources change) so every image build ships it: `package.mk` installs it and
`mkimage` copies it to the FAT root as `ZenithDtbSelector.exe`. Linux/macOS binaries land in `dist/`.
The catalog (`web/catalog.json`) is generated from the legacy `../dtb_selector.py` by `tools/gen_catalog.py`;
edit that script to add models.
