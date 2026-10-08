#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Zenith: opens the EKA2L1 (Symbian) frontend from the Tools menu, so it does not need an ES system of its own.

. /etc/profile
set_kill set "-9 eka2l1_sdl2"

/usr/bin/start_eka2l1.sh
