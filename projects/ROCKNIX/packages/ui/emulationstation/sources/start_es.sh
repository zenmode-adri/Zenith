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

### Zenith: el frontend lo elige `system.frontend` en system.cfg: `zfront` (el de fabrica desde el 8-nov) o `es`; sin el ajuste, ES
### (instalaciones de antes: a nadie le cambia sin elegirlo). Solo se cambia a mano o con Tools > Switch frontend; una OTA no toca
### system.cfg. ES sigue instalado como respaldo: si zfront sale con error SIN haber dibujado el primer fotograma (no creo
### /tmp/emulationstation.ready), cuenta el fallo en /run/zfront.fails y sale con error para que systemd lo lance otra vez; solo con
### 3 fallos seguidos crea /run/zfront.failed y lanza ES en esta misma pasada, hasta el proximo arranque (la eleccion no cambia). zfront cumple el mismo contrato que ES (ES_WAIT_*, bl_power,
### /tmp/emulationstation.ready, API en 127.0.0.1:1234, app_id emulationstation) y acepta --log-path (stderr a /var/log/zfront.log).
### Argumentos por defecto de zfront (cambiables con zfront.args en system.cfg): ver cpp/docs/install.md. El tema sale de ThemeSet y la resolucion de la salida.
if [ "$(get_setting system.frontend)" = "zfront" ] && [ -x /usr/bin/zfront ] && [ ! -e /run/zfront.failed ]
then
  ZFRONT_ARGS="$(get_setting zfront.args)"
  [ -n "${ZFRONT_ARGS}" ] || ZFRONT_ARGS="--systems /storage/.config/emulationstation/es_systems.cfg --fullscreen --log-path /var/log"
  rm -f /tmp/emulationstation.ready   # una marca vieja de una pasada anterior falsearia la comprobacion
  /usr/bin/zfront ${ZFRONT_ARGS}
  ZF_RC=$?
  # 129/143 = SIGHUP/SIGTERM de systemd (parada o reinicio pedido): no es un fallo del frontend
  if [ ${ZF_RC} -ne 0 ] && [ ${ZF_RC} -ne 129 ] && [ ${ZF_RC} -ne 143 ] && [ ! -e /tmp/emulationstation.ready ]
  then
    ZF_FAILS=$(( $(cat /run/zfront.fails 2>/dev/null || echo 0) + 1 ))
    echo ${ZF_FAILS} >/run/zfront.fails
    if [ ${ZF_FAILS} -lt 3 ]
    then
      echo "$(date '+%F %T') zfront fallo al arrancar (rc=${ZF_RC}, fallo ${ZF_FAILS} de 3 seguidos); systemd lo vuelve a lanzar" >>/var/log/start_es.log
      exit ${ZF_RC}
    fi
    echo "$(date '+%F %T') zfront fallo al arrancar 3 veces seguidas (rc=${ZF_RC}); se usa EmulationStation hasta el proximo arranque" >>/var/log/start_es.log
    touch /run/zfront.failed
  else
    rm -f /run/zfront.fails   # llego a dibujar (o se paro a peticion): los fallos ya no son seguidos
    exit ${ZF_RC}   # 0 = salida normal: systemd (Restart=always) lo vuelve a lanzar, igual que con ES
  fi
fi

emulationstation --log-path /var/log
