#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Runs battery-guard with a fake sysfs and stubbed killall/poweroff/sync/logger. Usage: battery-guard-test.sh [path to battery-guard]
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="${1:-${HERE}/../sources/scripts/battery-guard}"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0
mkdir -p "${T}/bin"
for c in logger sync; do printf '#!/bin/bash\nif [ "%s" = sync ]; then echo sync >>"%s/calls"; fi\n' "$c" "${T}" >"${T}/bin/$c"; done
# killall stub: -0 succeeds while the name is in "alive"; -TERM records the call and, unless "stubborn" exists, removes it from "alive".
cat >"${T}/bin/killall" <<EOS
#!/bin/bash
sig="\$1"; n="\$2"
if [ "\$sig" = "-0" ]; then grep -qx "\$n" "${T}/alive"; exit \$?; fi
echo "killall \$sig \$n" >>"${T}/calls"
[ -e "${T}/stubborn" ] || sed -i "/^\$n\$/d" "${T}/alive"
EOS
printf '#!/bin/bash\necho poweroff >>"%s/calls"\n' "${T}" >"${T}/bin/fakepoweroff"
# sleep stub: each call applies the next voltage of "${T}/seq" (if any), so a test can simulate voltage bounces
cat >"${T}/bin/sleep" <<EOS
#!/bin/bash
if [ -s "${T}/seq" ]; then head -1 "${T}/seq" >"${T}/bat/voltage_avg"; sed -i 1d "${T}/seq"; fi
EOS
chmod +x "${T}"/bin/*

# run NAME VOLTAGE STATUS KILLFILE_CONTENT|- EXPECTED_CALLS(; separated) [ALIVE_NAMES] [CAPACITY] [VOLTAGE_SEQUENCE: next voltages, one per loop pass]
# CAPACITY empty: no "capacity" file at all (kernel without the gauge).
run() {
  local name="$1" v="$2" st="$3" kf="$4" expected="$5" alive="${6:-}" cap="${7:-}" seq="${8:-}"
  rm -rf "${T}/bat" "${T}/calls" "${T}/kf" "${T}/stubborn" "${T}/seq"; mkdir -p "${T}/bat"
  [ -z "${seq}" ] || printf '%s\n' ${seq} >"${T}/seq"
  [ -z "${STUBBORN}" ] || touch "${T}/stubborn"
  : >"${T}/calls"
  echo "$v" >"${T}/bat/voltage_avg"; echo "$st" >"${T}/bat/status"
  [ -z "${cap}" ] || echo "${cap}" >"${T}/bat/capacity"
  [ "$kf" = "-" ] || printf '%s\n' "$kf" >"${T}/kf"
  printf '%s\n' ${alive} >"${T}/alive"
  # the loop is endless unless it powers off: stop it after 2 passes with a timeout
  PATH="${T}/bin:${PATH}" BAT_DIR="${T}/bat" BATGUARD_READS=3 BATGUARD_EVERY=0 BATGUARD_WAIT=3 \
    BATGUARD_POWEROFF="${T}/bin/fakepoweroff" BATGUARD_KILLFILE="${T}/kf" BATGUARD_KILLALL="${T}/bin/killall" \
    timeout 5 bash "${SCRIPT}" >/dev/null 2>&1
  local got; got="$(paste -sd';' "${T}/calls")"
  if [ "${got}" = "${expected}" ]; then OK=$((OK+1)); echo "  PASS ${name}"
  else BAD=$((BAD+1)); echo "  FAIL ${name}: got '${got}', expected '${expected}'"; fi
}

run "voltage OK: nothing" 3800000 Discharging "retroarch" "" "retroarch"
run "low but charging: nothing" 3000000 Charging "retroarch" "" "retroarch"
run "low, game running: SIGTERM to both, sync, then poweroff" 3000000 Discharging "retroarch retroarch32" \
  "killall -TERM retroarch;killall -TERM retroarch32;sync;poweroff" "retroarch retroarch32"
run "leading signal dropped: -9 PPSSPPSDL becomes SIGTERM" 3000000 Discharging "-9 PPSSPPSDL" \
  "killall -TERM PPSSPPSDL;sync;poweroff" "PPSSPPSDL"
run "unsafe and protected names refused" 3000000 Discharging "emulationstation zfront sway ../x a;b" "poweroff"
run "no kill file: poweroff only" 3000000 Discharging "-" "poweroff"
run "kill file but game already gone: SIGTERM, sync, poweroff" 3000000 Discharging "retroarch" \
  "killall -TERM retroarch;sync;poweroff" ""
STUBBORN=1 run "game ignores SIGTERM: waits, then sync and poweroff anyway" 3000000 Discharging "retroarch" "killall -TERM retroarch;sync;poweroff" "retroarch"
# capacity 0 (the gauge publishes it at its empty anchor): shuts down only if the voltage is ALSO low (< 3.5 V); never with a normal voltage
run "capacity 0, voltage normal (3.8 V): nothing" 3800000 Discharging "-" "" "" 0
run "capacity 0, Not charging, voltage normal: nothing" 3800000 "Not charging" "-" "" "" 0
run "capacity 0, voltage 3.5 V exactly (not below): nothing" 3500000 Discharging "-" "" "" 0
run "capacity 0, voltage low (3.4 V) sustained: poweroff" 3400000 Discharging "-" "poweroff" "" 0
run "capacity 0, voltage low, Not charging: poweroff" 3400000 "Not charging" "-" "poweroff" "" 0
run "capacity 0, voltage low, game running: SIGTERM, sync, poweroff" 3400000 Discharging "retroarch"   "killall -TERM retroarch;sync;poweroff" "retroarch" 0
run "capacity 0, voltage unreadable (no file): nothing" "" Discharging "-" "" "" 0
run "capacity 0, voltage bounces above 3.5 V: count restarts, nothing" 3400000 Discharging "-" "" "" 0 "3400000 3800000 3400000 3400000 3800000"
run "capacity 0, voltage bounces then 3 low reads in a row: poweroff" 3400000 Discharging "-" "poweroff" "" 0 "3400000 3800000 3400000 3400000 3400000"
run "capacity 0 but Charging, voltage low: nothing" 3400000 Charging "-" "" "" 0
run "capacity 0 but Full: nothing" 3800000 Full "-" "" "" 0
run "capacity 1, voltage 3.4 V (above 3.2 V): nothing" 3400000 Discharging "-" "" "" 1
run "capacity 1, voltage 3.1 V: poweroff by voltage (as before)" 3100000 Discharging "-" "poweroff" "" 1
run "capacity 100: nothing" 4100000 Discharging "-" "" "" 100
run "no capacity file (old kernel), voltage fine: nothing" 3800000 Discharging "-" ""
run "no capacity file, voltage 3.4 V: nothing (only the 3.2 V criterion)" 3400000 Discharging "-" ""
echo "  ($((OK)) passed, $((BAD)) failed)"
[ "${BAD}" = 0 ]
