#!/bin/bash
# Uso: gridmx.sh <tema-de-prueba> <prefijo>  -> vista de cuadricula en 4 ventanas (640x480, 480x320, 720x450, 720x405)
T=$1; P=$2
for R in "640 480 4x3" "480 320 3x2" "720 450 16x10" "720 405 16x9"; do
  set -- $T $P $R
  bash /c/tmp/zt/gridbase.sh $1 $2-$5 $3 $4 2>&1 | tail -1
done
