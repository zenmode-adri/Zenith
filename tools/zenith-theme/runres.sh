#!/bin/bash
# Uso: runres.sh W H estilo etiqueta  -> lanza ES en ventana WxH, va a la lista de GBA y captura sistemas + lista
W=$1; H=$2; ST=$3; L=$4
bash /c/tmp/zt/esres.sh $W $H $ST >/dev/null 2>&1
bash /c/tmp/zt/kbdstart.sh >/dev/null
bash /c/tmp/zt/shot.sh ${L}_sys 'sleep 600' >/dev/null
bash /c/tmp/zt/shot.sh ${L}_n right 'sleep 3500' >/dev/null
bash /c/tmp/zt/shot.sh ${L}_${ST} A 'sleep 6500' >/dev/null
echo hecho $L
