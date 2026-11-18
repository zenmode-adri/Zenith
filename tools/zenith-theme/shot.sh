#!/bin/bash
# Uso: shot.sh nombre [tecla ...]  (teclas: up down left right enter back esc "sleep 500")
# Envia teclas por el teclado virtual de la consola y baja una captura a C:/tmp/zt/shots/nombre.png
HK=SHA256:84k3Gu6Slf8Xh/VmU35IDd3xtDA+/TwSSBReAHMiqC4
PL="/c/Program Files/PuTTY/plink.exe"; PS="/c/Program Files/PuTTY/pscp.exe"
N=$1; shift
KEYS=""
for k in "$@"; do KEYS="$KEYS echo '$k' > /tmp/tuner/kbd.fifo;"; done
timeout 90 "$PL" -batch -hostkey $HK -pw rocknix root@192.168.1.118 "export XDG_RUNTIME_DIR=/var/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1; $KEYS sleep ${SL:-2}; grim /tmp/$N.png" < /dev/null
mkdir -p /c/tmp/zt/shots
timeout 60 "$PS" -batch -hostkey $HK -pw rocknix root@192.168.1.118:/tmp/$N.png /c/tmp/zt/shots/$N.png < /dev/null 2>&1 | tail -c 80
echo
