#!/bin/bash
# Integracion PC<->consola por simulacion: el selector (Go) activa el modo de prueba y el script real de la consola
# (dtb-probe.sh) recorre el ciclo con un "boton" simulado. No toca ninguna consola ni tarjeta.
SEL="${1:-./dist/zenith-dtb-selector-linux-amd64}"
SCRIPT="${2:-$(dirname "$0")/../../../../../../packages/sysutils/autostart/sources/firstboot/dtb-probe.sh}"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=0; fail=0
check() { if eval "$2"; then pass=$((pass+1)); echo "  PASS $1"; else fail=$((fail+1)); echo "  FAIL $1  [$2]"; fi; }
mkdir -p "$T/bin" "$T/in" "$T/stub"
cat >"$T/bin/evtest" <<'EOS'
#!/bin/bash
n=$(cat "$STUB/polls" 2>/dev/null || echo 0); n=$((n+1)); echo $n >"$STUB/polls"
pa=$(cat "$STUB/press_after" 2>/dev/null || echo 0)
[ "$pa" -gt 0 ] && [ "$n" -ge "$pa" ] && exit 10
exit 0
EOS
printf '#!/bin/bash\necho x >>"$STUB/reboots"\n' >"$T/bin/reboot"; printf '#!/bin/bash\n' >"$T/bin/zsplash"; chmod +x "$T/bin/"*
: >"$T/in/platform-x-event-joystick"
sed -e 's/sleep 60/:/g' "$SCRIPT" >"$T/probe.sh"

card() {
  rm -rf "$T/card"; mkdir -p "$T/card/extlinux"; : >"$T/card/KERNEL"
  printf 'LABEL ROCKNIX\n  LINUX /KERNEL\n  FDT /rk3326-gameconsole-r36ultra.dtb\n  APPEND boot=${partition_boot} quiet loglevel=1 console=ttyS2,1500000 uboot.hwid_adc=${hwid_adc}\n' >"$T/card/extlinux/extlinux.conf"
  echo "dtb-probe=1" >"$T/card/zenith-features.txt"
  for d in rk3326-gameconsole-r36ultra a b c d; do head -c 200 /dev/zero >"$T/card/$d.dtb"; done
  cp "$T/card/extlinux/extlinux.conf" "$T/orig.conf"
}
boot() {   # un arranque de la consola con el modo de prueba; $1 = tras cuantas lecturas "se pulsa" (0 = nunca)
  echo "${1:-0}" >"$T/stub/press_after"; rm -f "$T/stub/polls"
  DTBPROBE_FLASH="$T/card" DTBPROBE_TIMEOUT=2 DTBPROBE_POLL=0.05 DTBPROBE_IGNORE_FIRST=0 DTBPROBE_REMOUNT=0 \
  DTBPROBE_REBOOT="$T/bin/reboot" DTBPROBE_EVTEST="$T/bin/evtest" DTBPROBE_INPUT_DIR="$T/in" ZSPLASH="$T/bin/zsplash" \
  STUB="$T/stub" bash "$T/probe.sh" >/dev/null 2>&1
}
fdt() { awk '/^[ \t]*FDT[ \t]+/{print $2; exit}' "$T/card/extlinux/extlinux.conf"; }
nreb() { if [ -f "$T/stub/reboots" ]; then wc -l <"$T/stub/reboots"; else echo 0; fi; }

echo "I1: activar desde el PC, recorrer y confirmar en el 3er candidato"
card; "$SEL" --boot "$T/card" --probe-start --probe-list a.dtb,b.dtb,c.dtb,d.dtb | head -1
check "FDT = a.dtb" "[ \"\$(fdt)\" = /a.dtb ]"
check "estado activo 1 de 4" "$SEL --boot $T/card --probe-status | grep -q '\"active\":true.*\"total\":4'"
boot 0; check "tras 1er arranque FDT = b.dtb" "[ \"\$(fdt)\" = /b.dtb ]"
boot 0; check "tras 2o arranque FDT = c.dtb" "[ \"\$(fdt)\" = /c.dtb ]"
boot 3; check "con boton se queda c.dtb" "[ \"\$(fdt)\" = /c.dtb ]"
check "el PC ya no lo ve activo" "$SEL --boot $T/card --probe-status | grep -q '\"active\":false'"
check "el PC ve el resultado" "$SEL --boot $T/card --probe-status | grep -q 'elegido: c.dtb'"
check "solo cambio la linea FDT" "diff <(sed 's#/c.dtb#/rk3326-gameconsole-r36ultra.dtb#' $T/card/extlinux/extlinux.conf) $T/orig.conf >/dev/null"

echo "I2: nadie pulsa: tras agotar la lista queda el original"
card; "$SEL" --boot "$T/card" --probe-start --probe-list a.dtb,b.dtb,c.dtb >/dev/null
boot 0; boot 0; boot 0
check "extlinux = original byte a byte" "cmp -s $T/card/extlinux/extlinux.conf $T/orig.conf"
check "modo de prueba borrado" "[ ! -e $T/card/dtbprobe ]"

echo "I3: cancelar desde el PC a mitad"
card; "$SEL" --boot "$T/card" --probe-start --probe-list a.dtb,b.dtb,c.dtb >/dev/null; boot 0
check "va por b.dtb" "[ \"\$(fdt)\" = /b.dtb ]"
"$SEL" --boot "$T/card" --probe-stop >/dev/null
check "original restaurado" "cmp -s $T/card/extlinux/extlinux.conf $T/orig.conf"
check "ya no hay modo de prueba" "[ ! -e $T/card/dtbprobe ]"
boot 0; check "un arranque posterior no hace nada" "cmp -s $T/card/extlinux/extlinux.conf $T/orig.conf"

echo "I4: el orden recomendado se filtra por lo que hay en la tarjeta"
card; for d in rk3326-xifan-r36pro rk3326-r36s-type3-panel1 rk3326-r36s-type4; do head -c 200 /dev/zero >"$T/card/$d.dtb"; done
"$SEL" --boot "$T/card" --probe-start | head -1
check "usa los 3 que existen, en orden" "[ \"\$(cat $T/card/dtbprobe/list | tr '\n' ' ')\" = 'rk3326-xifan-r36pro.dtb rk3326-r36s-type3-panel1.dtb rk3326-r36s-type4.dtb ' ]"
check "doble activacion rechazada" "! $SEL --boot $T/card --probe-start >/dev/null 2>&1"

echo "I5: una imagen antigua (sin la marca) rechaza el modo de prueba y no toca nada"
card; rm -f "$T/card/zenith-features.txt"
check "se rechaza" "! $SEL --boot $T/card --probe-start --probe-list a.dtb,b.dtb >/dev/null 2>&1"
check "extlinux intacto" "cmp -s $T/card/extlinux/extlinux.conf $T/orig.conf"; check "sin carpeta dtbprobe" "[ ! -e $T/card/dtbprobe ]"
check "con --probe-force si se activa" "$SEL --boot $T/card --probe-start --probe-force --probe-list a.dtb,b.dtb >/dev/null"

echo; echo "Resultado: $pass correctas, $fail fallidas"; [ "$fail" -eq 0 ]
