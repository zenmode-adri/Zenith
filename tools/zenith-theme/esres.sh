#!/bin/bash
# Uso: esres.sh W H  -> para ES de la consola y lo lanza en ventana WxH dentro de sway (flotante, esquina sup. izq.)
# Restaurar: esres.sh stop   (arranca essway de nuevo)
HK=SHA256:84k3Gu6Slf8Xh/VmU35IDd3xtDA+/TwSSBReAHMiqC4
if [ "$1" = "stop" ]; then
  bash /c/tmp/zt/cons.sh 'killall emulationstation 2>/dev/null; sleep 1; systemctl restart essway; sleep 12; systemctl is-active essway'
  exit 0
fi
W=$1; H=$2; STYLE=${3:-grid}
bash /c/tmp/zt/cons.sh "systemctl stop essway; killall emulationstation 2>/dev/null; sleep 1
sed -i '/defaultView/d' /storage/.config/emulationstation/es_settings.cfg
sed -i 's#<string name=\"GamelistViewStyle\" value=\"[^\"]*\" />#<string name=\"GamelistViewStyle\" value=\"$STYLE\" />#' /storage/.config/emulationstation/es_settings.cfg
export XDG_RUNTIME_DIR=/var/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1 HOME=/storage
. /etc/profile
setsid emulationstation --windowed --resolution $W $H --log-path /tmp > /tmp/es-win.log 2>&1 < /dev/null &
sleep 14
swaymsg '[app_id=\".*\"] floating enable' >/dev/null 2>&1
swaymsg 'floating enable; move position 0 0; resize set $W $H' 2>&1 | head -2
sleep 1
swaymsg -t get_tree 2>/dev/null | grep -E '\"(app_id|name|width|height)\"' | head -12"
