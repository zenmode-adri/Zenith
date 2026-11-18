#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Zenith (proposal): scan /storage/roms/scummvm and rebuild the game list.
# Based on the ROCKNIX/JELOS script. Warns before the interface restarts and
# refuses to run while a game is open (the restart would kill it).

. /etc/profile
set -u

say() { printf '%s\n' "$*"; }
die() { printf 'Error: %s\n' "$*" >&2; sleep 5; exit 1; }

for cmd in systemctl pidof; do
  command -v "${cmd}" >/dev/null 2>&1 || die "required command '${cmd}' not found."
done
[ -f /usr/bin/start_scummvm.sh ] || die "ScummVM helper (start_scummvm.sh) is missing."
[ -n "${UI_SERVICE:-}" ] || die "the interface service is not known (UI_SERVICE is empty)."

for emu in retroarch scummvm drastic hatari flycast; do  # any emulator open would die with the restart
  if [ -n "$(pidof "${emu}" 2>/dev/null)" ]; then
    die "a game is running (${emu}). Close it first: this tool restarts the interface."
  fi
done

say "Scanning /storage/roms/scummvm for games..."
bash /usr/bin/start_scummvm.sh add >/dev/null 2>&1 || say "Warning: the scan reported a problem; continuing."
say "Adding the games to the list..."
bash /usr/bin/start_scummvm.sh create >/dev/null 2>&1 || die "could not create the game entries."

say ""
say "Done. The interface will restart in 5 seconds (about 10 s to come back)."
sleep 5
systemctl restart --no-block "${UI_SERVICE}"
