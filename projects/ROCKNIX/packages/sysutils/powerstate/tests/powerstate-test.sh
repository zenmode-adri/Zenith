#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Runs powerstate.sh against a fake /sys. Usage: powerstate-test.sh [path to powerstate.sh]
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="${1:-${HERE}/../sources/powerstate.sh}"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0

# run NAME EXPECTED "STATUS ONLINE" ...: each step is what the fake /sys shows on one pass of the loop.
# ONLINE "-" means that no power supply has an online file.
run() {
  local name="$1" expected="$2"; shift 2
  rm -rf "${T}/ps"; mkdir -p "${T}/ps/battery" "${T}/ps/charger"
  : >"${T}/calls"; : >"${T}/levels"
  printf '%s\n' "$@" >"${T}/steps"
  cat >"${T}/profile" <<PROFILE
step() {
  [ -s "${T}/steps" ] || exit 0
  read -r line <"${T}/steps"; sed -i 1d "${T}/steps"
  echo "\${line% *}" >"${T}/ps/battery/status"
  rm -f "${T}/ps/charger/online"
  [ "\${line##* }" = "-" ] || echo "\${line##* }" >"${T}/ps/charger/online"
}
sleep() { step; }
ledcontrol() { :; }
get_setting() { echo "${GPUPERF}"; }
log() { case "\$2" in Switching*) echo "\$2" >>"${T}/calls" ;; esac; }
battery_percent() { echo 50; }
gpu_performance_level() { echo "\$1" >>"${T}/levels"; }
step
PROFILE
  rm -f "${T}/level"; [ -z "${LEVEL}" ] || echo "${LEVEL}" >"${T}/level"
  sed -e "s|^\. /etc/profile|. ${T}/profile|" -e "s|/sys/class/power_supply|${T}/ps|g" -e "s|/tmp/.gpu_performance_level|${T}/level|g" "${SCRIPT}" >"${T}/run.sh"
  timeout 10 bash "${T}/run.sh" >/dev/null 2>&1
  local got; got="$(wc -l <"${T}/calls")"
  if [ "${got}" = "${expected}" ]; then
    OK=$((OK+1)); echo "  PASS ${name}"
  else
    BAD=$((BAD+1)); echo "  FAIL ${name}: ${got} changes of mode, expected ${expected}"
  fi
}

run "on battery, the status comes and goes: one change" 1 \
  "Discharging 0" "Not charging 0" "Discharging 0" "Not charging 0" "Discharging 0"
run "starting in Not charging with no charger: one change" 1 \
  "Not charging 0" "Discharging 0" "Not charging 0"
run "plugged in and not charging: one change" 1 \
  "Not charging 1" "Not charging 1"
run "plug in and unplug: three changes" 3 \
  "Discharging 0" "Charging 1" "Charging 1" "Discharging 0"
run "unplugging while Not charging counts as unplugging" 2 \
  "Not charging 1" "Not charging 0" "Discharging 0"
run "no online file: Not charging is kept as it was" 2 \
  "Discharging -" "Not charging -" "Not charging -"

# levels NAME "EXPECTED LEVELS" "STATUS ONLINE" ...: what the GPU was set to, in order
levels() {
  local name="$1" expected="$2" before="${BAD}"; shift 2
  run "${name}" "$#" "$@" >/dev/null   # every step of these is a change of mode
  BAD="${before}"; OK=$((OK-1))
  local got; got="$(tr '
' ' ' <"${T}/levels")"; got="${got% }"
  if [ "${got}" = "${expected}" ]; then
    OK=$((OK+1)); echo "  PASS ${name}"
  else
    BAD=$((BAD+1)); echo "  FAIL ${name}: levels '${got}', expected '${expected}'"
  fi
}
UNPLUG="Discharging 0"; PLUG="Charging 1"

GPUPERF="" LEVEL="" levels "nothing chosen: only the first pass sets the level" "auto" "${UNPLUG}" "${PLUG}" "${UNPLUG}"
GPUPERF="" LEVEL="" levels "nothing chosen, starting plugged in: the same" "auto" "${PLUG}" "${UNPLUG}" "${PLUG}"
GPUPERF="auto" LEVEL="" levels "auto chosen counts as nothing chosen" "auto" "${UNPLUG}" "${PLUG}"
GPUPERF="low" LEVEL="" levels "a level chosen: on battery, and auto when plugged in" "low auto low" "${UNPLUG}" "${PLUG}" "${UNPLUG}"
GPUPERF="" LEVEL="performance" levels "a game running: its level, on any change" "performance performance" "${UNPLUG}" "${PLUG}"
GPUPERF="low" LEVEL="performance" levels "a game running wins over the setting" "performance performance" "${PLUG}" "${UNPLUG}"

echo "Result: ${OK} passed, ${BAD} failed"
[ "${BAD}" = 0 ]
