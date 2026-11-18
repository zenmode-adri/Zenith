#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Zenith (proposal): rebuild the SDL controller database. The original ran silently
# (all output sent to /dev/null), so the user never knew if it worked.

. /etc/profile
set -u

say() { printf '%s\n' "$*"; }
die() { printf 'Error: %s\n' "$*" >&2; sleep 5; exit 1; }

command -v python3 >/dev/null 2>&1 || die "python3 not found."
[ -f /usr/bin/update_controller.py ] || die "update_controller.py is missing."

say "Rebuilding the controller database..."
if out=$(python3 /usr/bin/update_controller.py 2>&1); then
  say "Done. Controller mappings updated."
else
  die "the update failed: $(printf '%s' "${out}" | tail -n 1)"
fi
sleep 3
