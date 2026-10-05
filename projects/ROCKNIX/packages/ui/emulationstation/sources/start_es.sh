#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2024 ROCKNIX (https://github.com/ROCKNIX)

### setup is the same
. $(dirname $0)/es_settings

### Espera (max 10 s) a que Sway cree el socket Wayland: evita la carrera sway/ES
### ("Error initializing SDL! wayland not available" -> reinicio de 2 s)
i=0
while [ ! -S /run/0-runtime-dir/wayland-1 ] && [ "${i}" -lt 200 ]
do
  sleep 0.05
  i=$((i+1))
done

emulationstation --log-path /var/log
