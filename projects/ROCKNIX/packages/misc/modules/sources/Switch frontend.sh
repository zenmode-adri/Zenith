#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Zenith: switches the frontend between EmulationStation and zfront from the Tools menu, and writes the choice down
# (`system.frontend=zfront` or `es` in system.cfg; without the setting EmulationStation starts). Nothing else changes it.
# The interface restarts by itself (about 10 s): run it again from Tools to come back.

. /etc/profile

if [ "$(get_setting system.frontend)" = "zfront" ]
then
  set_setting system.frontend es
else
  [ -x /usr/bin/zfront ] || exit 1
  rm -f /run/zfront.failed /run/zfront.fails  # a fresh start for zfront
  set_setting system.frontend zfront
fi

systemctl restart --no-block essway
