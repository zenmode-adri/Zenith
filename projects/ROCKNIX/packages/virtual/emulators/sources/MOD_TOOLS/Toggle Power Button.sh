#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Zenith (proposal): switch the power button between "suspend" and "power off".
# The original had Chinese comments and two embedded PNG images in base64; this
# version prints the result in the terminal instead. Needs a quick on-device test:
# it restarts systemd-logind, as the original did.

. /etc/profile
set -u

say() { printf '%s\n' "$*"; }
die() { printf 'Error: %s\n' "$*" >&2; sleep 5; exit 1; }

for cmd in grep sed cut systemctl; do
  command -v "${cmd}" >/dev/null 2>&1 || die "required command '${cmd}' not found."
done

LOGIND_DIR="/storage/.config/logind.conf.d"
LOGIND_CONF="${LOGIND_DIR}/logind.conf"

mkdir -p "${LOGIND_DIR}" || die "cannot create ${LOGIND_DIR}."

if [ ! -f "${LOGIND_CONF}" ]; then
  if [ -f /etc/systemd/logind.conf ]; then
    cp /etc/systemd/logind.conf "${LOGIND_CONF}" || die "cannot copy the default logind.conf."
  else
    printf '[Login]\nHandlePowerKey=suspend\n' > "${LOGIND_CONF}" || die "cannot write ${LOGIND_CONF}."
  fi
fi

current=$(grep -E '^HandlePowerKey=' "${LOGIND_CONF}" | cut -d= -f2 | tail -n 1)
if [ -z "${current}" ]; then
  # the line is missing or commented out: the system default is suspend
  current="suspend"
  if grep -qE '^[#;]*[[:space:]]*HandlePowerKey=' "${LOGIND_CONF}"; then
    sed -i -E 's/^[#;]*[[:space:]]*HandlePowerKey=.*/HandlePowerKey=suspend/' "${LOGIND_CONF}"
  else
    printf 'HandlePowerKey=suspend\n' >> "${LOGIND_CONF}"
  fi
fi

if [ "${current}" = "suspend" ]; then
  new="poweroff"
  label="POWER OFF"
else
  new="suspend"
  label="SUSPEND"
fi

sed -i -E "s/^HandlePowerKey=.*/HandlePowerKey=${new}/" "${LOGIND_CONF}" || die "cannot update ${LOGIND_CONF}."
systemctl restart systemd-logind || die "could not restart systemd-logind."

say "The power button now does: ${label}."
sleep 3
