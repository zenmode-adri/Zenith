#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Zenith (proposal): remove the "._*" files that macOS leaves on the game card.
# Only regular files are deleted (the original used rm -rf, which also took folders),
# and it reports how many were removed.

. /etc/profile
set -u

say() { printf '%s\n' "$*"; }
die() { printf 'Error: %s\n' "$*" >&2; sleep 5; exit 1; }

ROMS="/storage/roms"
command -v find >/dev/null 2>&1 || die "find not found."
[ -d "${ROMS}" ] || die "${ROMS} is not available."

say "Looking for ._ files in ${ROMS}..."
count=$(find "${ROMS}" -type f -name '._*' 2>/dev/null | wc -l)
if [ "${count}" -eq 0 ]; then
  say "Nothing to remove."
else
  find "${ROMS}" -type f -name '._*' -delete 2>/dev/null || die "some files could not be removed."
  say "Removed ${count} file(s)."
fi
sleep 3
