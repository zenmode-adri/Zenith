#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Runs userconfig-setup on a fake /storage: the theme links are there after an update too. Usage: userconfig-themes-test.sh [script]
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="${1:-${HERE}/../scripts/userconfig-setup}"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0
check() { if eval "$2"; then OK=$((OK+1)); echo "  PASS $1"; else BAD=$((BAD+1)); echo "  FAIL $1 [$2]"; fi; }

cat >"${T}/functions" <<'P'
tocon() { :; }
P
fresh() {
  rm -rf "${T}/storage"; mkdir -p "${T}/storage/.config/locale" "${T}/storage/.cache"
  : >"${T}/storage/.cache/ld.so.cache"
  echo "device" >"${T}/storage/.configured"
}
# with ES on the image; the second argument, any: without it
boot() {
  [ -n "$1" ] && rm -f "${T}/es" || : >"${T}/es"
  sed -e "s|^\. /etc/profile.d/001-functions|. ${T}/functions|" \
      -e "s|/storage|${T}/storage|g" -e "s|/usr/bin/emulationstation|${T}/es|g" "${SCRIPT}" >"${T}/run.sh"
  QUIRK_DEVICE=device bash "${T}/run.sh" >/dev/null 2>&1
}
L="${T}/storage/.config/emulationstation/themes"
link() { readlink "${L}/$1"; }

echo "Already configured, an update brought new themes"
fresh; boot
check "Next linked" "[ \"\$(link es-theme-zenith-next)\" = /usr/share/themes/es-theme-zenith-next ]"
check "Next dark linked" "[ \"\$(link es-theme-zenith-next-dark)\" = /usr/share/themes/es-theme-zenith-next-dark ]"
check "the others too" "[ \"\$(link es-theme-iconic)\" = /usr/share/themes/es-theme-iconic ]"

echo "A link to somewhere else is replaced"
fresh; mkdir -p "${L}"; ln -s /tmp/zt-stage/themes/es-theme-zenith-next "${L}/es-theme-zenith-next"; boot
check "points to the image" "[ \"\$(link es-theme-zenith-next)\" = /usr/share/themes/es-theme-zenith-next ]"

echo "A folder of the user stays as it is"
fresh; mkdir -p "${L}/es-theme-iconic"; : >"${L}/es-theme-iconic/mine.xml"; boot
check "still a folder" "[ -d '${L}/es-theme-iconic' ] && [ ! -L '${L}/es-theme-iconic' ]"
check "nothing added inside" "[ \"\$(ls '${L}/es-theme-iconic')\" = mine.xml ]"

echo "A good link is not written again"
fresh; boot; a="$(stat -c %i "${L}/es-theme-zenith")"; boot
check "same link" "[ '${a}' = \"\$(stat -c %i '${L}/es-theme-zenith')\" ]"

echo "An image without ES does not get the folder"
fresh; boot none
check "no folder" "[ ! -e '${L}' ]"

echo "Result: ${OK} passed, ${BAD} failed"
[ "${BAD}" = 0 ]
