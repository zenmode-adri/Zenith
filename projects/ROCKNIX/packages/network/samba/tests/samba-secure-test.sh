#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Runs samba-secure with stubbed get_setting / smbpasswd and its paths in a temporary folder. Usage: samba-secure-test.sh [script]
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="${1:-${HERE}/../sources/samba-secure}"
NEWCONF="${HERE}/../config/smb.conf"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0
check() { if eval "$2"; then OK=$((OK+1)); else BAD=$((BAD+1)); echo "FAIL: $1"; fi; }
mkdir -p "${T}/bin" "${T}/storage/.config" "${T}/usr"
: >"${T}/profile"
printf '#!/bin/bash\n[ "$1" = root.password ] && cat "%s/rootpw" 2>/dev/null\n' "${T}" >"${T}/bin/get_setting"
# smbpasswd stub: records its arguments and the two lines it reads, adds the user to the passdb like the real one
printf '#!/bin/bash\necho "$*" >>"%s/calls"; read a; read b; echo "pw=$a/$b" >>"%s/calls"; mkdir -p "$(dirname "%s")"; echo "root:0:x:y:[U]:LCT-0:" >>"%s"\n' \
  "${T}" "${T}" "${T}/cache/samba/smbpasswd" "${T}/cache/samba/smbpasswd" >"${T}/bin/smbpasswd"
chmod +x "${T}"/bin/*
cp "${NEWCONF}" "${T}/usr/smb.conf"
run() { PATH="${T}/bin:${PATH}" SMB_PROFILE="${T}/profile" SMB_USER_CONF="${T}/storage/.config/smb.conf" SMB_DEFAULT_CONF="${T}/usr/smb.conf" \
  SMB_PASSDB="${T}/cache/samba/smbpasswd" bash "${SCRIPT}"; }

# the old default (guest = root) is replaced; root gets the root password
git -C "${HERE}" show e7ec8d15:projects/ROCKNIX/packages/network/samba/config/smb.conf >"${T}/storage/.config/smb.conf" 2>/dev/null
check "old default is the known one" '[ "$(md5sum <"${T}/storage/.config/smb.conf" | cut -d" " -f1)" = 322b4e7deab3924ee71fd5c796791001 ]'
echo "s3cret" >"${T}/rootpw"
run
check "old default replaced" 'cmp -s "${T}/storage/.config/smb.conf" "${NEWCONF}"'
check "smbpasswd called for root" 'grep -qx -- "-s -a root" "${T}/calls"'
check "with the root password twice" 'grep -qx "pw=s3cret/s3cret" "${T}/calls"'
# second run: nothing to do
: >"${T}/calls"; run
check "no second smbpasswd" '[ ! -s "${T}/calls" ]'
# an edited smb.conf stays as it is
echo "# mine" >"${T}/storage/.config/smb.conf"; run
check "edited conf kept" '[ "$(cat "${T}/storage/.config/smb.conf")" = "# mine" ]'
# no root password: no smbpasswd call
rm -f "${T}/cache/samba/smbpasswd" "${T}/rootpw"; : >"${T}/calls"; run
check "no password, no call" '[ ! -s "${T}/calls" ]'
# the new default lets no guest in and shares nothing publicly
check "map to guest = Never" 'grep -q "^  map to guest = Never$" "${NEWCONF}"'
check "no guest account" '! grep -q "guest account" "${NEWCONF}"'
check "no public share" '! grep -q "public = yes" "${NEWCONF}" && [ "$(grep -c "public = no" "${NEWCONF}")" -eq 7 ]'
echo "samba-secure: ${OK} ok, ${BAD} failed"
[ "${BAD}" -eq 0 ]
