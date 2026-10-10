#!/bin/bash
# Uso: cons.sh '<comando>'  -> ejecuta en la consola por SSH (solo lectura salvo que el comando escriba)
HK=SHA256:84k3Gu6Slf8Xh/VmU35IDd3xtDA+/TwSSBReAHMiqC4
timeout ${T:-90} "/c/Program Files/PuTTY/plink.exe" -batch -hostkey $HK -pw rocknix root@192.168.1.118 "$1" < /dev/null
