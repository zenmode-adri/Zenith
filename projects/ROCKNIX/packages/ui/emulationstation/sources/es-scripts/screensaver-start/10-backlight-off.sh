#!/bin/sh
# Zenith: ES screensaver-start event: turn the backlight off (a black frame still burns the LCD backlight)
# "dim" (ScreenSaverBehavior, first argument) only dims the picture: the light stays on, otherwise dim would be black.
[ "$1" = "dim" ] && exit 0
# "slideshow" / "random video" show pictures (zfront's ssmedia): the light stays on too. When zfront finds nothing to show it falls back to black and writes bl_power=4 itself.
case "$1" in slideshow|"random video") exit 0 ;; esac
for b in /sys/class/backlight/*/bl_power; do echo 4 > "$b" 2>/dev/null; done
