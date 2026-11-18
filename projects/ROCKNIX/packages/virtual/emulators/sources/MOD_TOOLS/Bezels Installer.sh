#!/bin/bash

. /etc/profile
set -u

for cmd in unzip sed grep cp; do
  command -v "${cmd}" >/dev/null 2>&1 || { echo "Error: required command '${cmd}' not found." >&2; exit 1; }
done

ROM_DIR="/storage/roms"
TARGET_RES="1920x1080 1080x1920 1024x768 1280x720 720x1280 960x720 720x960 854x480 480x854 544x960 960x544 720x720 480x640 640x480 480x320 320x480"
RESOLUTION_RE='(1920x1080|1080x1920|1280x720|1024x768|960x720|720x960|720x1280|854x480|480x854|960x544|544x960|720x720|640x480|480x640|480x320|320x480)'
FORCE_INSTALL=1
RESOLUTION_ARG=""

normalize_resolution() {
  local res="$1"
  local width height

  [ -n "${res}" ] || return 1

  case " ${TARGET_RES} " in
    *" ${res} "*) ;;
    *) return 1 ;;
  esac

  IFS='x' read -r width height <<< "${res}"
  if (( width < height )); then
    res="${height}x${width}"
  fi

  echo "${res}"
}

detect_current_resolution() {
  local width height res state candidate

  if [ -r /sys/class/display/vinfo ]; then
    width="$(sed -n 's/^[[:space:]]*width:[[:space:]]*//p' /sys/class/display/vinfo | head -n1)"
    height="$(sed -n 's/^[[:space:]]*height:[[:space:]]*//p' /sys/class/display/vinfo | head -n1)"
    if [[ "${width}" =~ ^[0-9]+$ && "${height}" =~ ^[0-9]+$ ]] &&
       res="$(normalize_resolution "${width}x${height}")"; then
      echo "${res}"
      return
    fi
  fi

  for state in /sys/kernel/debug/dri/*/state; do
    [ -r "${state}" ] || continue
    while read -r candidate; do
      if res="$(normalize_resolution "${candidate}")"; then
        echo "${res}"
        return
      fi
    done < <(sed -n 's/.*name:\[\([0-9]\+x[0-9]\+\)p[0-9].*/\1/p' "${state}")
  done

  for candidate in $(grep -oE "${RESOLUTION_RE}" /sys/class/graphics/fb0/modes 2>/dev/null); do
    if res="$(normalize_resolution "${candidate}")"; then
      echo "${res}"
      return
    fi
  done
}

detect_resolution() {
  local detected_res

  detected_res="$1"

  if [ -z "${detected_res}" ]; then
    detected_res="$(detect_current_resolution)"
  else
    detected_res="$(normalize_resolution "${detected_res}")"
  fi

  echo "${detected_res}"
}

ensure_bezel_archives() {
  mkdir -p /storage/bezels

  if ! compgen -G "/storage/bezels/bezels_*.zip" >/dev/null; then
    cp /usr/share/misc/bezels_*.zip /storage/bezels/ 2>/dev/null
  fi
}

for arg in "$@"; do
  case "${arg}" in
    --skip-existing)
      FORCE_INSTALL=0
      ;;
    --force)
      FORCE_INSTALL=1
      ;;
    *)
      RESOLUTION_ARG="${arg}"
      ;;
  esac
done

if [ "${FORCE_INSTALL}" -eq 0 ] && { [ -e "/roms/bezels/gba" ] || [ -e "${ROM_DIR}/bezels/gba" ]; }; then
  echo "Bezels are already installed; nothing to do."
  exit 0
fi

detected_res="$(detect_resolution "${RESOLUTION_ARG}")"

if [ -z "${detected_res}" ]; then
  echo "Error: could not detect the screen resolution; bezels not installed." >&2
  exit 1
fi

ensure_bezel_archives

target_zip="/storage/bezels/bezels_${detected_res}.zip"

if [ -f "${target_zip}" ]; then
  mkdir -p "${ROM_DIR}/bezels"
  if unzip -oq "${target_zip}" -d "${ROM_DIR}/bezels/"; then
    echo "Bezels for ${detected_res} installed in ${ROM_DIR}/bezels."
  else
    echo "Error: the bezels archive is damaged: ${target_zip}" >&2
    exit 1
  fi
else
  echo "Error: no bezels archive for ${detected_res} in /storage/bezels." >&2
  exit 1
fi
