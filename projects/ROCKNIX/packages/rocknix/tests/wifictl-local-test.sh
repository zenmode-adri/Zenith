#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Vuelta del "modo de juego local" (AP) a estacion, con iwctl/connmanctl/rfkill falsos y CON ESTADO (modo, encendido,
# retraso de encendido tras rfkill unblock, como la RK915). Comprueba las dos ramas (iwd y connman).
# Usage: wifictl-local-test.sh [ruta de wifictl]
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="${1:-${HERE}/../sources/scripts/wifictl}"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0
check() { if [ "$2" = "$3" ]; then OK=$((OK+1)); echo "  PASS $1"; else BAD=$((BAD+1)); echo "  FAIL $1"; printf '    esperado: %q\n    obtenido: %q\n' "$2" "$3"; fi; }
mkdir -p "${T}/bin" "${T}/ap" "${T}/st"
sed -e 's#^\. /etc/profile$#get_setting() { case "$1" in wifi.ssid) echo "${FAKE_SSID}";; wifi.key) echo "${FAKE_PSK}";; wifi.adhoc.enabled) echo "${FAKE_ADHOC}";; wifi.adhoc.id) echo "${FAKE_ID}";; wifi.country) echo ES;; esac; }; set_setting() { :; }#' \
    -e 's#^WIFI_DEV=.*#WIFI_DEV=wlan0#' \
    -e "s#^IWD_AP_CFG_DIR=.*#IWD_AP_CFG_DIR=${T}/ap#" \
    -e 's#^\[ ! -d "/sys/class/net/${WIFI_DEV}" \] && exit 0#:#' "${SCRIPT}" >"${T}/wifictl"
# iwctl con estado en ${FAKE_ST}: mode (station|ap), powered (on|off), warm (n.o de ordenes de set-property que iwd
# rechaza todavia tras `rfkill unblock`: el chip esta encendiendo, como la RK915)
cat >"${T}/bin/iwctl" <<'EOF'
#!/bin/bash
ST="${FAKE_ST}"; echo "iwctl $*" >>"${ST}/calls"
[ "$1" = "--passphrase" ] && { echo "pass=$2" >>"${ST}/calls"; shift 2; }
[ "$1" = "--" ] && shift
mode=$(cat "${ST}/mode"); pw=$(cat "${ST}/powered")
case "$1 $2 $3" in
  "device list "*) echo "wlan0" ;;
  "device wlan0 show") echo "Mode      ${mode}"; echo "Powered   ${pw}" ;;
  "device wlan0 set-property")
    w=$(cat "${ST}/warm"); if [ "${w}" -gt 0 ]; then echo $((w-1)) >"${ST}/warm"; exit 1; fi
    case "$4" in Mode) echo "$5" >"${ST}/mode" ;; Powered) echo "$5" >"${ST}/powered" ;; esac ;;
  "ap wlan0 stop") exit 0 ;;
  "ap wlan0 start-profile") echo "$4" >"${ST}/apstarted" ;;
  "station wlan0 show") [ "${mode}" = station ] && [ "${pw}" = on ] ;;
  "station wlan0 connect") if [ "${mode}" = station ]; then echo "$4" >"${ST}/joined"; else exit 1; fi ;;
  *) exit 0 ;;
esac
EOF
cat >"${T}/bin/connmanctl" <<'EOF'
#!/bin/bash
echo "connmanctl $*" >>"${FAKE_ST}/calls"
if [ "${FAKE_CONNMAN}" = 1 ]; then case "$1" in technologies) echo "/net/connman/technology/wifi";; services) echo "*AO MiRed wifi_aa_bb_managed_psk";; esac; fi
exit 0
EOF
cat >"${T}/bin/rfkill" <<'EOF'
#!/bin/bash
echo "rfkill $*" >>"${FAKE_ST}/calls"
if [ "$1" = unblock ]; then echo "${FAKE_WARM:-3}" >"${FAKE_ST}/warm"; echo on >"${FAKE_ST}/powered"; fi
[ "$1" = block ] && echo off >"${FAKE_ST}/powered"
exit 0
EOF
printf '#!/bin/bash\nshift; exec "$@"\n' >"${T}/bin/timeout"
printf '#!/bin/bash\nexit 0\n' >"${T}/bin/iw"
printf '#!/bin/bash\n:\n' >"${T}/bin/sleep"
chmod +x "${T}/bin/"*
reset() { echo "$1" >"${T}/st/mode"; echo on >"${T}/st/powered"; echo 0 >"${T}/st/warm"; : >"${T}/st/calls"; rm -f "${T}/st/joined" "${T}/st/apstarted"; }
# W adhoc id orden...: ejecuta wifictl con el entorno falso (FAKE_PSK, FAKE_SSID, FAKE_CONNMAN, FAKE_WARM, STATION_TRIES por entorno)
W() {
  local a="$1" i="$2"; shift 2
  PATH="${T}/bin:${PATH}" FAKE_ST="${T}/st" FAKE_ADHOC="$a" FAKE_ID="$i" FAKE_SSID="${FAKE_SSID-MiRed}" FAKE_PSK="${FAKE_PSK-}" bash "${T}/wifictl" "$@" >/dev/null 2>&1
}
export FAKE_CONNMAN=0 FAKE_WARM=3

echo "== rama iwd =="
# 1. flujo de zfront al DESACTIVAR: estaba en AP; disable + enable + connect; el chip tarda 3 ordenes en aceptar Mode
reset ap; touch "${T}/ap/NETPLAY_AP.ap" "${T}/ap/MiRed.ap"
W 0 1 disable; W 0 1 enable; W 0 1 connect MiRed "" ES
check "iwd: tras salir del AP el modo es station" "station" "$(cat "${T}/st/mode")"
check "iwd: se une a la red de casa pese a la espera del chip" "MiRed" "$(cat "${T}/st/joined" 2>/dev/null)"
check "iwd: borra el perfil NETPLAY_AP.ap (el que de verdad se creo)" "no" "$([ -e "${T}/ap/NETPLAY_AP.ap" ] && echo si || echo no)"
check "iwd: clave vacia: sin --passphrase" "0" "$(grep -c -- '--passphrase' "${T}/st/calls")"
# 2. chip muy lento (12 ordenes rechazadas): entra el ciclo Powered off / Mode / on
reset ap; FAKE_WARM=12 W 0 1 disable; FAKE_WARM=12 W 0 1 enable; FAKE_WARM=12 STATION_TRIES=30 W 0 1 connect MiRed "" ES
check "iwd: con encendido lento tambien vuelve a station" "station" "$(cat "${T}/st/mode")"
# 3. con clave
reset ap; FAKE_PSK=secreta1 W 0 1 connect MiRed secreta1 ES
check "iwd: con clave usa --passphrase" "1" "$(grep -c 'pass=secreta1' "${T}/st/calls")"
# 4. solo enable (reanudacion) con el chip aun en AP y juego local apagado
reset ap; W 0 1 enable
check "iwd: enable con AP sobrante y adhoc=0 vuelve a station" "station" "$(cat "${T}/st/mode")"
# 5. enable con adhoc=1 NO toca el modo
reset ap; W 1 1 enable
check "iwd: enable con adhoc=1 deja el AP" "ap" "$(cat "${T}/st/mode")"
# 6. activar el modo local (anfitrion)
reset station; rm -f "${T}/ap/"*; W 1 1 connect MiRed "" ES
check "iwd: anfitrion arranca NETPLAY_AP" "NETPLAY_AP" "$(cat "${T}/st/apstarted" 2>/dev/null)"
check "iwd: anfitrion pone modo ap" "ap" "$(cat "${T}/st/mode")"
# 7. SSID de casa con barra
reset ap; W 0 1 connect "Casa/5G" "" ES
check "iwd: SSID con barra: sale del AP" "station" "$(cat "${T}/st/mode")"
check "iwd: SSID con barra: se une" "Casa/5G" "$(cat "${T}/st/joined" 2>/dev/null)"
# 8. el chip no sale nunca del AP: no se une y no se cuelga
reset ap; echo 999 >"${T}/st/warm"; STATION_TRIES=3 W 0 1 connect MiRed "" ES
check "iwd: sin recuperar el chip no finge unirse" "no" "$([ -e "${T}/st/joined" ] && echo si || echo no)"

echo "== rama connman =="
reset ap; FAKE_CONNMAN=1 W 0 1 connect MiRed "" ES
check "connman: sale del modo AP antes de conectar" "station" "$(cat "${T}/st/mode")"
check "connman: ejecuta connmanctl connect" "1" "$(grep -c 'connmanctl connect' "${T}/st/calls")"
reset ap; FAKE_CONNMAN=1 W 0 1 enable
check "connman: enable no usa iwctl" "0" "$(grep -c '^iwctl' "${T}/st/calls")"
echo "  (${OK} passed, ${BAD} failed)"
[ "${BAD}" = 0 ]
