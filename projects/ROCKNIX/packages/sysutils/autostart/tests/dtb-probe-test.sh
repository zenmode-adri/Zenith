#!/bin/bash
# Pruebas del modo de prueba de DTB por simulacion (no toca ninguna consola ni tarjeta).
# Uso: bash dtb-probe-test.sh [ruta/a/dtb-probe.sh]
SCRIPT="${1:-$(dirname "$0")/../sources/firstboot/dtb-probe.sh}"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok()   { pass=$((pass+1)); echo "  PASS $1"; }
bad()  { fail=$((fail+1)); echo "  FAIL $1"; }
check() { if eval "$2"; then ok "$1"; else bad "$1  [$2]"; fi; }

# --- stubs ---
mkdir -p "$T/bin" "$T/in"
cat >"$T/bin/evtest" <<'EOS'
#!/bin/bash
# simula un boton: devuelve 10 (pulsado) cuando el numero de consultas llega a $STUB/press_after (0 = nunca)
n=$(cat "$STUB/polls" 2>/dev/null || echo 0); n=$((n+1)); echo $n >"$STUB/polls"
pa=$(cat "$STUB/press_after" 2>/dev/null || echo 0)
[ "$pa" -gt 0 ] && [ "$n" -ge "$pa" ] && exit 10
exit 0
EOS
cat >"$T/bin/reboot" <<'EOS'
#!/bin/bash
echo x >>"$STUB/reboots"
EOS
cat >"$T/bin/zsplash" <<'EOS'
#!/bin/bash
echo "$*" >>"$STUB/splash"
EOS
chmod +x "$T/bin/"*
: >"$T/in/platform-test-event-joystick"
: >"$T/in/platform-test-adc-keys-event"

ORIG='LABEL ROCKNIX
  LINUX /KERNEL
  ## nota que no debe tocarse
  FDT /a.dtb
  APPEND boot=${partition_boot} quiet loglevel=1 console=ttyS2,1500000 ssh uboot.hwid_adc=${hwid_adc}
'

# crea un /flash falso: setup <lista...>  -> FDT inicial = primera
setup() {
  rm -rf "$T/flash" "$T/stub"; mkdir -p "$T/flash/extlinux" "$T/flash/dtbprobe" "$T/stub"
  printf '%s' "$ORIG" >"$T/flash/extlinux/extlinux.conf"
  cp "$T/flash/extlinux/extlinux.conf" "$T/flash/dtbprobe/extlinux.orig"
  : >"$T/flash/KERNEL"
  : >"$T/flash/a.dtb"; : >"$T/flash/b.dtb"; : >"$T/flash/c.dtb"
  echo active >"$T/flash/dtbprobe/state"
  printf '%s\n' "$@" >"$T/flash/dtbprobe/list"
  echo 0 >"$T/flash/dtbprobe/index"
}
run() {   # run [press_after]
  echo "${1:-0}" >"$T/stub/press_after"
  DTBPROBE_FLASH="$T/flash" DTBPROBE_TIMEOUT=2 DTBPROBE_POLL=0.05 DTBPROBE_IGNORE_FIRST=0 DTBPROBE_REMOUNT=0 \
  DTBPROBE_REBOOT="$T/bin/reboot" DTBPROBE_EVTEST="$T/bin/evtest" DTBPROBE_INPUT_DIR="$T/in" ZSPLASH="$T/bin/zsplash" \
  STUB="$T/stub" bash "$SCRIPT" >"$T/out.txt" 2>&1
  RC=$?
}
fdt() { awk '/^[ \t]*FDT[ \t]+/{print $2; exit}' "$T/flash/extlinux/extlinux.conf"; }
reboots() { if [ -f "$T/stub/reboots" ]; then wc -l <"$T/stub/reboots"; else echo 0; fi; }
sed_sleep() { :; }
# el script duerme 60 s tras reboot: para las pruebas se sustituye por una copia sin esas esperas largas
FAST="$T/dtb-probe.fast.sh"; sed -e 's/sleep 60/:/g' "$SCRIPT" >"$FAST"; SCRIPT="$FAST"

echo "T1: sin marca de prueba no hace nada"
setup a.dtb b.dtb c.dtb; rm -rf "$T/flash/dtbprobe"; run
check "sale con 0" "[ $RC -eq 0 ]"; check "extlinux intacto" "[ \"\$(cat $T/flash/extlinux/extlinux.conf)\" = \"\$(printf '%s' \"\$ORIG\")\" ]"; check "no reinicia" "[ \"\$(reboots)\" -eq 0 ]"

echo "T2: sin boton avanza al siguiente candidato y reinicia"
setup a.dtb b.dtb c.dtb; run 0
check "FDT = b.dtb" "[ \"\$(fdt)\" = /b.dtb ]"; check "indice = 1" "[ \"\$(cat $T/flash/dtbprobe/index)\" = 1 ]"
check "reinicia una vez" "[ \"\$(reboots)\" -eq 1 ]"; check "se pinto la tarjeta" "grep -q 'start zenith-probe' $T/stub/splash"
check "solo cambia la linea FDT" "diff <(printf '%s' \"\$ORIG\") $T/flash/extlinux/extlinux.conf | grep -c '^[<>]' | grep -qx 2"
check "el original se conserva" "cmp -s $T/flash/dtbprobe/extlinux.orig <(printf '%s' \"\$ORIG\")"

echo "T3: con boton se queda el candidato y sigue el arranque"
setup a.dtb b.dtb c.dtb; printf '%s' "$ORIG" | sed 's#/a.dtb#/b.dtb#' >"$T/flash/extlinux/extlinux.conf"; echo 1 >"$T/flash/dtbprobe/index"; run 3
check "sale con 0" "[ $RC -eq 0 ]"; check "FDT sigue b.dtb" "[ \"\$(fdt)\" = /b.dtb ]"; check "no reinicia" "[ \"\$(reboots)\" -eq 0 ]"
check "borra el modo de prueba" "[ ! -e $T/flash/dtbprobe ]"; check "deja el resultado" "grep -q 'elegido: b.dtb' $T/flash/dtbprobe-last.txt"

echo "T4: agotados los candidatos restaura el original y reinicia"
setup a.dtb b.dtb c.dtb; printf '%s' "$ORIG" | sed 's#/a.dtb#/c.dtb#' >"$T/flash/extlinux/extlinux.conf"; echo 2 >"$T/flash/dtbprobe/index"; run 0
check "extlinux = original (a.dtb)" "[ \"\$(fdt)\" = /a.dtb ]"; check "igual byte a byte" "cmp -s $T/flash/extlinux/extlinux.conf <(printf '%s' \"\$ORIG\")"
check "reinicia" "[ \"\$(reboots)\" -eq 1 ]"; check "borra el modo de prueba" "[ ! -e $T/flash/dtbprobe ]"; check "resultado sin exito" "grep -q 'sin resultado' $T/flash/dtbprobe-last.txt"

echo "T5: si el usuario cambio extlinux a mano se cancela sin tocarlo"
setup a.dtb b.dtb c.dtb; printf '%s' "$ORIG" | sed 's#/a.dtb#/c.dtb#' >"$T/flash/extlinux/extlinux.conf"; run 0
check "FDT sigue c.dtb" "[ \"\$(fdt)\" = /c.dtb ]"; check "no reinicia" "[ \"\$(reboots)\" -eq 0 ]"; check "borra el modo de prueba" "[ ! -e $T/flash/dtbprobe ]"

echo "T6: nombres invalidos o inexistentes se descartan de la lista"
setup "../x.dtb" "mal nombre.dtb" "falta.dtb" "a.dtb" "b.dtb"; run 0
check "queda solo a,b: pasa a b.dtb" "[ \"\$(fdt)\" = /b.dtb ]"; check "no salio de la tarjeta" "[ ! -e $T/x.dtb ]"

echo "T7: tope de intentos"
setup a.dtb b.dtb c.dtb; echo 99 >"$T/flash/dtbprobe/attempts"; run 0
check "restaura el original" "cmp -s $T/flash/extlinux/extlinux.conf <(printf '%s' \"\$ORIG\")"; check "borra el modo de prueba" "[ ! -e $T/flash/dtbprobe ]"

echo "T8: extlinux con CRLF queda sin CR y con FDT correcto"
setup a.dtb b.dtb c.dtb; printf '%s' "$ORIG" | sed 's/$/\r/' >"$T/flash/extlinux/extlinux.conf"; run 0
check "FDT = b.dtb" "[ \"\$(fdt)\" = /b.dtb ]"; check "sin CR en el fichero" "! grep -q \$'\\r' $T/flash/extlinux/extlinux.conf"

echo "T9: sin zenith-splash sigue funcionando"
setup a.dtb b.dtb c.dtb; rm -f "$T/bin/zsplash"; run 0
check "avanza igualmente" "[ \"\$(fdt)\" = /b.dtb ]"; check "lo anota en el log" "grep -q 'no se pudo pintar' $T/flash/dtbprobe/log"

echo "T10: indice corrupto se trata como 0"
setup a.dtb b.dtb c.dtb; echo "basura;rm -rf /" >"$T/flash/dtbprobe/index"; run 0
check "avanza a b.dtb" "[ \"\$(fdt)\" = /b.dtb ]"

echo; echo "Resultado: $pass correctas, $fail fallidas"
[ "$fail" -eq 0 ]
