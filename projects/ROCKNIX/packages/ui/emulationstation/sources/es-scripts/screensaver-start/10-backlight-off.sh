#!/bin/sh
# Zenith: ES screensaver-start event: turn the backlight off (a black frame still burns the LCD backlight)
# "dim" (ScreenSaverBehavior, first argument) only dims the picture: the light stays on, otherwise dim would be black.
[ "$1" = "dim" ] && exit 0
for b in /sys/class/backlight/*/bl_power; do echo 4 > "$b" 2>/dev/null; done
