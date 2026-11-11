#!/bin/sh
# Captura una variante en 4 resoluciones x todas las pantallas de screens.txt y monta imagenes con rejilla + el informe de contraste.
# Uso (en WSL): shoot-variant.sh <paleta.json> <carpeta_de_trabajo>   [ZFRONT=/ruta/al/binario x86]
# Genera <trabajo>/theme-<name> (mkvariant.py sobre theme-light/theme-dark de la misma carpeta, hechos con make-themes.sh),
# las capturas sueltas <trabajo>/shots/<name>/<WxH>_<pantalla>.png y los montajes <trabajo>/<name>_montaje.png (1.ª pagina,
# con el contraste), <name>_montaje2.png (2.ª pagina)...
set -e
PAL=$1; W=$2
HERE=$(cd "$(dirname "$0")" && pwd)
ZFRONT=${ZFRONT:-./build/x86/zfront}
NAME=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['name'])" "$PAL")
MODE=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['mode'])" "$PAL")
[ -d "$W/theme-$MODE" ] || (cd "$W" && sh "$HERE/../../make-themes.sh" >/dev/null)
python3 "$HERE/mkvariant.py" "$PAL" "$W/theme-$MODE" "$W/theme-$NAME"
S="$W/shots/$NAME"; rm -rf "$S"; mkdir -p "$S"
TAB=$(printf '\t')
for sz in 480x320 640x480 720x405 720x720; do
  while IFS="$TAB" read -r sname args; do
    [ -n "$sname" ] || continue
    # shellcheck disable=SC2086
    "$ZFRONT" --theme "$W/theme-$NAME" --size $sz --preview "$S/${sz}_$sname.png" $args 2>/dev/null
  done < "$HERE/screens.txt"
done
python3 "$HERE/montage.py" "$PAL" "$S" "$W/${NAME}_montaje.png"
