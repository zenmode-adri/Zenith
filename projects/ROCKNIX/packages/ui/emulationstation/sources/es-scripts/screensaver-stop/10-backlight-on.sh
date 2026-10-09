#!/bin/sh
# Zenith: ES screensaver-stop event: turn the backlight back on (any button)
for b in /sys/class/backlight/*/bl_power; do echo 0 > "$b" 2>/dev/null; done
