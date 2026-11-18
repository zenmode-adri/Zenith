#!/bin/bash
HK=SHA256:84k3Gu6Slf8Xh/VmU35IDd3xtDA+/TwSSBReAHMiqC4
bash /c/tmp/zt/cons.sh 'killall kbd 2>/dev/null; mkdir -p /tmp/tuner'
timeout 60 "/c/Program Files/PuTTY/pscp.exe" -batch -hostkey $HK -pw rocknix /c/tmp/zt/kbd root@192.168.1.118:/tmp/tuner/kbd < /dev/null | tail -c 30; echo
bash /c/tmp/zt/cons.sh 'chmod +x /tmp/tuner/kbd; setsid /tmp/tuner/kbd > /tmp/tuner/kbd.log 2>&1 < /dev/null & sleep 4; cat /tmp/tuner/kbd.log'
