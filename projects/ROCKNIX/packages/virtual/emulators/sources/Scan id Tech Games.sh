#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Zenith (proposal): scan /storage/roms/idtech and create the launchers.
# Same job as the ROCKNIX script, but it says what it did and fails with a clear message.

. /etc/profile
set -u

say() { printf '%s\n' "$*"; }
die() { printf 'Error: %s\n' "$*" >&2; sleep 5; exit 1; }

[ -x /usr/bin/generate-idtech.sh ] || die "generate-idtech.sh is missing."

say "Scanning /storage/roms/idtech..."
if /usr/bin/generate-idtech.sh >/dev/null 2>&1; then
  say "Done. If new games do not show up, update the game lists from the main menu."
else
  die "the scan failed. Check that the game files are in /storage/roms/idtech."
fi
sleep 3
