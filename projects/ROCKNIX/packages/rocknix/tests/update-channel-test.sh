#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# The updater only ever asks Zenith's own releases: never upstream AURKNIX/ROCKNIX (their OTA package has the same name and would
# replace Zenith), whatever an old /etc/os-release says, and a release whose tag is not a build date is not an update.
# Fake profile, curl and jq. Usage: update-channel-test.sh [rocknix-update]
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="${1:-${HERE}/../sources/scripts/rocknix-update}"
OPTIONS="${HERE}/../../../../../distributions/ROCKNIX/options"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0
check() { if eval "$2"; then OK=$((OK+1)); echo "  PASS $1"; else BAD=$((BAD+1)); echo "  FAIL $1 [$2]"; fi; }

mkdir -p "${T}/bin"
sed "s|^\. /etc/profile\$|. ${T}/profile|" "${SCRIPT}" >"${T}/rocknix-update"
# curl: logs the URL, answers with ${T}/json (fails when there is none)
cat >"${T}/bin/curl" <<P
#!/bin/bash
for a in "\$@"; do case "\$a" in http*) echo "\$a" >>"${T}/urls" ;; esac; done
[ -f "${T}/json" ] || exit 22
cat "${T}/json"
P
# jq: the tag is the first word of the "json", the asset URL the second
cat >"${T}/bin/jq" <<P
#!/bin/bash
read -r tag url
case "\$*" in
  *tag_name*) echo "\${tag}" ;;
  *browser_download_url*) echo "\${url}" ;;
esac
P
chmod +x "${T}/bin/curl" "${T}/bin/jq"

profile() {  # $1 org, $2 repo, $3 updates.force
  cat >"${T}/profile" <<P
HW_DEVICE=RK3326 HW_ARCH=aarch64 OS_VERSION=20261110
GIT_ORGANIZATION="$1" GIT_REPO="$2"
get_setting() { [ "\$1" = updates.force ] && echo "$3"; }
set_setting() { :; }
P
}
run() { rm -f "${T}/urls"; OUT="$(PATH="${T}/bin:${PATH}" sh "${T}/rocknix-update" "$@" 2>"${T}/err")"; RC=$?; }

echo "the image's own settings"
check "options name Zenith's repository" "grep -q 'GIT_ORGANIZATION=\"zenmode-adri\"' '${OPTIONS}' && grep -q 'GIT_REPO=\"Zenith\"' '${OPTIONS}'"
check "nothing in options points upstream for updates or bugs" "! grep -E 'GIT_|_URL=' '${OPTIONS}' | grep -qE 'AveyondFly|rocknix.org'"

echo "an image built before the change (os-release says AveyondFly)"
profile AveyondFly distribution_rocknix 0
rm -f "${T}/json"; run check
check "asks Zenith" "grep -q 'repos/zenmode-adri/Zenith/releases/latest' '${T}/urls'"
check "never asks upstream" "! grep -qE 'AveyondFly|ROCKNIX' '${T}/urls'"
check "no update" "[ ${RC} -ne 0 ] && [ -z '${OUT}' ]"
profile AveyondFly distribution_rocknix 20260809
run check
check "forced version: still Zenith" "grep -q 'repos/zenmode-adri/Zenith/releases/tags/20260809' '${T}/urls' && ! grep -q AveyondFly '${T}/urls'"
profile ROCKNIX distribution 1
run releases
check "releases list: still Zenith" "grep -q 'repos/zenmode-adri/Zenith/releases' '${T}/urls' && ! grep -q 'repos/ROCKNIX' '${T}/urls'"

echo "Zenith's releases"
profile zenmode-adri Zenith 0
echo "v0.1.0 https://x/download/v0.1.0/AURKNIX-RK3326.aarch64-v0.1.0.tar" >"${T}/json"; run check
check "a release that is not a build date is not an update" "[ ${RC} -ne 0 ] && [ -z '${OUT}' ]"
echo "20261023 https://x/download/20261023/AURKNIX-RK3326.aarch64-20261023.tar" >"${T}/json"; run check
check "an older build is not an update" "[ ${RC} -ne 0 ] && [ -z '${OUT}' ]"
echo "20270101 https://x/download/20270101/AURKNIX-RK3326.aarch64-20270101.tar" >"${T}/json"; run check
check "a newer build is" "[ ${RC} -eq 0 ] && [ '${OUT}' = 20270101 ]"

echo "${OK} passed, ${BAD} failed"
[ "${BAD}" -eq 0 ]
