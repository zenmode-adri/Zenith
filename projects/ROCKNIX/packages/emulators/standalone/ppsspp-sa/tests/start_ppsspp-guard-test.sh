#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Test (WSL/PC) of the "Zenith exit guard" section of start_ppsspp.sh: beacon + watchdog that make `killall -TERM ppsspp` save PPSSPP's cache
# but never leave it hanging. Fake PPSSPPs are scripts named "ppsspp" (same process name as the real one), `pkill -x ppsspp` stands in for killall.
# Usage: start_ppsspp-guard-test.sh [path to start_ppsspp.sh]
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="${1:-${HERE}/../scripts/start_ppsspp.sh}"
T="$(mktemp -d)"; trap 'sigk 2>/dev/null; rm -rf "${T}"' EXIT
OK=0; BAD=0
chk() { if [ "$2" = "$3" ]; then OK=$((OK+1)); echo "  PASS $1"; else BAD=$((BAD+1)); echo "  FAIL $1: got '$2' expected '$3'"; fi; }
chkr() { if [ "$2" -ge "$3" ] && [ "$2" -le "$4" ]; then OK=$((OK+1)); echo "  PASS $1 ($2 s)"; else BAD=$((BAD+1)); echo "  FAIL $1: $2 s, expected $3..$4"; fi; }
sig() { pkill -"$1" -x ppsspp; pkill -"$1" -x zppsspp-exit; }
sigk() { pkill -9 -x ppsspp 2>/dev/null; pkill -9 -x zppsspp-exit 2>/dev/null; }
command -v pkill >/dev/null || { echo "  SKIP: pkill missing"; exit 0; }
sigk 2>/dev/null
sed -n '/^### Zenith exit guard (inicio)/,/^### Zenith exit guard (fin)/p' "${SCRIPT}" > "${T}/guard.sh"
[ -s "${T}/guard.sh" ] || { echo "  FAIL: guard section not found in ${SCRIPT}"; exit 1; }
mkdir -p "${T}/fake"
# fake PPSSPP kinds (all named "ppsspp"): quit = leaves 1 s after SIGTERM with code 0 (like SDL_QUIT); deaf = ignores SIGTERM; self = exits by itself after 1 s
cat >"${T}/fake/ppsspp" <<'E'
#!/bin/bash
echo $$ > "$FAKE_DIR/app.pid"
case "$FAKE_KIND" in
  quit) trap 'sleep 1; echo saved >"$FAKE_DIR/cache"; exit 0' TERM; while :; do sleep 0.2; done ;;
  deaf) trap '' TERM; while :; do sleep 0.2; done ;;
  self) sleep 1; exit 0 ;;
esac
E
chmod +x "${T}/fake/ppsspp"
cat >"${T}/driver.sh" <<'E'
ZG_GRACE="${ZG_GRACE:-3}"
[ -n "$FAKE_NOEXEC" ] && chmod() { :; }   # the beacon file stays 644: exec fails with "Permission denied", like /tmp mounted noexec
. "$GUARD"
set_kill() { echo "$*" >>"$FAKE_DIR/setkill"; }
logger() { echo "$*" >>"$FAKE_DIR/log"; }
zg_run "$FAKE_DIR/fake/ppsspp"
echo "RC=$RC TERMED=$ZG_TERMED" > "$FAKE_DIR/result"
E
# runcase NAME KIND ACTION(none|term|stop-term|stop-all-term) [ZG_DIR]; leaves RC TERMED ELAPSED
runcase() {
  rm -rf "${T}/w"; mkdir -p "${T}/w"; export FAKE_DIR="${T}/w" FAKE_KIND="$2" GUARD="${T}/guard.sh" ZG_DIR="${4:-${T}/w/zg}"
  ln -s "${T}/fake" "${T}/w/fake"
  local t0 t1; t0=$(date +%s.%N)
  bash "${T}/driver.sh" >/dev/null 2>&1 &
  local d=$!
  local i=0; while [ ! -s "${T}/w/app.pid" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
  sleep 1.2
  local app; app=$(cat "${T}/w/app.pid" 2>/dev/null)
  case "$3" in
    term) sig TERM ;;
    stop-term) kill -STOP "${app}"; sleep 0.3; sig TERM ;;
    stop-all-term) sig STOP; sleep 0.3; sig TERM ;;
  esac
  local w=0; while kill -0 "$d" 2>/dev/null && [ $w -lt 200 ]; do sleep 0.1; w=$((w+1)); done
  t1=$(date +%s.%N); ELAPSED=$(printf '%.0f' "$(echo "$t1 - $t0 - 1.2" | bc -l)")
  kill -0 "$d" 2>/dev/null && { kill -9 "$d"; DRV=stuck; } || DRV=done
  RES="$(cat "${T}/w/result" 2>/dev/null)"
  APP_LEFT=$(( $(pgrep -x ppsspp | wc -l) + $(pgrep -x zppsspp-exit | wc -l) ))
}
runcase "x" self none
chk "app exits by itself: RC 0, not asked from outside" "${RES}" "RC=0 TERMED=0"
chk "... no ppsspp-named process left (beacon cleaned)" "${APP_LEFT}" 0
chk "... set_kill -TERM ppsspp was written" "$(cat "${T}/w/setkill")" "set -TERM ppsspp zppsspp-exit"
runcase "x" quit term
chk "TERM: PPSSPP exits 0 by itself and saves, asked from outside" "${RES}" "RC=0 TERMED=1"
chk "... cache written (clean exit, not killed)" "$(cat "${T}/w/cache" 2>/dev/null)" saved
chk "... no escalation logged" "$(cat "${T}/w/log" 2>/dev/null | grep -c 'kill -9')" 0
chk "... nothing left" "${APP_LEFT}" 0
runcase "x" deaf term
chk "TERM ignored by PPSSPP: killed with -9 (137) after the grace" "${RES}" "RC=137 TERMED=1"
chkr "... in grace + detection time" "${ELAPSED}" 2 6
chk "... escalation logged" "$(cat "${T}/w/log" 2>/dev/null | grep -c 'kill -9')" 1
chk "... nothing left" "${APP_LEFT}" 0
runcase "x" quit stop-term
chk "PPSSPP stopped (SIGSTOP) then TERM: the watchdog resumes it, it exits 0 by itself and saves" "${RES}" "RC=0 TERMED=1"
chk "... cache written" "$(cat "${T}/w/cache" 2>/dev/null)" saved
chkr "... fast, no grace wait" "${ELAPSED}" 0 4
chk "... nothing left" "${APP_LEFT}" 0
runcase "x" quit stop-all-term
chk "PPSSPP and beacon both stopped (Home menu) then TERM: resumed, clean exit 0 and saved" "${RES}" "RC=0 TERMED=1"
chk "... cache written" "$(cat "${T}/w/cache" 2>/dev/null)" saved
chkr "... fast" "${ELAPSED}" 0 4
chk "... nothing left" "${APP_LEFT}" 0
runcase "x" deaf stop-all-term
chk "hung PPSSPP (ignores TERM) that was also stopped: resumed, still killed (137) after the grace" "${RES}" "RC=137 TERMED=1"
chkr "... in time" "${ELAPSED}" 2 6
chk "... nothing left" "${APP_LEFT}" 0
runcase "x" self none "/proc/nonexistent/zg"
chk "beacon cannot be created: PPSSPP still runs, RC 0, TERMED 0" "${RES}" "RC=0 TERMED=0"
chk "... set_kill NOT switched to -TERM (stays -9 as before)" "$(cat "${T}/w/setkill" 2>/dev/null)" ""
chk "... failure logged" "$(cat "${T}/w/log" 2>/dev/null | grep -c baliza)" 1
# beacon that does not start (noexec /tmp): must NOT look like "someone asked to close" (it would kill a healthy PPSSPP after the grace)
FAKE_NOEXEC=1 runcase "x" quit none
unset FAKE_NOEXEC
chk "beacon does not start: PPSSPP is NOT killed (driver still waiting after 20 s)" "${DRV}" stuck
chk "... no escalation logged" "$(cat "${T}/w/log" 2>/dev/null | grep -c 'kill -9')" 0
chk "... set_kill NOT switched to -TERM" "$(cat "${T}/w/setkill" 2>/dev/null)" ""
sigk 2>/dev/null
# legacy -9 (killall -9 ppsspp): both die, launcher records 137, no escalation noise
rm -rf "${T}/w"; mkdir -p "${T}/w"; export FAKE_DIR="${T}/w" FAKE_KIND=quit GUARD="${T}/guard.sh" ZG_DIR="${T}/w/zg"; ln -s "${T}/fake" "${T}/w/fake"
bash "${T}/driver.sh" >/dev/null 2>&1 & d=$!
sleep 1.5; pkill -9 -x zppsspp-exit; pkill -9 -x ppsspp; w=0; while kill -0 "$d" 2>/dev/null && [ $w -lt 80 ]; do sleep 0.1; w=$((w+1)); done
chk "legacy killall -9 ppsspp: 137, asked from outside, fast" "$(cat "${T}/w/result" 2>/dev/null) w<30=$([ $w -lt 30 ] && echo y || echo n)" "RC=137 TERMED=1 w<30=y"
echo "  ($OK passed, $BAD failed)"
[ "${BAD}" = 0 ]
