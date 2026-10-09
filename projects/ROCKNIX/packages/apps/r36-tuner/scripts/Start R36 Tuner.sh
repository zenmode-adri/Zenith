#!/bin/bash

# SPDX-License-Identifier: MIT
# R36 Tuner Next (zenmode-adri) for Zenith: opens the tuner from the Tools menu.

. /etc/profile

set_kill set "r36-tuner"

sway_fullscreen "r36-tuner" &

/usr/bin/r36-tuner
