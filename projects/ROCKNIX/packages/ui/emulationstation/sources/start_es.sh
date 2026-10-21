#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2024 ROCKNIX (https://github.com/ROCKNIX)

### setup is the same
. $(dirname $0)/es_settings

### Sway aun puede estar arrancando: ES (parche 0044) espera el socket Wayland por dentro, justo antes de abrir la ventana (max 10 s),
### asi su enlazado y su preambulo se solapan con el arranque de Sway. Evita la carrera sway/ES
### ("Error initializing SDL! wayland not available" -> reinicio de 2 s)
export ES_WAIT_WAYLAND_SOCKET=/run/0-runtime-dir/wayland-1

emulationstation --log-path /var/log
