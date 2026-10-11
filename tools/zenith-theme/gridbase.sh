#!/bin/bash
# Uso: gridbase.sh <tema> <prefijo> [W H]  -> vista de cuadricula de GBA con el tema (1:1 nativo o ventana WxH)
T=$1; P=$2; W=$3; H=$4
C=/storage/.config/emulationstation/es_settings.cfg
bash /c/tmp/zt/cons.sh "sed -i 's#name=\"ThemeSet\" value=\"[a-z-]*\"#name=\"ThemeSet\" value=\"$T\"#' $C" >/dev/null 2>&1
if [ -z "$W" ]; then
  bash /c/tmp/zt/cons.sh 'killall kbd 2>/dev/null; systemctl restart essway; sleep 22' >/dev/null 2>&1
  W=720; H=720
else
  bash /c/tmp/zt/esres.sh $W $H grid >/dev/null 2>&1
fi
bash /c/tmp/zt/kbdstart.sh >/dev/null 2>&1
bash /c/tmp/zt/shot.sh $P-sys >/dev/null 2>&1
bash /c/tmp/zt/shot.sh $P-grid left A >/dev/null 2>&1
python -c "
from PIL import Image
for s in ('sys','grid'):
    p='C:/tmp/zt/shots/$P-%s.png'%s
    Image.open(p).crop((0,0,$W,$H)).save(p)"
echo "$(date +%H:%M:%S) $P listo"
