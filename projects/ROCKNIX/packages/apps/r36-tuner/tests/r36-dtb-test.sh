#!/bin/bash
# Pruebas simuladas de r36-dtb con un DTB REAL de la Ultra y fdtput/fdtget reales (no toca ninguna consola ni tarjeta).
# Cada "arranque" simulado tiene su propio boot_id (newboot): el ayudante es idempotente por arranque.
# Uso: bash r36-dtb-test.sh <r36-dtb> <rk3326-gameconsole-r36ultra.dtb> [otro.dtb]
HELPER="${1:?uso: r36-dtb-test.sh <r36-dtb> <dtb real> [otro dtb]}"
REAL="${2:?falta el DTB real}"
OTHER="${3:-$2}"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=0; fail=0
check() { if eval "$2"; then pass=$((pass+1)); echo "  PASS $1"; else fail=$((fail+1)); echo "  FAIL $1  [$2]"; fi; }
mkdir -p "$T/bin"; printf '#!/bin/bash\necho x >>"$STUB/reboots"\n' >"$T/bin/reboot"; chmod +x "$T/bin/reboot"
# a real sync takes long on a PC and proves nothing here: T33 checks the order of the writes
mkdir -p "$T/nosync"; printf '#!/bin/sh\nexit 0\n' >"$T/nosync/sync"; chmod +x "$T/nosync/sync"; export PATH="$T/nosync:$PATH"

ORIG='LABEL ROCKNIX
  LINUX /KERNEL
  ## nota que no debe tocarse
  FDT /rk3326-gameconsole-r36ultra.dtb
  APPEND boot=${partition_boot} quiet loglevel=1 console=ttyS2,1500000 uboot.hwid_adc=${hwid_adc}
'
BID=0
newboot() { BID=$((BID+1)); }
setup() {
  rm -rf "$T/flash" "$T/stub"; mkdir -p "$T/flash/extlinux" "$T/stub"
  printf '%s' "$ORIG" >"$T/flash/extlinux/extlinux.conf"; : >"$T/flash/KERNEL"
  cp "$REAL" "$T/flash/rk3326-gameconsole-r36ultra.dtb"; cp "$OTHER" "$T/flash/rk3326-otro.dtb"
  newboot
}
mkdir -p "$T/image"; cp "$REAL" "$T/image/rk3326-gameconsole-r36ultra.dtb"; cp "$OTHER" "$T/image/rk3326-otro.dtb"
r() { R36DTB_FLASH="$T/flash" R36DTB_REMOUNT="${R36DTB_REMOUNT:-0}" R36DTB_REBOOT="$T/bin/reboot" R36DTB_BOOTID="boot$BID" R36DTB_IMAGE="${R36DTB_IMAGE:-$T/image}" STUB="$T/stub" bash "$HELPER" "$@" >"$T/out.txt" 2>"$T/err.txt"; RC=$?; }
fdt() { awk '/^[ \t]*FDT[ \t]+/{print $2; exit}' "$T/flash/extlinux/extlinux.conf"; }
nreb() { if [ -f "$T/stub/reboots" ]; then wc -l <"$T/stub/reboots"; else echo 0; fi; }
sum() { sha256sum "$1" | cut -d' ' -f1; }
st() { cat "$T/flash/tuner/state" 2>/dev/null; }

echo "T1: path prepara tuned.dtb y la copia, sin tocar extlinux ni el de fabrica"
setup; F=$(sum "$T/flash/rk3326-gameconsole-r36ultra.dtb"); r path
check "rc 0 e imprime la ruta" "[ $RC -eq 0 ] && [ \"\$(cat $T/out.txt)\" = $T/flash/tuned.dtb ]"
check "tuned.dtb == fabrica" "cmp -s $T/flash/tuned.dtb $T/flash/rk3326-gameconsole-r36ultra.dtb"
check ".bak == fabrica" "cmp -s $T/flash/tuned.dtb.bak $T/flash/rk3326-gameconsole-r36ultra.dtb"
check "extlinux intacto" "[ \"\$(cat $T/flash/extlinux/extlinux.conf)\" = \"\$(printf '%s' \"\$ORIG\")\" ]"
check "registra la fabrica y su sha" "[ \"\$(cat $T/flash/tuner/factory)\" = rk3326-gameconsole-r36ultra.dtb ] && [ \"\$(cat $T/flash/tuner/factory.sha)\" = $F ]"
r path; check "es idempotente" "[ $RC -eq 0 ] && cmp -s $T/flash/tuned.dtb $T/flash/rk3326-gameconsole-r36ultra.dtb"

echo "T2: parchear tuned.dtb con fdtput NO cambia el de fabrica; la tension se lee de vuelta"
fdtput -t u "$T/flash/tuned.dtb" /opp-table-0/opp-1008000000 opp-microvolt-L1 1100000 1100000 1350000
fdtput -t u "$T/flash/tuned.dtb" /opp-table-1/opp-600000000 opp-microvolt-L1 1100000
check "CPU 1008 MHz L1 = 1100000" "[ \"\$(fdtget -t u $T/flash/tuned.dtb /opp-table-0/opp-1008000000 opp-microvolt-L1 | awk '{print \$1}')\" = 1100000 ]"
check "GPU 600 MHz L1 = 1100000" "[ \"\$(fdtget -t u $T/flash/tuned.dtb /opp-table-1/opp-600000000 opp-microvolt-L1)\" = 1100000 ]"
check "el DTB de fabrica sigue igual" "[ \"\$(sum $T/flash/rk3326-gameconsole-r36ultra.dtb)\" = $F ]"
check "la copia .bak sigue igual" "cmp -s $T/flash/tuned.dtb.bak $T/flash/rk3326-gameconsole-r36ultra.dtb"
r path; check "otra llamada a path NO pisa lo parcheado" "! cmp -s $T/flash/tuned.dtb $T/flash/rk3326-gameconsole-r36ultra.dtb"

echo "T3: pending arma la red de seguridad y solo cambia la linea FDT"
r pending
check "FDT = tuned.dtb" "[ \"\$(fdt)\" = /tuned.dtb ]"; check "estado pending" "[ \"\$(st)\" = pending ]"
check "solo cambia la linea FDT" "diff <(printf '%s' \"\$ORIG\") $T/flash/extlinux/extlinux.conf | grep -c '^[<>]' | grep -qx 2"
check "guarda extlinux con el DTB de fabrica" "cmp -s $T/flash/tuner/extlinux.conf.factory <(printf '%s' \"\$ORIG\")"

echo "T4: arranque confirmado: pending -> booting -> trial y se queda tuneado"
newboot; r boot-start; check "booting" "[ \"\$(st)\" = booting ]"; check "sin reinicio" "[ \"\$(nreb)\" -eq 0 ]"
r boot-start; check "una 2a llamada en el MISMO arranque no revierte" "[ \"\$(st)\" = booting ] && [ \"\$(nreb)\" -eq 0 ] && [ \"\$(fdt)\" = /tuned.dtb ]"
r boot-ok; check "trial" "[ \"\$(st)\" = trial ]"; check "sigue tuned.dtb" "[ \"\$(fdt)\" = /tuned.dtb ]"
newboot; r boot-start; check "un arranque normal posterior no hace nada" "[ \"\$(st)\" = trial ] && [ \"\$(nreb)\" -eq 0 ]"

echo "T5: arranque NO confirmado (cuelgue): en el SIGUIENTE arranque se restaura el de fabrica y se reinicia"
setup; r path; r pending; newboot; r boot-start          # pending -> booting
check "esta en booting" "[ \"\$(st)\" = booting ]"
newboot; r boot-start                                    # otro arranque: el anterior no se confirmo
check "FDT vuelve a fabrica" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ]"; check "estado failed" "[ \"\$(st)\" = failed ]"
check "reinicia una vez" "[ \"\$(nreb)\" -eq 1 ]"; check "extlinux = original byte a byte" "cmp -s $T/flash/extlinux/extlinux.conf <(printf '%s' \"\$ORIG\")"
newboot; r boot-start; check "tras failed no repite" "[ \"\$(nreb)\" -eq 1 ]"

echo "T6: revert deja todo como estaba"
setup; r path; r pending; r revert
check "FDT de fabrica" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ]"; check "sin tuned.dtb ni .bak" "[ ! -e $T/flash/tuned.dtb ] && [ ! -e $T/flash/tuned.dtb.bak ]"
check "extlinux = original" "cmp -s $T/flash/extlinux/extlinux.conf <(printf '%s' \"\$ORIG\")"; check "estado reverted" "[ \"\$(st)\" = reverted ]"

echo "T7: si el usuario cambia de modelo con otra herramienta, la copia vieja se descarta"
setup; r path; sed -i 's#/rk3326-gameconsole-r36ultra.dtb#/rk3326-otro.dtb#' "$T/flash/extlinux/extlinux.conf"; r path
check "nueva fabrica = rk3326-otro.dtb" "[ \"\$(cat $T/flash/tuner/factory)\" = rk3326-otro.dtb ]"
check "tuned.dtb es copia de la nueva" "cmp -s $T/flash/tuned.dtb $T/flash/rk3326-otro.dtb"

echo "T8: extlinux apunta a un DTB que no existe: falla sin crear nada"
setup; sed -i 's#/rk3326-gameconsole-r36ultra.dtb#/no-existe.dtb#' "$T/flash/extlinux/extlinux.conf"; r path
check "rc 1" "[ $RC -eq 1 ]"; check "no crea tuned.dtb" "[ ! -e $T/flash/tuned.dtb ]"

echo "T9: extlinux con CRLF sigue funcionando y queda sin CR"
setup; printf '%s' "$ORIG" | sed 's/$/\r/' >"$T/flash/extlinux/extlinux.conf"; r path; r pending
check "FDT = tuned.dtb" "[ \"\$(fdt)\" = /tuned.dtb ]"; check "sin CR" "! grep -q \$'\\r' $T/flash/extlinux/extlinux.conf"

echo "T10: sin estado no hace nada (boot-start / boot-ok)"
setup; r boot-start; r boot-ok
check "sin carpeta tuner" "[ ! -e $T/flash/tuner ]"; check "extlinux intacto" "cmp -s $T/flash/extlinux/extlinux.conf <(printf '%s' \"\$ORIG\")"

echo "T11: booting pero extlinux ya no apunta a tuned.dtb: no toca nada"
setup; r path; r pending; newboot; r boot-start; sed -i 's#/tuned.dtb#/rk3326-otro.dtb#' "$T/flash/extlinux/extlinux.conf"; cp "$T/flash/extlinux/extlinux.conf" "$T/before.conf"; newboot; r boot-start
check "extlinux sin cambios" "cmp -s $T/flash/extlinux/extlinux.conf $T/before.conf"; check "no reinicia" "[ \"\$(nreb)\" -eq 0 ]"

echo "T12: orden invalida"
setup; r nada; check "rc 2" "[ $RC -eq 2 ]"

echo "T13: una actualizacion cambia el DTB de fabrica: la copia de trabajo queda vieja y se descarta SIN dejar extlinux roto"
setup; r path; fdtput -t u "$T/flash/tuned.dtb" /opp-table-1/opp-600000000 opp-microvolt-L1 1100000; r pending; newboot; r boot-start; r boot-ok
fdtput -t u "$T/flash/rk3326-gameconsole-r36ultra.dtb" /opp-table-1/opp-200000000 opp-microvolt-L0 960000   # "update.sh" refresca el de fabrica
newboot; r boot-start
check "estado stale" "[ \"\$(st)\" = stale ]"; check "FDT vuelve a fabrica (existe)" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ] && [ -f $T/flash/rk3326-gameconsole-r36ultra.dtb ]"
check "la copia vieja se borra" "[ ! -e $T/flash/tuned.dtb ] && [ ! -e $T/flash/tuned.dtb.bak ]"; check "no reinicia" "[ \"\$(nreb)\" -eq 0 ]"
r path; check "path rehace la copia desde el nuevo de fabrica" "cmp -s $T/flash/tuned.dtb $T/flash/rk3326-gameconsole-r36ultra.dtb"

echo "T14: el DTB de fabrica cambia y el usuario vuelve a abrir el tuner (path) con tuned.dtb en uso"
setup; r path; r pending; newboot; r boot-start; r boot-ok
fdtput -t u "$T/flash/rk3326-gameconsole-r36ultra.dtb" /opp-table-1/opp-200000000 opp-microvolt-L0 960000
r path; check "path no deja extlinux roto" "[ -f $T/flash/\$(fdt) ] || [ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ]"
check "FDT existe en la FAT" "[ -f \"$T/flash/\$(fdt | sed 's#^/##')\" ]"

echo "T20: source no escribe nada y sigue a la copia de trabajo"
setup; before=$(cd "$T/flash" && find . -type f | sort | xargs sha256sum | sha256sum)
r source; check "source = fabrica" "grep -q '/flash/rk3326-gameconsole-r36ultra.dtb' $T/out.txt"
after=$(cd "$T/flash" && find . -type f | sort | xargs sha256sum | sha256sum)
check "source no cambia la FAT" "[ '$before' = '$after' ]"

echo "T21: install coloca la copia editada y arma la red"
setup; cp "$REAL" "$T/edit.dtb"; fdtput -t u "$T/edit.dtb" /opp-table-1/opp-600000000 opp-microvolt-L1 1087500
r install "$T/edit.dtb"; check "install rc 0" "[ $RC -eq 0 ]"; check "pending" "[ \"\$(st)\" = pending ]"; check "FDT tuned" "[ \"\$(fdt)\" = /tuned.dtb ]"
check "valor en tuned.dtb" "[ \"\$(fdtget -t u $T/flash/tuned.dtb /opp-table-1/opp-600000000 opp-microvolt-L1)\" = 1087500 ]"
check "fabrica intacto" "cmp -s $REAL $T/flash/rk3326-gameconsole-r36ultra.dtb"
r source; check "source = tuned" "grep -q '/flash/tuned.dtb' $T/out.txt"

echo "T22: install sin fichero no toca nada"
setup; r install "$T/nada.dtb"; check "install rc != 0" "[ $RC -ne 0 ]"; check "FDT de fabrica" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ]"

echo "T23: con el modo de prueba de DTB activo no instala"
setup; cp "$REAL" "$T/edit.dtb"; mkdir -p "$T/flash/dtbprobe"; echo 1 >"$T/flash/dtbprobe/state"
r install "$T/edit.dtb"; check "install rechazado" "[ $RC -ne 0 ]"; check "FDT de fabrica" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ]"

echo "T24: la prueba de estabilidad termina bien: trial -> testing -> ok"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; newboot; r boot-start; r boot-ok; check "trial" "[ \"\$(st)\" = trial ]"
r test-start; check "testing" "[ \"\$(st)\" = testing ]"; r confirm; check "ok" "[ \"\$(st)\" = ok ]"
newboot; r boot-start; check "sin marcha atras" "[ \"\$(fdt)\" = /tuned.dtb ] && [ \"\$(nreb)\" -eq 0 ]"

echo "T25: cuelgue DURANTE la prueba: el siguiente arranque vuelve a fabrica y reinicia"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; newboot; r boot-start; r boot-ok; r test-start
newboot; r boot-start; check "failed" "[ \"\$(st)\" = failed ]"; check "FDT de fabrica" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ]"; check "reinicia" "[ \"\$(nreb)\" -eq 1 ]"
r ack; check "ack quita el aviso" "[ ! -f $T/flash/tuner/state ]"

echo "T26b: parar la prueba de un cambio ya validado no le quita la validacion; con errores si"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; newboot; r boot-start; r boot-ok; r test-start; r confirm
r test-start; r test-end; check "sigue ok" "[ \"\$(st)\" = ok ]"
r test-start; r test-end failed; check "con errores, trial" "[ \"\$(st)\" = trial ]"
r test-start; r confirm; r confirm; check "confirmar dos veces vale" "[ $RC -eq 0 ] && [ \"\$(st)\" = ok ]"
echo "T26: parar la prueba no confirma"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; newboot; r boot-start; r boot-ok; r test-start; r test-end
check "vuelve a trial" "[ \"\$(st)\" = trial ]"

echo "T27: las copias estan en la FAT ANTES de cambiar FDT, y sin ellas no se instala"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"
check "pasos para el PC" "grep -q 'rk3326-gameconsole-r36ultra.dtb' $T/flash/R36-TUNER-RECOVERY.txt"
check "extlinux de recuperacion igual que el original" "cmp -s $T/flash/tuner/extlinux.conf.factory <(printf '%s' \"\$ORIG\")"
check "segunda copia del DTB" "cmp -s $T/flash/tuned.dtb.bak $REAL"
setup; cp "$REAL" "$T/edit.dtb"; mkdir -p "$T/flash/tuner"; mkdir "$T/flash/tuned.dtb.bak"
r install "$T/edit.dtb"; check "si la copia falla, no instala" "[ $RC -ne 0 ]"; check "FDT sigue de fabrica" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ]"
rmdir "$T/flash/tuned.dtb.bak"

echo "T28: cuelgue ANTES de la red: nada se revierte solo, y la copia desde el PC lo arregla"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"
check "sigue en tuned.dtb" "[ \"\$(fdt)\" = /tuned.dtb ]"; check "estado pending" "[ \"\$(st)\" = pending ]"
cp "$T/flash/tuner/extlinux.conf.factory" "$T/flash/extlinux/extlinux.conf"
newboot; r boot-start; check "FDT de fabrica" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ]"; check "no reinicia" "[ \"\$(nreb)\" -eq 0 ]"; check "avisa: failed" "[ \"\$(st)\" = failed ]"
r boot-ok; check "no pasa a trial" "[ \"\$(st)\" = failed ]"
r source; check "edita desde fabrica" "grep -q 'rk3326-gameconsole-r36ultra.dtb' $T/out.txt"

echo "T29: la copia de extlinux se rehace: lleva el APPEND de hoy, no el de la primera vez"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; sed -i 's/quiet/quiet nuevo=1/' "$T/flash/extlinux/extlinux.conf"; r install "$T/edit.dtb"
check "APPEND nuevo" "grep -q 'nuevo=1' $T/flash/tuner/extlinux.conf.factory"; check "FDT de fabrica" "grep -q 'FDT /rk3326-gameconsole-r36ultra.dtb' $T/flash/tuner/extlinux.conf.factory"

echo "T30: restaurar desde el tuner quita los ficheros de recuperacion; el reset desde el PC se consume"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; r revert
check "sin pasos" "[ ! -f $T/flash/R36-TUNER-RECOVERY.txt ]"; check "fabrica intacto" "cmp -s $T/flash/rk3326-gameconsole-r36ultra.dtb $REAL"
: >"$T/flash/r36-tuner-reset.txt"; r reset-done; check "reset borrado" "[ ! -f $T/flash/r36-tuner-reset.txt ]"

echo "T31: un corte deja el estado vacio con tuned.dtb puesto: vuelve al de fabrica, y sin bucle"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; newboot; r boot-start; r boot-ok; : >"$T/flash/tuner/state"
newboot; r boot-start
check "FDT de fabrica" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ]"; check "un reinicio" "[ \"\$(nreb)\" -eq 1 ]"; check "aviso" "[ \"\$(st)\" = failed ]"
newboot; r boot-start; check "sin bucle" "[ \"\$(nreb)\" -eq 1 ]"

echo "T32: lo mismo sin fichero de estado, y con un estado desconocido"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; rm -f "$T/flash/tuner/state"
newboot; r boot-start; check "sin fichero: de fabrica" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ] && [ \"\$(nreb)\" -eq 1 ]"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; echo xx >"$T/flash/tuner/state"
newboot; r boot-start; check "desconocido: de fabrica" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ] && [ \"\$(nreb)\" -eq 1 ]"

echo "T33: pending ya esta escrito cuando cambia lo que arrancara, tambien al volver a ajustar"
mkdir -p "$T/mv"; printf '#!/bin/bash\nfor a; do d=$a; done\ncase "$d" in */tuned.dtb|*/extlinux.conf) echo "$(basename "$d") $(cat "$T_FLASH/tuner/state" 2>/dev/null)" >>"$T_LOG";; esac\nexec /bin/mv "$@"\n' >"$T/mv/mv"; chmod +x "$T/mv/mv"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; newboot; r boot-start; r boot-ok; r test-start; r confirm
check "de partida: ok" "[ \"\$(st)\" = ok ]"
: >"$T/mv.log"; PATH="$T/mv:$PATH" T_FLASH="$T/flash" T_LOG="$T/mv.log" r install "$T/edit.dtb"
check "tuned.dtb cambia con pending" "grep -q '^tuned.dtb pending' $T/mv.log"; check "nunca sin pending" "! grep -qv ' pending$' $T/mv.log"
setup; cp "$REAL" "$T/edit.dtb"; : >"$T/mv.log"; PATH="$T/mv:$PATH" T_FLASH="$T/flash" T_LOG="$T/mv.log" r install "$T/edit.dtb"
check "primera vez: nunca sin pending" "[ -s $T/mv.log ] && ! grep -qv ' pending$' $T/mv.log"

echo "T34: si no se puede restaurar no dice que lo ha hecho, y lo reintenta en el siguiente arranque"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; newboot; r boot-start
chmod 555 "$T/flash/extlinux"; newboot; r boot-start
check "sin reinicio" "[ \"\$(nreb)\" -eq 0 ]"; check "sigue sin confirmar" "[ \"\$(st)\" = booting ]"; check "sigue tuned" "[ \"\$(fdt)\" = /tuned.dtb ]"
chmod 755 "$T/flash/extlinux"; newboot; r boot-start
check "reintenta y restaura" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ] && [ \"\$(nreb)\" -eq 1 ] && [ \"\$(st)\" = failed ]"

echo "T35: sin poder montar /flash en escritura no se instala ni se dice que se ha hecho"
mkdir -p "$T/mnt"; printf '#!/bin/bash\nexit 1\n' >"$T/mnt/mount"; chmod +x "$T/mnt/mount"
r1() { PATH="$T/mnt:$PATH" R36DTB_FLASH="$T/flash" R36DTB_REMOUNT=1 R36DTB_REBOOT="$T/bin/reboot" R36DTB_BOOTID="boot$BID" bash "$HELPER" "$@" >"$T/out.txt" 2>"$T/err.txt"; RC=$?; }
setup; cp "$REAL" "$T/edit.dtb"; r1 install "$T/edit.dtb"
check "install falla" "[ $RC -ne 0 ]"; check "FDT intacto" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ]"; check "sin estado" "[ ! -f $T/flash/tuner/state ]"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; newboot; r boot-start; r boot-ok; r1 test-start; check "test-start falla" "[ $RC -ne 0 ] && [ \"\$(st)\" = trial ]"
r1 confirm; check "confirm falla" "[ $RC -ne 0 ] && [ \"\$(st)\" = trial ]"

echo "T36: test-start y confirm no dicen que si cuando no toca"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"
r test-start; check "test-start con pending falla" "[ $RC -ne 0 ] && [ \"\$(st)\" = pending ]"
r confirm; check "confirm con pending falla" "[ $RC -ne 0 ] && [ \"\$(st)\" = pending ]"
newboot; r boot-start; r boot-ok; r test-start; r test-end; r confirm; check "tras la prueba, confirm vale" "[ $RC -eq 0 ] && [ \"\$(st)\" = ok ]"

echo "T37: el DTB de fabrica se comprueba contra la copia intacta de la imagen"
IMG="$T/img"; mkdir -p "$IMG"; cp "$REAL" "$IMG/rk3326-gameconsole-r36ultra.dtb"
setup; cp "$REAL" "$T/edit.dtb"; R36DTB_IMAGE="$IMG" r install "$T/edit.dtb"
check "el de la imagen se acepta" "[ $RC -eq 0 ] && [ \"\$(fdt)\" = /tuned.dtb ]"
setup; cp "$REAL" "$T/edit.dtb"; sed -i 's#/rk3326-gameconsole-r36ultra.dtb#/rk3326-otro.dtb#' "$T/flash/extlinux/extlinux.conf"; R36DTB_IMAGE="$IMG" r install "$T/edit.dtb"
check "uno que la imagen no trae se rechaza" "[ $RC -eq 3 ] && [ \"\$(fdt)\" = /rk3326-otro.dtb ] && [ ! -f $T/flash/tuner/factory ]"
setup; cp "$REAL" "$T/edit.dtb"; printf x >>"$T/flash/rk3326-gameconsole-r36ultra.dtb"; R36DTB_IMAGE="$IMG" r install "$T/edit.dtb"
check "el mismo nombre con otro contenido se rechaza" "[ $RC -eq 3 ] && [ ! -f $T/flash/tuned.dtb ]"
setup; cp "$REAL" "$T/edit.dtb"; R36DTB_IMAGE="$T/no-existe" r install "$T/edit.dtb"
check "sin copia en la imagen no se acepta: no se puede saber que es de fabrica" "[ $RC -eq 3 ] && [ ! -f $T/flash/tuned.dtb ]"

echo "T38: sin el fichero factory se usa el de recuperacion; sin ninguno, revert no dice que ha restaurado"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; newboot; r boot-start; rm -f "$T/flash/tuner/factory"
newboot; r boot-start; check "restaura con extlinux.conf.factory" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ] && [ \"\$(nreb)\" -eq 1 ]"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; rm -f "$T/flash/tuner/factory" "$T/flash/tuner/extlinux.conf.factory"
r revert; check "revert falla" "[ $RC -ne 0 ] && [ \"\$(fdt)\" = /tuned.dtb ]"

echo "T39: /flash que no vuelve a solo lectura: se reintenta, el trabajo queda y se apunta"
mkdir -p "$T/mnt"; printf '#!/bin/bash\nn=$(cat "$STUB/mounts" 2>/dev/null || echo 0); echo $((n+1)) >"$STUB/mounts"\ncase "$*" in *remount,ro*) [ -f "$STUB/busy" ] && exit 1;; esac\nexit 0\n' >"$T/mnt/mount"; chmod +x "$T/mnt/mount"
printf '#!/bin/sh\nexit 0\n' >"$T/mnt/sleep"; chmod +x "$T/mnt/sleep"
setup; cp "$REAL" "$T/edit.dtb"; : >"$T/stub/busy"; OLDP="$PATH"; PATH="$T/mnt:$PATH"; R36DTB_REMOUNT=1 r install "$T/edit.dtb"; PATH="$OLDP"
check "instalado" "[ $RC -eq 0 ] && [ \"\$(fdt)\" = /tuned.dtb ]"
check "reintentado (1 rw, 3 ro)" "[ \"\$(cat $T/stub/mounts)\" -eq 4 ]"
check "queda en el registro" "grep -q 'sigue en escritura' $T/flash/tuner/log"

echo "T40: la unidad se activa con el estado o con tuned.dtb, cualquiera de los dos"
U="$(dirname "$HELPER")/r36-dtb-boot.service"
check "dos condiciones de tipo cualquiera" "[ \"\$(grep -c '^ConditionPathExists=|' '$U')\" -eq 2 ] && grep -q '^ConditionPathExists=|/flash/tuner/state$' '$U' && grep -q '^ConditionPathExists=|/flash/tuned.dtb$' '$U'"
check "ninguna obligatoria" "! grep -q '^ConditionPathExists=[^|]' '$U'"
setup; cp "$REAL" "$T/edit.dtb"; r install "$T/edit.dtb"; rm -f "$T/flash/tuner/state"; newboot; r boot-start
check "sin estado y con tuned.dtb puesto: vuelve al de fabrica" "[ \"\$(fdt)\" = /rk3326-gameconsole-r36ultra.dtb ] && [ \"\$(nreb)\" -eq 1 ]"

echo; echo "Resultado: $pass correctas, $fail fallidas"; [ "$fail" -eq 0 ]
