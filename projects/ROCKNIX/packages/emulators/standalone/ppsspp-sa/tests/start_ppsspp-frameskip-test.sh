#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Test (WSL/PC) of the "Auto Frame Skip" block of start_ppsspp.sh (FrameSkip/AutoFrameSkip + one-time migration marker)
# with a fake get_setting. Usage: start_ppsspp-frameskip-test.sh [path to start_ppsspp.sh]
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="${1:-${HERE}/../scripts/start_ppsspp.sh}"
T="$(mktemp -d)"; trap 'chmod -R u+w "${T}" 2>/dev/null; rm -rf "${T}"' EXIT
OK=0; BAD=0
# the block lives between the "#Auto Frame Skip" and "#Graphics Backend" comments
sed -n '/^  #Auto Frame Skip/,/^  #Graphics Backend/p' "${SCRIPT}" | sed '$d' > "${T}/block.sh"
[ -s "${T}/block.sh" ] || { echo "  FAIL: Auto Frame Skip block not found in ${SCRIPT}"; exit 1; }
MARK=".zenith-frameskip-migrated"

# setup: fresh conf dir with an ini holding FS (FrameSkip line; "-" = no line) and AUTO (AutoFrameSkip value)
setup() {
  rm -rf "${T}/w"; mkdir -p "${T}/w/conf/PSP/SYSTEM"
  { printf '[Graphics]\nInternalResolution = 1\n'
    [ "$1" = "-" ] || printf 'FrameSkip = %s\n' "$1"
    printf 'FrameSkipType = 0\n'
    [ "$2" = "-" ] || printf 'AutoFrameSkip = %s\n' "$2"
    printf 'FrameSkipUnthrottle = True\n'; } >"${T}/w/conf/PSP/SYSTEM/ppsspp.ini"
}
# launch ASKIP: runs the block like the launcher does (ASKIP="" = setting not set by the frontend)
launch() {
  cat >"${T}/w/run.sh" <<EOS
logger() { :; }
CONF_DIR="${T}/w/conf"; PPSSPP_INI="PSP/SYSTEM/ppsspp.ini"; ASKIP="$1"
. "${T}/block.sh"
echo REACHED_END
EOS
  sh "${T}/w/run.sh" 2>&1 | grep -q '^REACHED_END$' && REACHED=yes || REACHED=no
}
ini() { sed -n "s/^$1 = //p" "${T}/w/conf/PSP/SYSTEM/ppsspp.ini" | head -n 1; }
chk() { if [ "$2" = "$3" ]; then OK=$((OK+1)); echo "  PASS $1"; else BAD=$((BAD+1)); echo "  FAIL $1: got '$2' expected '$3'"; fi; }

# (a) inherited ini FrameSkip=3 / Auto=True, no frontend setting
setup 3 True; launch ""
chk "(a) inherited 3/True, no setting: FrameSkip = 0" "$(ini FrameSkip)" 0
chk "(a) ... AutoFrameSkip = False" "$(ini AutoFrameSkip)" False
chk "(a) ... marker created" "$([ -e "${T}/w/conf/${MARK}" ] && echo yes || echo no)" yes
chk "(a) ... FrameSkipType / FrameSkipUnthrottle untouched" "$(ini FrameSkipType)/$(ini FrameSkipUnthrottle)" "0/True"
# (b) user later picks FrameSkip=2 in the PPSSPP menu: respected on the next launch
sed -i 's/^FrameSkip = 0$/FrameSkip = 2/' "${T}/w/conf/PSP/SYSTEM/ppsspp.ini"; launch ""
chk "(b) second launch, user chose FrameSkip=2: respected" "$(ini FrameSkip)" 2
chk "(b) ... AutoFrameSkip stays False" "$(ini AutoFrameSkip)" False
launch 0; chk "(b) auto_frame_skip=0 behaves the same: 2 kept" "$(ini FrameSkip)" 2
# (c) auto_frame_skip=1 with FrameSkip=0
setup 0 False; launch 1
chk "(c) auto=1, FrameSkip 0: AutoFrameSkip = True" "$(ini AutoFrameSkip)" True
chk "(c) ... FrameSkip = 3 (limit)" "$(ini FrameSkip)" 3
chk "(c) ... no marker created in auto mode" "$([ -e "${T}/w/conf/${MARK}" ] && echo yes || echo no)" no
# (d) auto_frame_skip=1 with FrameSkip=2
setup 2 False; launch 1
chk "(d) auto=1, FrameSkip 2: AutoFrameSkip = True" "$(ini AutoFrameSkip)" True
chk "(d) ... FrameSkip = 2 respected" "$(ini FrameSkip)" 2
# (e) marker not writable (conf dir read-only; ini dir still writable): launcher reaches the end, FrameSkip = 0
if [ "$(id -u)" != 0 ]; then
  setup 3 True; chmod a-w "${T}/w/conf"; launch ""; chmod u+w "${T}/w/conf"
  chk "(e) read-only marker dir: launcher reaches the end" "${REACHED}" yes
  chk "(e) ... FrameSkip = 0" "$(ini FrameSkip)" 0
  chk "(e) ... AutoFrameSkip = False" "$(ini AutoFrameSkip)" False
fi
# (f) extra cases
setup - -; launch ""
chk "(f) ini without FrameSkip/Auto lines, no setting: launcher survives" "${REACHED}" yes
setup - True; launch 1
chk "(f) auto=1, ini without FrameSkip line: line added = 3" "$(ini FrameSkip)" 3
setup 5 True; launch ""
chk "(f) marker absent but FrameSkip=5 inherited: migrated to 0 (documented one-time reset)" "$(ini FrameSkip)" 0
chk "(f) FrameSkipType line not confused with FrameSkip" "$(ini FrameSkipType)" 0
rm -rf "${T}/w"; mkdir -p "${T}/w/conf"; launch ""
chk "(f) no ini at all: launcher survives" "${REACHED}" yes
# (g) anisotropic filter one-time migration (marker .zenith-aniso-migrated): only the old factory value 4 becomes 0
ANM=".zenith-aniso-migrated"
asetup() { setup 0 False; sed -i "/^\[Graphics\]/a AnisotropyLevel = $1" "${T}/w/conf/PSP/SYSTEM/ppsspp.ini"; [ -z "$2" ] || : >"${T}/w/conf/${ANM}"; }
asetup 4 ""; launch ""
chk "(g) AnisotropyLevel 4 without marker: 0" "$(ini AnisotropyLevel)" 0
chk "(g) ... marker created" "$([ -e "${T}/w/conf/${ANM}" ] && echo yes || echo no)" yes
asetup 2 ""; launch ""
chk "(g) AnisotropyLevel 2 without marker: respected" "$(ini AnisotropyLevel)" 2
asetup 4 1; launch ""
chk "(g) AnisotropyLevel 4 chosen by the user (marker present): respected" "$(ini AnisotropyLevel)" 4
asetup 0 ""; launch ""
chk "(g) AnisotropyLevel 0: stays 0" "$(ini AnisotropyLevel)" 0
if [ "$(id -u)" != 0 ]; then
  asetup 4 ""; chmod a-w "${T}/w/conf"; launch ""; chmod u+w "${T}/w/conf"
  chk "(g) read-only conf dir: launcher reaches the end" "${REACHED}" yes
fi
asetup 4 ""; sed -i '/^AnisotropyLevel/d' "${T}/w/conf/PSP/SYSTEM/ppsspp.ini"; launch ""
chk "(g) ini without AnisotropyLevel line: launcher survives" "${REACHED}" yes
echo "  (${OK} passed, ${BAD} failed)"
[ "${BAD}" = 0 ]
