#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2024 ROCKNIX (https://github.com/ROCKNIX)

### setup is the same
. $(dirname $0)/es_settings

### Sway aun puede estar arrancando: ES (parche 0044) espera el socket Wayland por dentro, justo antes de abrir la ventana (max 10 s),
### asi su enlazado y su preambulo se solapan con el arranque de Sway. Evita la carrera sway/ES
### ("Error initializing SDL! wayland not available" -> reinicio de 2 s)
export ES_WAIT_WAYLAND_SOCKET=/run/0-runtime-dir/wayland-1
### Las ROMs pueden seguir montandose (rocknix-automount) mientras ES arranca: ES (parche 0045) espera este fichero justo antes de escanear sistemas (max 15 s)
export ES_WAIT_ROMS_READY=/run/rocknix-automount.ready

### Si ES se reinicia (o se cierra) con el salvapantallas activo, el script screensaver-stop no llega a ejecutarse y el panel se queda apagado
### (screensaver-start pone bl_power=4): al arrancar ES la luz siempre se enciende
for b in /sys/class/backlight/*/bl_power; do echo 0 > "$b" 2>/dev/null; done

emulationstation --log-path /var/log
