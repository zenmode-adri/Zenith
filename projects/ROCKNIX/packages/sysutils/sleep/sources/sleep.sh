#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2022-24 JELOS (https://github.com/JustEnoughLinuxOS)
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)

. /etc/profile

if [ -e "/sys/firmware/devicetree/base/model" ]; then
  QUIRK_DEVICE=$(tr -d '\0' </sys/firmware/devicetree/base/model 2>/dev/null)
fi
QUIRK_DEVICE="$(echo ${QUIRK_DEVICE} | sed -e "s#[/]#-#g")"

EVENTLOG="/var/log/sleep.log"

headphones() {
  if [ "${DEVICE_FAKE_JACKSENSE}" == "true" ]; then
    log $0 "Headphone sense: ${1}"
    systemctl ${1} headphones >${EVENTLOG} 2>&1
  fi
}

inputsense() {
  log $0 "Input sense: ${1}"
  systemctl ${1} input >${EVENTLOG} 2>&1
}

powerstate() {
  log $0 "Power state: ${1}"
  systemctl ${1} powerstate >${EVENTLOG} 2>&1
}

bluetooth() {
  if [ "$(get_setting controllers.bluetooth.enabled)" == "1" ]; then
    log $0 "Bluetooth: ${1}"
    systemctl ${1} bluetooth >${EVENTLOG} 2>&1
  fi
}

modules() {
  log $0 "Modules: ${1}"
  case ${1} in
    stop)
      if [ -e "/usr/config/modules.bad" ]; then
        for module in $(cat /usr/config/modules.bad); do
          EXISTS=$(lsmod | grep ${module})
          if [ $? = 0 ]; then
            echo ${module} >>/tmp/modules.load
            modprobe -r ${module} >${EVENTLOG} 2>&1
          fi
        done
      fi
      ;;
    start)
      if [ -e "/tmp/modules.load" ]; then
        for module in $(cat /tmp/modules.load); do
          MODCNT=0
          MODATTEMPTS=10
          while true; do
            if (( "${MODCNT}" < "${MODATTEMPTS}" )); then
              modprobe ${module%% *} >${EVENTLOG} 2>&1
              if [ $? = 0 ]; then
                break
              fi
            else
              break
            fi
            MODCNT=$((${MODCNT} + 1))
            sleep .5
          done
        done
        rm -f /tmp/modules.load
      fi
      ;;
  esac
}

quirks() {
  for QUIRK in /usr/lib/autostart/quirks/platforms/"${HW_DEVICE}"/sleep.d/${1}/*                /usr/lib/autostart/quirks/devices/"${QUIRK_DEVICE}"/sleep.d/${1}/*; do
    [ -x "${QUIRK}" ] || continue
    if [ -n "${QUIRK_ONLY}" ]; then
      case "${QUIRK##*/}" in ${QUIRK_ONLY}) ;; *) continue ;; esac
    fi
    if [ -n "${QUIRK_SKIP}" ]; then
      case "${QUIRK##*/}" in ${QUIRK_SKIP}) continue ;; esac
    fi
    "${QUIRK}" >${EVENTLOG} 2>&1
  done
}

get_paneladj_value() {
    local value

    value="$(get_setting "display.${1}")"

    if ! [[ "${value}" =~ ^[0-9]+$ ]]; then
      value=50
    fi

    if (( value < 5 )); then
      value=5
    elif (( value > 100 )); then
      value=100
    fi

    value=$(( ((value + 2) / 5) * 5 ))

    if (( value < 5 )); then
      value=5
    elif (( value > 100 )); then
      value=100
    fi

    echo "${value}"
}

restore_panel_adjustments() {
    command -v paneladj >/dev/null 2>&1 || return 0

    local property
    local value

    for property in brightness contrast saturation hue; do
      value="$(get_paneladj_value "${property}")"
      log $0 "Restoring ${property} panel adjustment to ${value}%."
      paneladj "${property}" "${value}" >/dev/null 2>&1 &
    done
    wait
}

# Pantalla: se apaga la retroiluminacion un poco despues del aviso (el aviso se ve >= 1 s) y se vuelve a poner
# al despertar en cuanto el panel tiene sus ajustes de color. El valor se guarda en /run (no en system.cfg).
SLEEP_NOTICE_MIN_MS=1300
now_ms() { local u; read -r u _ < /proc/uptime; u=${u/./}; echo $((10#${u} * 10)); }
backlight_off() {
  local f
  for f in /sys/class/backlight/*/brightness; do
    [ -w "${f}" ] || continue
    cat "${f}" > "/run/.sleep-bl.$(basename "$(dirname "${f}")")"
    echo 0 > "${f}"
  done
}
backlight_on() {
  local f
  for f in /sys/class/backlight/*/brightness; do
    [ -r "/run/.sleep-bl.$(basename "$(dirname "${f}")")" ] || continue
    cat "/run/.sleep-bl.$(basename "$(dirname "${f}")")" > "${f}" 2>/dev/null
    rm -f "/run/.sleep-bl.$(basename "$(dirname "${f}")")"
  done
}

# Aviso en pantalla al empezar a suspender (hasta que se apaga la pantalla pasan 1-3 s). EmulationStation lo muestra
# con su API local (mako no se dibuja encima de ES); en un juego se usa mako-notify. Por defecto en ingles.
suspend_notice() {
  [ -S /run/0-runtime-dir/wayland-1 ] || return 0
  case "$(get_setting system.language)" in
    es_*) MSG="Suspendiendo..." ;;
    fr_*) MSG="Mise en veille..." ;;
    de_*) MSG="Standby..." ;;
    it_*) MSG="Sospensione..." ;;
    pt_*) MSG="Suspendendo..." ;;
    *)    MSG="Suspending..." ;;
  esac
  export XDG_RUNTIME_DIR=/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1
  export SWAYSOCK=/run/0-runtime-dir/sway-ipc.0.sock DBUS_SESSION_BUS_ADDRESS=unix:path=/run/0-runtime-dir/bus
  if [ "$(swaymsg -t get_tree 2>/dev/null | jq -r '.. | select(.focused?) | .app_id' 2>/dev/null)" = "emulationstation" ]; then
    curl -s -m 2 -X POST -H 'Content-Type: text/plain' -d "${MSG}" 'http://127.0.0.1:1234/notify?duration=1500' >/dev/null 2>&1
  else
    /usr/bin/mako-notify "${MSG}" >/dev/null 2>&1
  fi
}

# Quita el aviso al volver de la suspension (el reloj de ES se para al suspender y el aviso reaparecia).
clear_suspend_notice() {
  curl -s -m 2 -X POST -d x http://127.0.0.1:1234/notify/clear >/dev/null 2>&1
  XDG_RUNTIME_DIR=/run/0-runtime-dir DBUS_SESSION_BUS_ADDRESS=unix:path=/run/0-runtime-dir/bus makoctl dismiss -a >/dev/null 2>&1
}

case $1 in
  pre)
    T0=$(now_ms)
    ( suspend_notice ) &

    # Carril WiFi (en paralelo con el resto): wifictl + descarga del driver RK915 (~2 s en serie)
    (
      if [ "$(get_setting wifi.enabled)" == "1" ]; then
        log $0 "Disabling WIFI."
        nohup wifictl disable >${EVENTLOG}.wifi 2>&1
      fi
      QUIRK_ONLY='*rk915*' quirks pre
    ) &
    WIFILANE=$!

    # Carril pantalla: tras el aviso (>= SLEEP_NOTICE_MIN_MS) se apaga la luz
    (
      d=$(( SLEEP_NOTICE_MIN_MS - ($(now_ms) - T0) ))
      [ "${d}" -gt 0 ] && sleep "$(printf '%d.%03d' $((d / 1000)) $((d % 1000)))"
      backlight_off
    ) &
    SCREENLANE=$!

    # Carril resto
    headphones stop
    inputsense stop
    bluetooth stop
    powerstate stop
    modules stop
    QUIRK_SKIP='*rk915*' quirks pre
    touch /run/.last_sleep_time

    wait ${WIFILANE} ${SCREENLANE}
    log $0 "pre listo en $(( $(now_ms) - T0 )) ms"
    ;;
  post)
    T0=$(now_ms)
    ( clear_suspend_notice ) &

    # Carril pantalla: primero los ajustes del panel (en paralelo) y luego la luz, para no encender con colores sin ajustar
    (
      restore_panel_adjustments
      backlight_on
      log $0 "pantalla lista en $(( $(now_ms) - T0 )) ms"
    ) &
    SCREENLANE=$!

    ledcontrol
    modules start
    powerstate start
    headphones start
    inputsense start
    bluetooth start

    if [ "$(get_setting wifi.enabled)" == "1" ]; then
      log $0 "Enabling WIFI."
      nohup wifictl enable >${EVENTLOG} 2>&1
    fi

    DEVICE_VOLUME=$(get_setting "audio.volume" 2>/dev/null)
    log $0 "Restoring volume to ${DEVICE_VOLUME}%."
    amixer -c 0 -M set "${DEVICE_AUDIO_MIXER}" ${DEVICE_VOLUME}% >${EVENTLOG} 2>&1

    BRIGHTNESS_2=$(get_setting system.brightness2)
    if [ -n "${BRIGHTNESS_2}" ]; then
        log $0 "Restoring brightness for display 2 to ${BRIGHTNESS_2}."
        brightness set 2 ${BRIGHTNESS_2} >${EVENTLOG} 2>&1
    fi

    quirks post

    wait ${SCREENLANE}
    BRIGHTNESS=$(get_setting system.brightness)
    log $0 "Restoring brightness}."
    brightness set ${BRIGHTNESS} >${EVENTLOG} 2>&1
    log $0 "post listo en $(( $(now_ms) - T0 )) ms"
    ;;
esac
