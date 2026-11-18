#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2023 JELOS (https://github.com/JustEnoughLinuxOS)

. /etc/profile

ROOTPASS=$(get_setting root.password)
# Set the root user and password for SyncThing
syncthing generate --gui-user root --gui-password ${ROOTPASS}
# The panel is only reachable from the console itself (the Cloud tab of the web page talks to it here); the sync port stays open
xmlstarlet ed --inplace -u "//configuration/gui/address" -v "127.0.0.1:8384" /storage/.config/syncthing/config.xml

syncthing --no-browser --no-restart
