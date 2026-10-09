#!/bin/sh
# Zenith: ES screensaver-start event: turn the backlight off (a black frame still burns the LCD backlight)
for b in /sys/class/backlight/*/bl_power; do echo 4 > "$b" 2>/dev/null; done
