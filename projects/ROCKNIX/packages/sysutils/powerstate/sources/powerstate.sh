#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2023 JELOS (https://github.com/JustEnoughLinuxOS)

###
### Normally this would be a udev rule, but some devices like the AyaNeo Air
### do not properly report the applied power state to udev, and we can't use
### inotifyd to watch the status in /sys.
###

. /etc/profile

BATCNT=0
unset CURRENT_MODE
unset AC_STATUS
ledcontrol $(get_setting led.color)

### On battery, rk817 says "Not charging" whenever the average current is not negative, so the status comes and goes
### with the load. With no charger online that is still the battery; if no supply reports online, the status is kept.
battery_status() {
  local status online found=0
  status="$(cat /sys/class/power_supply/[bB][aA][tT]*/status 2>/dev/null)"
  if [ "${status}" = "Not charging" ]; then
    for online in /sys/class/power_supply/*/online; do
      [ -e "${online}" ] || continue
      found=1
      [ "$(cat "${online}" 2>/dev/null)" = "1" ] && found=2 && break
    done
    [ "${found}" = "1" ] && status="Discharging"
  fi
  echo "${status}"
}

### $1 = the level for this power state when no game is running. With no game and nothing chosen in the settings only the first pass
### sets it: otherwise a status that comes and goes undoes a governor written by hand (R36 Tuner, a measurement).
set_gpu_level() {
  local chosen
  if [ -e "/tmp/.gpu_performance_level" ]; then
    gpu_performance_level "$(cat /tmp/.gpu_performance_level)"
    return
  fi
  chosen="$(get_setting system.gpuperf)"
  case "${chosen}" in
    ""|auto|default) [ -n "${CURRENT_MODE}" ] && return ;;
  esac
  gpu_performance_level "$1"
}

while true; do
  AC_STATUS="$(battery_status)"
  if [[ ! "${CURRENT_MODE}" =~ ${AC_STATUS} ]]; then
    case ${AC_STATUS} in
      Disch*)
        log $0 "Switching to battery mode."
        GPUPROFILE=$(get_setting system.gpuperf)
        set_gpu_level "${GPUPROFILE:-auto}"
        if [ "${DEVICE_LED_CHARGING}" = "true" ]; then
          ledcontrol discharging
        fi
      ;;
      *)
        log $0 "Switching to performance mode."
        # keeps the level runemu.sh set for the running game (gpuperf or the performance mode)
        set_gpu_level auto
        if [ "${DEVICE_LED_CHARGING}" = "true" ]; then
          ledcontrol charging
        fi
      ;;
    esac
    CURRENT_MODE="${AC_STATUS}"
  fi
  ### Until we have an overlay. :rofl:
  BATLEFT=$(battery_percent)
  if (( "${BATCNT}" >= "20" )) && [[ "${AC_STATUS}" =~ Disch ]]; then
    AUDIBLEALERT=$(get_setting system.battery.warning)
    AUDIBLEALERT_THRESHOLD=$(get_setting system.battery.warning_threshold)
    [[ -z $AUDIBLEALERT_THRESHOLD ]] && AUDIBLEALERT_THRESHOLD=25

    if [[ ${BATLEFT} -le ${AUDIBLEALERT_THRESHOLD} ]]; then
      if [ "${DEVICE_LED_CONTROL}" = "true" ] && [ ! "${DEVICE_BATTERY_LED_STATUS}" = "true" ]; then
        # Flash the RGB or power LED if available.
        led_flash
        BATCNT=0
      elif [ "${AUDIBLEALERT}" = "1" ]; then
        say "BATTERY AT ${BATLEFT}%"
        BATCNT=0
      fi
    fi
  elif (( "${BATLEFT}" > "97" )); then
    if [ "${DEVICE_LED_CHARGING}" = "true" ]; then
      # Reset the LED as if the battery was full.
      ledcontrol discharging
    fi
  fi
  BATCNT=$(( ${BATCNT} + 1 ))
  sleep 2
done
