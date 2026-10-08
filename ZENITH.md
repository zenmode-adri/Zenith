# Zenith - notes

## Boot messages and debugging
Zenith hides kernel messages on the screen during boot (kernel command line `loglevel=1`,
`vt.global_cursor_default=0`). They are still recorded:

- `dmesg` / `journalctl -k` over SSH show everything.
- `/flash/bootlog/` (the FAT partition, readable from Windows) keeps a log of each boot.
- To see everything live on screen and on the serial console, copy `extlinux/extlinux.conf.debug`
  over `extlinux/extlinux.conf` on the boot partition, or remove `loglevel=1` from the `APPEND` line.
  If your device needs a fixed device tree, add your `FDT /your-device.dtb` line back.

See `CREDITS.md` for credits and license notices.
