extlinux.conf is a file which has a final word when describing how to boot your device.

Usually u-boot runs boot.scr which checks some hardware stuff (ADC voltage, some GPIO levels),
chooses a dtb based on collected data and passes that to extlinux.

Thay may fail if you have a rare device or faulty hardware or something else unexpected.
In this case you can configure extlinux.conf manually.
Main things to edit:
 * APPEND -- kernel cmdline. If you have UART soldered you may want to remove 'quiet' option.
   Also this parameter can modify kernel behaviour (e.g. disable or configure drivers)
 * FDT -- a path to .dtb file to use. Please note that uboot's extlinux has a poor parser,
   and even a space after dtb file path breaks the boot.
 * FDTOVERLAYS -- dtb overlays to apply. This line can point to non-existing files, that's ok.
   Overlays may be used to apply configuration specific to your device
   (e.g. joystic axis inversion, display init sequence, etc.)

Zenith: boot messages and debugging
 * By default the kernel command line has "loglevel=1 vt.global_cursor_default=0", so kernel messages
   (for example harmless driver errors) are NOT drawn on the screen during boot. Nothing is lost:
   run "dmesg" or "journalctl -k" over SSH, or read the boot logs saved in /flash/bootlog.
 * To see everything on screen (and on the serial console), copy extlinux.conf.debug over
   extlinux.conf, or just remove "loglevel=1" from the APPEND line. Re-add your FDT line if you use one.
