#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# The frontend chosen stays chosen: start_es.sh starts what system.frontend says (zfront or es; none = es, as before zfront was the
# default), one failed start does not hand the session to ES (three in a row do, until the next boot), and Switch frontend writes
# the choice down both ways. Fake zfront and ES on a fake /run. Usage: frontend-test.sh [start_es.sh] [Switch frontend.sh]
HERE="$(cd "$(dirname "$0")" && pwd)"
START="${1:-${HERE}/../sources/start_es.sh}"
SWITCH="${2:-${HERE}/../../../misc/modules/sources/Switch frontend.sh}"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0
check() { if eval "$2"; then OK=$((OK+1)); echo "  PASS $1"; else BAD=$((BAD+1)); echo "  FAIL $1 [$2]"; fi; }

mkdir -p "${T}/run" "${T}/log" "${T}/bin"
cat >"${T}/profile" <<P
get_setting() { sed -n "s/^\$1=//p" "${T}/system.cfg" | tail -1; }
set_setting() { del_setting "\$1"; echo "\$1=\$2" >>"${T}/system.cfg"; }
del_setting() { sed -i "/^\$1=/d" "${T}/system.cfg"; }
systemctl() { :; }
P
cp "${T}/profile" "${T}/es_settings"
# the fake zfront does what ${T}/zmode says: ok (draws, exits 0), start (fails before drawing), later (draws, then crashes)
cat >"${T}/bin/zfront" <<P
#!/bin/bash
echo zfront >>"${T}/ran"
case "\$(cat "${T}/zmode")" in
  ok) : >"${T}/ready"; exit 0 ;;
  start) exit 1 ;;
  later) : >"${T}/ready"; exit 139 ;;
esac
P
cat >"${T}/bin/emulationstation" <<P
#!/bin/bash
echo es >>"${T}/ran"
P
chmod +x "${T}/bin/zfront" "${T}/bin/emulationstation"
sed -e "s|/usr/bin/zfront|${T}/bin/zfront|g" -e "s|^emulationstation |${T}/bin/emulationstation |" -e "s|/run/|${T}/run/|g" \
    -e "s|/tmp/emulationstation.ready|${T}/ready|g" -e "s|/var/log|${T}/log|g" -e "s|/sys/class/backlight|${T}/bl|g" "${START}" >"${T}/start_es.sh"
sed -e "s|^\. /etc/profile|. ${T}/profile|" -e "s|/usr/bin/zfront|${T}/bin/zfront|g" -e "s|/run/|${T}/run/|g" "${SWITCH}" >"${T}/switch.sh"
start() { : >"${T}/ran"; rm -f "${T}/ready"; bash "${T}/start_es.sh" >/dev/null 2>&1; echo $? >"${T}/rc"; }
ran() { tr '\n' ' ' <"${T}/ran" | sed 's/ $//'; }
boot() { rm -f "${T}"/run/*; }  # /run is empty after a reboot
cfg() { printf '%s\n' "$@" >"${T}/system.cfg"; }

echo "zfront chosen"
boot; cfg system.frontend=zfront; echo ok >"${T}/zmode"; start
check "starts zfront" "[ \"\$(ran)\" = zfront ]"

echo "ES chosen"
boot; cfg system.frontend=es; start
check "starts ES" "[ \"\$(ran)\" = es ]"

echo "Nothing chosen (installed before zfront was the default)"
boot; cfg system.language=en_US; start
check "starts ES" "[ \"\$(ran)\" = es ]"

echo "zfront fails once before drawing"
boot; cfg system.frontend=zfront; echo start >"${T}/zmode"; start
check "ES not started" "[ \"\$(ran)\" = zfront ]"
check "exits with an error so systemd tries again" "[ \"\$(cat ${T}/rc)\" != 0 ]"
check "no fallback yet" "[ ! -e ${T}/run/zfront.failed ]"
echo ok >"${T}/zmode"; start
check "the next try starts zfront" "[ \"\$(ran)\" = zfront ]"

echo "Failures in between good starts never add up"
boot; cfg system.frontend=zfront
for m in start start ok start start ok start start; do echo $m >"${T}/zmode"; start; done
check "no fallback" "[ ! -e ${T}/run/zfront.failed ]"

echo "Three failed starts in a row"
boot; cfg system.frontend=zfront; echo start >"${T}/zmode"
start; start; start
check "the third hands over to ES" "[ \"\$(ran)\" = 'zfront es' ]"
check "fallback marked until the next boot" "[ -e ${T}/run/zfront.failed ]"
start
check "ES while it lasts" "[ \"\$(ran)\" = es ]"
check "the choice is still zfront" "[ \"\$(grep '^system.frontend=' ${T}/system.cfg)\" = system.frontend=zfront ]"
boot; echo ok >"${T}/zmode"; start
check "after a reboot zfront again" "[ \"\$(ran)\" = zfront ]"

echo "zfront crashes after drawing (systemd starts it again)"
boot; cfg system.frontend=zfront; echo later >"${T}/zmode"; start
check "no ES" "[ \"\$(ran)\" = zfront ]"
check "its own exit code" "[ \"\$(cat ${T}/rc)\" = 139 ]"
start; start; start
check "no fallback however often" "[ ! -e ${T}/run/zfront.failed ]"

echo "Switch frontend writes the choice down"
cfg system.frontend=zfront; bash "${T}/switch.sh" >/dev/null 2>&1
check "zfront -> es" "[ \"\$(grep '^system.frontend=' ${T}/system.cfg)\" = system.frontend=es ]"
: >"${T}/run/zfront.failed"; echo 2 >"${T}/run/zfront.fails"; bash "${T}/switch.sh" >/dev/null 2>&1
check "es -> zfront" "[ \"\$(grep '^system.frontend=' ${T}/system.cfg)\" = system.frontend=zfront ]"
check "and gives zfront a fresh start" "[ ! -e ${T}/run/zfront.failed ] && [ ! -e ${T}/run/zfront.fails ]"
cfg system.language=en_US; bash "${T}/switch.sh" >/dev/null 2>&1
check "nothing chosen (ES) -> zfront" "[ \"\$(grep '^system.frontend=' ${T}/system.cfg)\" = system.frontend=zfront ]"

echo "Result: ${OK} passed, ${BAD} failed"
[ "${BAD}" = 0 ]
