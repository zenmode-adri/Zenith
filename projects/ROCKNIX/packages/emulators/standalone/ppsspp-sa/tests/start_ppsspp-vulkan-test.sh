#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Test (WSL/PC) of the Zenith block of start_ppsspp.sh: the graphics backend is chosen by the user in PPSSPP's own menu (ppsspp.ini),
# the launcher only reads it, prepares the Vulkan environment of the loaded driver on EVERY launch, migrates the old ppsspp.vulkan
# setting once, and keeps the Vulkan safety net (2 early failures -> OpenGL written into the ini).
# Fakes: get_setting/set_setting/del_setting, a fake /usr/lib/mali-vk and /sys/module. Usage: start_ppsspp-vulkan-test.sh [path to start_ppsspp.sh]
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="${1:-${HERE}/../scripts/start_ppsspp.sh}"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0
# the block lives between the "### Zenith RK3326" comment and "#Retroachievements"
sed -n '/^### Zenith RK3326/,/^#Retroachievements/p' "${SCRIPT}" | sed '$d' > "${T}/block.sh"
[ -s "${T}/block.sh" ] || { echo "  FAIL: Zenith block not found in ${SCRIPT}"; exit 1; }
# and the per-game "#Graphics Backend" section of the main part
sed -n '/^  #Graphics Backend/,/^  #Internal Resolution/p' "${SCRIPT}" | sed '$d' > "${T}/pergame.sh"
[ -s "${T}/pergame.sh" ] || { echo "  FAIL: #Graphics Backend section not found in ${SCRIPT}"; exit 1; }

chk() { if [ "$2" = "$3" ]; then OK=$((OK+1)); echo "  PASS $1"; else BAD=$((BAD+1)); echo "  FAIL $1: got '$2' expected '$3'"; fi; }
G="GraphicsBackend = 0 (OPENGL)"; V="GraphicsBackend = 3 (VULKAN)"

# ---------------------------------------------------------------------------------------------------------------------------------
# single launch. Variables (set by the caller): MOD (mali_kbase|panfrost|none), INI (none|nolines|nosection|gl|vk|disabled), SET (value of the old
# ppsspp.vulkan or empty), AOFF (old autooff value or empty), MIG=1 (migration marker already there), FAILED (text of FailedGraphicsBackends.txt or
# empty = no file), MARK (content of .vk-running or empty = no marker), PRE_LD (LD_LIBRARY_PATH before). Results in R_* variables.
launch1() {
  rm -rf "${T}/w"; mkdir -p "${T}/w/conf/PSP/SYSTEM" "${T}/w/sys" "${T}/w/vk"
  [ "${MOD}" = none ] || mkdir -p "${T}/w/sys/${MOD}"
  : >"${T}/w/settings"
  [ -z "${SET}" ] || echo "ppsspp.vulkan=${SET}" >>"${T}/w/settings"
  [ -z "${AOFF}" ] || echo "ppsspp.vulkan_autooff=${AOFF}" >>"${T}/w/settings"
  local I="${T}/w/conf/PSP/SYSTEM/ppsspp.ini"
  case "${INI}" in
    none) ;;
    nolines) printf '[General]\nCurrentDirectory = /x\n[Graphics]\nVSyncInterval = False\n' >"${I}" ;;
    nosection) printf '[General]\nCurrentDirectory = /x\n' >"${I}" ;;
    gl) printf '[Graphics]\nGraphicsBackend = 0 (OPENGL)\nVSyncInterval = False\n' >"${I}" ;;
    vk) printf '[Graphics]\nGraphicsBackend = 3 (VULKAN)\nVSyncInterval = False\n' >"${I}" ;;
    disabled) printf '[Graphics]\nGraphicsBackend = 3 (VULKAN)\nDisabledGraphicsBackends = VULKAN\n' >"${I}" ;;
  esac
  [ "${MIG}" = 1 ] && : >"${T}/w/conf/.zenith-backend-migrated"
  [ -z "${FAILED}" ] || printf '%s' "${FAILED}" >"${T}/w/conf/PSP/SYSTEM/FailedGraphicsBackends.txt"
  [ -z "${MARK}" ] || printf '%s\n' "${MARK}" >"${T}/w/conf/.vk-running"
  cat >"${T}/w/run.sh" <<EOS
get_setting() { grep "^\$1=" "${T}/w/settings" 2>/dev/null | cut -d= -f2; }
set_setting() { grep -v "^\$1=" "${T}/w/settings" >"${T}/w/s2" 2>/dev/null; echo "\$1=\$2" >>"${T}/w/s2"; mv "${T}/w/s2" "${T}/w/settings"; }
del_setting() { grep -v "^\$1=" "${T}/w/settings" >"${T}/w/s2" 2>/dev/null; mv "${T}/w/s2" "${T}/w/settings"; }
logger() { :; }
CONF_DIR="${T}/w/conf"; PPSSPP_INI="PSP/SYSTEM/ppsspp.ini"
R36_MALI_VK_DIR="${T}/w/vk"; R36_SYS_MODULE="${T}/w/sys"
unset LD_LIBRARY_PATH VK_DRIVER_FILES VK_IMPLICIT_LAYER_PATH R36_NOBGRA PAN_I_WANT_A_BROKEN_VULKAN_DRIVER PANVK_OPT PANVK_CPU_INDEX_MINMAX DISABLE_MANGOHUD
[ -z "${PRE_LD}" ] || export LD_LIBRARY_PATH="${PRE_LD}"
. "${T}/block.sh"
echo "MODE=\${PVK_MODE}"; echo "VKON=\${PVK_ON}"; echo "LD=\${LD_LIBRARY_PATH}"; echo "VKF=\${VK_DRIVER_FILES}"; echo "IMPL=\${VK_IMPLICIT_LAYER_PATH}"
echo "NOBGRA=\${R36_NOBGRA}"; echo "BROKEN=\${PAN_I_WANT_A_BROKEN_VULKAN_DRIVER}"; echo "PANOPT=\${PANVK_OPT}"; echo "MINMAX=\${PANVK_CPU_INDEX_MINMAX}"
EOS
  local out; out="$(sh "${T}/w/run.sh" 2>&1)"
  R_OUT="${out}"
  for k in MODE VKON LD VKF IMPL NOBGRA BROKEN PANOPT MINMAX; do eval "R_${k}=\"\$(echo \"\${out}\" | sed -n 's/^${k}=//p')\""; done
  R_INI="$(grep -o '^GraphicsBackend = .*' "${I}" 2>/dev/null | head -1)"; [ -n "${R_INI}" ] || R_INI="-"
  R_SET="$(grep '^ppsspp.vulkan=' "${T}/w/settings" | cut -d= -f2)"; [ -n "${R_SET}" ] || R_SET="-"
  R_AOFF="$(grep '^ppsspp.vulkan_autooff=' "${T}/w/settings" | cut -d= -f2)"; [ -n "${R_AOFF}" ] || R_AOFF="-"
  [ -e "${T}/w/conf/PSP/SYSTEM/FailedGraphicsBackends.txt" ] && R_FF=kept || R_FF=gone
  [ -e "${T}/w/conf/.vk-running" ] && R_RUN=yes || R_RUN=no
  [ -e "${T}/w/conf/.zenith-backend-migrated" ] && R_MIG=yes || R_MIG=no
}
reset() { MOD=mali_kbase; INI=gl; SET=; AOFF=; MIG=1; FAILED=; MARK=; PRE_LD=; }

echo "# 1. environment and backend read from the ini (the launcher never chooses it)"
reset; launch1
chk "kbase, ini OpenGL: arm environment prepared, Vulkan not in use" "${R_MODE}|${R_VKON}" "arm|"
chk "... LD_LIBRARY_PATH = loader dir only (no /usr/lib/mali-vk: OpenGL keeps the system libmali)" "${R_LD}" "/usr/lib/r36-vk"
chk "... ICD = mali.json, WSI layer path, nobgra on" "${R_VKF}|${R_IMPL}|${R_NOBGRA}" "/usr/lib/mali-vk/mali.json|/usr/lib/mali-vk|1"
chk "... ini stays OpenGL (the launcher does not write the backend)" "${R_INI}" "${G}"
chk "... no running marker in OpenGL" "${R_RUN}" "no"
reset; INI=vk; launch1
chk "kbase, ini Vulkan: same environment, Vulkan in use" "${R_MODE}|${R_VKON}|${R_LD}|${R_VKF}" "arm|1|/usr/lib/r36-vk|/usr/lib/mali-vk/mali.json"
chk "... ini stays Vulkan" "${R_INI}" "${V}"
chk "... running marker left for the safety net" "${R_RUN}" "yes"
reset; MOD=panfrost; launch1
chk "panfrost, ini OpenGL: PanVK environment prepared, not in use" "${R_MODE}|${R_VKON}|${R_VKF}" "panvk||/usr/lib/r36-vk/panfrost_icd.json"
chk "... PanVK options (broken-driver flag, OPT=4, minmax) and loader dir only" "${R_BROKEN}|${R_PANOPT}|${R_MINMAX}|${R_LD}" "1|4|1|/usr/lib/r36-vk"
chk "... no Arm layer variables with PanVK" "${R_IMPL}|${R_NOBGRA}" "|"
chk "... ini stays OpenGL" "${R_INI}" "${G}"
reset; MOD=panfrost; INI=vk; launch1
chk "panfrost, ini Vulkan: PanVK in use, ini stays Vulkan, marker" "${R_MODE}|${R_VKON}|${R_INI}|${R_RUN}" "panvk|1|${V}|yes"
reset; MOD=none; INI=vk; launch1
chk "no GPU module: no environment, Vulkan not in use, ini untouched" "${R_MODE}|${R_VKON}|${R_LD}|${R_VKF}|${R_INI}|${R_RUN}" "||||${V}|no"
reset; INI=nolines; launch1
chk "ini without GraphicsBackend line: nothing is added (PPSSPP uses its default)" "${R_INI}|${R_VKON}" "-|"
reset; INI=nosection; launch1
chk "ini without [Graphics] section: untouched, no error" "${R_INI}" "-"
reset; INI=none; launch1
chk "no ini at all: no error, environment still prepared" "${R_MODE}|${R_INI}" "arm|-"
reset; INI=disabled; launch1
chk "ini vetoes Vulkan (DisabledGraphicsBackends): not in use, no marker, ini untouched" "${R_VKON}|${R_RUN}|${R_INI}" "|no|${V}"
reset; PRE_LD=/foo/lib; launch1
chk "an existing LD_LIBRARY_PATH is kept after the loader dir" "${R_LD}" "/usr/lib/r36-vk:/foo/lib"
reset; INI=gl; FAILED=VULKAN; launch1
chk "stale failed file VULKAN with ini OpenGL and no marker: file removed, ini stays OpenGL" "${R_FF}|${R_INI}" "gone|${G}"
reset; INI=gl; FAILED=VULKAN; MARK=crash; launch1
chk "ini OpenGL + failed VULKAN + crash marker: safety net does not fire, ini stays OpenGL" "${R_INI}|${R_AOFF}|${R_FF}|${R_RUN}" "${G}|-|gone|no"
reset; INI=vk; FAILED=OPENGL; MARK=crash; launch1
chk "failed file mentions only OPENGL: Vulkan kept" "${R_INI}|${R_VKON}" "${V}|1"
reset; INI=vk; FAILED=VULKAN; launch1
chk "failed file VULKAN but clean previous exit (no marker): Vulkan kept, file removed" "${R_INI}|${R_VKON}|${R_FF}" "${V}|1|gone"
reset; INI=vk; FAILED=VULKAN; MARK=crash; launch1
chk "failed VULKAN + crash marker: OpenGL written into the ini, Vulkan not in use" "${R_INI}|${R_VKON}|${R_FF}|${R_AOFF}" "${G}||gone|crash"
reset; INI=vk; FAILED="VULKAN,OPENGL"; MARK=crash; launch1
chk "failed VULKAN,OPENGL + crash marker: OpenGL" "${R_INI}|${R_AOFF}" "${G}|crash"
reset; INI=nolines; FAILED=VULKAN; MARK=crash; launch1
chk "ini without backend line cannot be 'Vulkan': net does not fire" "${R_INI}|${R_AOFF}" "-|-"
reset; INI=none; FAILED=VULKAN; MARK=crash; launch1
chk "no ini + failed file: no error, nothing written" "${R_INI}|${R_AOFF}" "-|-"

echo "# 2. one-time migration of the old ppsspp.vulkan setting"
reset; MIG=0; SET=1; INI=gl; launch1
chk "ppsspp.vulkan=1, ini OpenGL: ini becomes Vulkan" "${R_INI}|${R_VKON}" "${V}|1"
chk "... setting deleted, marker written" "${R_SET}|${R_MIG}" "-|yes"
reset; MIG=0; SET=0; INI=vk; launch1
chk "ppsspp.vulkan=0, ini Vulkan: ini becomes OpenGL" "${R_INI}|${R_VKON}" "${G}|"
chk "... setting deleted, marker written" "${R_SET}|${R_MIG}" "-|yes"
reset; MIG=0; SET=; INI=vk; launch1
chk "setting empty, ini Vulkan (user's menu choice): ini untouched" "${R_INI}|${R_MIG}" "${V}|yes"
reset; MIG=0; SET=; INI=gl; launch1
chk "setting empty, ini OpenGL: ini untouched" "${R_INI}|${R_MIG}" "${G}|yes"
reset; MIG=0; SET=1; AOFF=early; INI=gl; launch1
chk "old autooff notice is deleted by the migration (and Vulkan is back on)" "${R_AOFF}|${R_INI}" "-|${V}"
reset; MIG=0; SET=1; INI=nolines; launch1
chk "ppsspp.vulkan=1, ini without backend line: Vulkan line is added" "${R_INI}" "${V}"
reset; MIG=0; SET=1; INI=nosection; launch1
chk "ppsspp.vulkan=1, ini without [Graphics]: section and line appended" "${R_INI}" "${V}"
reset; MIG=0; SET=1; INI=none; launch1
chk "ppsspp.vulkan=1 and no ini: no error, setting still deleted" "${R_INI}|${R_SET}|${R_MIG}" "-|-|yes"
reset; MIG=1; SET=1; INI=gl; launch1
chk "migration done: a stale ppsspp.vulkan=1 does NOT override the user's menu choice" "${R_INI}|${R_VKON}" "${G}|"
echo "# 3. per-game explicit graphics_backend (only explicit values write the ini)"
pg() { rm -rf "${T}/p"; mkdir -p "${T}/p"; printf '[Graphics]\nGraphicsBackend = %s\n' "$2" >"${T}/p/ppsspp.ini"
  ( CONF_DIR="${T}/p"; PPSSPP_INI="ppsspp.ini"; GRENDERER="$1"; . "${T}/pergame.sh" ) >/dev/null 2>&1; grep -o '^GraphicsBackend = .*' "${T}/p/ppsspp.ini"; }
chk "graphics_backend=opengl forces OpenGL" "$(pg opengl '3 (VULKAN)')" "${G}"
chk "graphics_backend=vulkan forces Vulkan" "$(pg vulkan '0 (OPENGL)')" "${V}"
chk "graphics_backend empty: the ini is left alone (Vulkan)" "$(pg '' '3 (VULKAN)')" "${V}"
chk "graphics_backend empty: the ini is left alone (OpenGL)" "$(pg '' '0 (OPENGL)')" "${G}"
chk "graphics_backend=auto (anything else): the ini is left alone" "$(pg auto '3 (VULKAN)')" "${V}"

echo "# 4. sequences of launches: the safety net depends on the ini backend"
# seq_start: fresh conf dir, migration done, ini Vulkan (chosen in PPSSPP's menu), kbase module. seq_launch RC FRAMES(yes|no) [TERMED]: runs the block (= the
# launcher's start), then fakes PPSSPP: it writes FailedGraphicsBackends.txt with the backend in use, deletes it if it drew 10 frames (FRAMES=yes), and the
# launcher then gets exit code RC (RC=boot: nothing runs afterwards, as after a power cut). TERMED=1: close asked from outside (SIGTERM, PPSSPP exits 0).
seq_start() { rm -rf "${T}/s"; mkdir -p "${T}/s/conf/PSP/SYSTEM" "${T}/s/sys/mali_kbase" "${T}/s/vk"; : >"${T}/s/settings"; : >"${T}/s/conf/.zenith-backend-migrated"
  printf '[Graphics]\nGraphicsBackend = 3 (VULKAN)\n' >"${T}/s/conf/PSP/SYSTEM/ppsspp.ini"; }
seq_launch() {
  local rc="$1" frames="$2" termed="${3:-0}"
  cat >"${T}/s/run.sh" <<EOS
get_setting() { grep "^\$1=" "${T}/s/settings" 2>/dev/null | cut -d= -f2; }
set_setting() { grep -v "^\$1=" "${T}/s/settings" >"${T}/s/s2" 2>/dev/null; echo "\$1=\$2" >>"${T}/s/s2"; mv "${T}/s/s2" "${T}/s/settings"; }
del_setting() { grep -v "^\$1=" "${T}/s/settings" >"${T}/s/s2" 2>/dev/null; mv "${T}/s/s2" "${T}/s/settings"; }
logger() { :; }
CONF_DIR="${T}/s/conf"; PPSSPP_INI="PSP/SYSTEM/ppsspp.ini"
R36_MALI_VK_DIR="${T}/s/vk"; R36_SYS_MODULE="${T}/s/sys"
. "${T}/block.sh"
echo "VKON=\${PVK_ON}"
# fake PPSSPP
[ -n "\${PVK_ON}" ] && echo VULKAN > "${T}/s/conf/PSP/SYSTEM/FailedGraphicsBackends.txt" || echo OPENGL > "${T}/s/conf/PSP/SYSTEM/FailedGraphicsBackends.txt"
[ "${frames}" = yes ] && rm -f "${T}/s/conf/PSP/SYSTEM/FailedGraphicsBackends.txt"
[ "${rc}" = boot ] || { command -v zvk_after_run >/dev/null 2>&1 && zvk_after_run ${rc} ${termed}; }
exit 0
EOS
  SEQ_MODE="$(sh "${T}/s/run.sh" 2>&1 | sed -n 's/^VKON=//p')"
}
seq_ini() { grep -o '^GraphicsBackend = .*' "${T}/s/conf/PSP/SYSTEM/ppsspp.ini"; }
autooff() { grep '^ppsspp.vulkan_autooff=' "${T}/s/settings" | cut -d= -f2; }
seq_expect() { chk "$1" "${SEQ_MODE}" "$2"; }

seq_start; seq_launch 0 no;   seq_launch 0 no;   seq_expect "early clean exit (code 0, file left behind): next launch stays in Vulkan" 1
seq_start; seq_launch 0 no;   seq_launch 0 no;   seq_launch 0 no; seq_expect "launched twice in a row and left early (code 0): still Vulkan" 1
seq_start; seq_launch 143 no; seq_launch 143 no; seq_launch 143 no; seq_expect "early exit by SIGTERM (143): still Vulkan" 1
seq_start; seq_launch 137 no; seq_launch 137 yes; seq_expect "one early SIGKILL (137, the system exit): still Vulkan" 1
seq_start; seq_launch 137 no; seq_launch 137 no; seq_expect "two early SIGKILLs: the 2nd launch is still Vulkan (1 point)" 1
seq_start; seq_launch 137 no; seq_launch 137 no; seq_launch 0 no; seq_expect "two consecutive early SIGKILLs: 3rd launch is OpenGL" ""
chk "... the ini now says OpenGL (written by the launcher)" "$(seq_ini)" "${G}"
seq_start; seq_launch 137 no; seq_launch 137 yes; seq_launch 137 no; seq_launch 0 no; seq_expect "early kill, a healthy run in between, early kill again: count was reset, Vulkan" 1
seq_start; seq_launch 137 no; seq_launch 0 no; seq_launch 137 no; seq_launch 0 no; seq_expect "early kill, early clean exit in between, early kill: reset, Vulkan" 1
seq_start; seq_launch 139 no; seq_launch 0 no; seq_expect "crash (139) before drawing: next launch is OpenGL" ""
seq_start; seq_launch 134 no; seq_launch 0 no; seq_expect "abort (134) before drawing: next launch is OpenGL" ""
seq_start; seq_launch 1 no;   seq_launch 0 no; seq_expect "exit code 1 before drawing: next launch is OpenGL" ""
seq_start; seq_launch boot no; seq_launch 0 no; seq_expect "hard reset once (no exit code recorded): still Vulkan (1 point)" 1
seq_start; seq_launch boot no; seq_launch boot no; seq_launch 0 no; seq_expect "hard reset twice in a row: OpenGL" ""
seq_start; seq_launch boot yes; seq_launch 0 no; seq_expect "hard reset after a healthy run (file already deleted): Vulkan" 1
seq_start; seq_launch 139 yes; seq_launch 0 no; seq_expect "crash AFTER drawing (file deleted): Vulkan" 1
# the ini says OpenGL (the user's choice): nothing to watch, never a marker, never switches anything
seq_start; printf '[Graphics]\nGraphicsBackend = 0 (OPENGL)\n' >"${T}/s/conf/PSP/SYSTEM/ppsspp.ini"; seq_launch 0 no; seq_launch 137 no; seq_launch 137 no; seq_expect "ini OpenGL: never Vulkan, whatever the exits" ""
chk "... and no marker left in OpenGL mode" "$([ -e "${T}/s/conf/.vk-running" ] && echo yes || echo no)" no
chk "... ini stays OpenGL, no autooff" "$(seq_ini)|$(autooff)" "${G}|"
# exit asked from outside (SIGTERM, code 0): "early" is decided by the frame marker (failed file still there), not by the exit code
seq_start; seq_launch 0 no 1; seq_launch 0 no; seq_expect "TERM before 10 frames (code 0): 1 point, next launch still Vulkan" 1
seq_start; seq_launch 0 no 1; seq_launch 0 no 1; seq_launch 0 no; seq_expect "two TERM exits before 10 frames in a row (black screen): next launch is OpenGL" ""
chk "... and the ini says OpenGL" "$(seq_ini)" "${G}"
seq_start; seq_launch 0 yes 1; seq_launch 0 yes 1; seq_launch 0 yes 1; seq_launch 0 no; seq_expect "TERM after 10+ frames (file deleted): clean, Vulkan" 1
seq_start; seq_launch 0 no 1; seq_launch 0 yes 1; seq_launch 0 no 1; seq_launch 0 no; seq_expect "early TERM, healthy TERM, early TERM: count was reset, Vulkan" 1
seq_start; seq_launch 143 no 1; seq_launch 143 no 1; seq_launch 0 no; seq_expect "TERM that kills PPSSPP before SDL (143), early, twice: OpenGL" ""
seq_start; seq_launch 0 no 1; seq_launch 137 no 1; seq_launch 0 no; seq_expect "early TERM then escalated -9 (137) early: 2 points, OpenGL" ""
seq_start; seq_launch 137 no 1; seq_launch 137 no 1; seq_launch 0 no; seq_expect "two early escalated -9 (137, TERMED): as before, OpenGL" ""
seq_start; seq_launch 137 no 1; seq_launch 137 yes 1; seq_launch 137 no 1; seq_launch 0 no; seq_expect "early -9, healthy -9, early -9: reset, Vulkan" 1
seq_start; seq_launch 139 no 1; seq_launch 0 no; seq_expect "crash (139) before drawing, even with TERMED: OpenGL (2 points)" ""
seq_start; seq_launch 0 no 0; seq_launch 0 no 0; seq_launch 0 no 0; seq_expect "early exit through PPSSPP's own menu (code 0, not asked from outside): not a point" 1
seq_start; seq_launch 0 no 1; seq_launch 0 no 0; seq_launch 0 no; seq_expect "early TERM then own-menu exit: the own-menu exit clears the marker, Vulkan" 1
# the visible notice (setting ppsspp.vulkan_autooff, read by zfront/a Tool; and the journal)
seq_start; seq_launch 137 no; seq_launch 137 no; seq_launch 0 no
chk "two early SIGKILLs: autooff=early is recorded (visible reason)" "$(autooff)" early
seq_launch 0 no; chk "... and it stays while OpenGL is the backend" "$(autooff)" early
sed -i "s/^GraphicsBackend = .*/${V}/" "${T}/s/conf/PSP/SYSTEM/ppsspp.ini"; seq_launch 0 no
chk "user picks Vulkan again in PPSSPP's menu: next launch is Vulkan" "${SEQ_MODE}" 1
chk "... and the autooff notice is cleared" "$(autooff)" ""
seq_start; seq_launch 139 no; seq_launch 0 no
chk "crash (139): autooff=crash" "$(autooff)" crash
seq_start; seq_launch 137 no; seq_launch 137 yes; seq_launch 0 no
chk "no strike pair: no autooff" "$(autooff)" ""
# in-session switch (PPSSPP execs itself, same launcher process, no new launch): launched in OpenGL, closed by a crash -> marker, but the NEXT launch has
# ini Vulkan (the menu saved it before the restart) and the failed file lists VULKAN: 2 points (crash) -> OpenGL
seq_start; printf '[Graphics]\nGraphicsBackend = 0 (OPENGL)\n' >"${T}/s/conf/PSP/SYSTEM/ppsspp.ini"; seq_launch 0 no
sed -i "s/^GraphicsBackend = .*/${V}/" "${T}/s/conf/PSP/SYSTEM/ppsspp.ini"; echo VULKAN >"${T}/s/conf/PSP/SYSTEM/FailedGraphicsBackends.txt"; echo crash >"${T}/s/conf/.vk-running"; seq_launch 0 no
chk "switched to Vulkan in-session and it crashed at once: next launch is OpenGL" "${SEQ_MODE}" ""

echo "# 5. storage full / unwritable files: the game still starts and the net cannot misfire"
# marker unwritable (a directory with its name): printf fails; with `:` in sh the launcher would die before reaching the end
seq_start; mkdir "${T}/s/conf/.vk-running"; seq_launch 139 no
chk "marker not writable (storage full): launcher still reaches the end, Vulkan" "${SEQ_MODE}" 1
seq_launch 139 no; seq_launch 139 no
chk "... and repeated crashes cannot switch Vulkan off (no marker, no points)" "${SEQ_MODE}" 1
chk "... ini untouched, no autooff" "$(seq_ini)|$(autooff)" "${V}|"
seq_start; mkdir "${T}/s/conf/.vk-strikes"; seq_launch 137 no; seq_launch 137 no; seq_launch 137 no
chk "strike counter not writable: never reaches 2, stays Vulkan" "${SEQ_MODE}" 1
if [ "$(id -u)" != 0 ]; then
  # migration marker not writable (conf dir read-only): the launcher survives and, being idempotent, does not undo the user's OpenGL choice
  reset; MIG=0; SET=1; INI=gl; launch1
  sed -i "s/^GraphicsBackend = .*/${G}/" "${T}/w/conf/PSP/SYSTEM/ppsspp.ini"; rm -f "${T}/w/conf/.zenith-backend-migrated"; chmod a-w "${T}/w/conf"
  sh "${T}/w/run.sh" >"${T}/w/o2" 2>&1; chmod u+w "${T}/w/conf"
  chk "migration marker not writable: launcher survives, the user's OpenGL choice is not undone" "$(grep -o '^GraphicsBackend = .*' "${T}/w/conf/PSP/SYSTEM/ppsspp.ini")|$(grep -c '^MODE=arm' "${T}/w/o2")" "${G}|1"
  # the ini cannot be rewritten (sed -i needs the directory): 2 points but OpenGL cannot be written -> still Vulkan this time, no false notice
  seq_start; seq_launch 137 no; seq_launch 137 no; chmod a-w "${T}/s/conf/PSP/SYSTEM"; seq_launch 0 no; chmod u+w "${T}/s/conf/PSP/SYSTEM"
  chk "2 points but the ini cannot be written: launcher survives, no false autooff" "${SEQ_MODE}|$(autooff)" "1|"
  seq_start; seq_launch 0 no; chmod a-w "${T}/s/conf" "${T}/s/conf/PSP/SYSTEM"; seq_launch 139 no; seq_launch 0 no
  chmod u+w "${T}/s/conf" "${T}/s/conf/PSP/SYSTEM"
  chk "read-only conf dir: launcher survives and stays Vulkan" "${SEQ_MODE}" 1
fi
echo "  (${OK} passed, ${BAD} failed)"
[ "${BAD}" = 0 ]
