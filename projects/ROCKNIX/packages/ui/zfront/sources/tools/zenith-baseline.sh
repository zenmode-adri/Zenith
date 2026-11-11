#!/bin/sh
# Garantiza que Zenith no cambia: renderiza claro y oscuro a 4 resoluciones x todas las pantallas de screens.txt y compara
# con las sumas guardadas.
# Uso: zenith-baseline.sh make   <binario> <ref.sha>   guarda las sumas (hacerlo con el binario ANTERIOR al cambio)
#      zenith-baseline.sh check  <binario> <ref.sha>   compara (sale con 1 y lista lo que cambio)
# La salida de zfront es determinista (-ffp-contract=off), asi que vale sha256 de los PNG.
set -e
MODE=$1; Z=$2; REF=$3
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d -t zf-baseline.XXXXXX)
trap 'rm -rf "$W"' EXIT
(cd "$W" && sh "$HERE/../../make-themes.sh" >/dev/null)
TAB=$(printf '\t')
for t in light dark; do
  for sz in 480x320 640x480 720x405 720x720; do
    while IFS="$TAB" read -r name args; do
      [ -n "$name" ] || continue
      # shellcheck disable=SC2086
      "$Z" --theme "$W/theme-$t" --size $sz --preview "$W/${t}_${sz}_$name.png" $args 2>/dev/null
    done < "$HERE/screens.txt"
  done
done
(cd "$W" && sha256sum *.png > "$W/now.sha")
if [ "$MODE" = make ]; then cp "$W/now.sha" "$REF"; echo "guardadas $(wc -l < "$REF") sumas en $REF"; exit 0; fi
if diff -q "$REF" "$W/now.sha" >/dev/null; then echo "Zenith IDENTICO ($(wc -l < "$REF") capturas)"; else echo "CAMBIOS en Zenith:"; diff "$REF" "$W/now.sha" | grep '^>' | awk '{print $3}'; exit 1; fi
