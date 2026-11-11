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
printf '#!/bin/bash\n:\n' >"${T}/bin/sleep"
chmod +x "${T}"/bin/*

# run NAME VOLTAGE STATUS KILLFILE_CONTENT|- EXPECTED_CALLS(; separated)
run() {
  local name="$1" v="$2" st="$3" kf="$4" expected="$5" alive="${6:-}"
  rm -rf "${T}/bat" "${T}/calls" "${T}/kf" "${T}/stubborn"; mkdir -p "${T}/bat"
  [ -z "${STUBBORN}" ] || touch "${T}/stubborn"
  : >"${T}/calls"
  echo "$v" >"${T}/bat/voltage_avg"; echo "$st" >"${T}/bat/status"
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
echo "  ($((OK)) passed, $((BAD)) failed)"
[ "${BAD}" = 0 ]
