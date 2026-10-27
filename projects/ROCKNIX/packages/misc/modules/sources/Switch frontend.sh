#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Zenith: switches the frontend between EmulationStation and zfront from the Tools menu.
# `system.frontend=zfront` makes start_es.sh start zfront; without the setting EmulationStation starts.
# The interface restarts by itself (about 10 s): run it again from Tools to come back.

. /etc/profile

if [ "$(get_setting system.frontend)" = "zfront" ]
then
  del_setting system.frontend
else
  [ -x /usr/bin/zfront ] || exit 1
  rm -f /run/zfront.failed
  set_setting system.frontend zfront
fi

systemctl restart essway
