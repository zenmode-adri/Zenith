#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Ejecuta sources/scripts/wifictl con iwctl/connmanctl/iw/sleep falsos: rechazo de SSID/clave con salto de linea y
# SSID que empieza por "-" protegido con "--". Usage: wifictl-test.sh [ruta de wifictl]
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="${1:-${HERE}/../sources/scripts/wifictl}"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0
check() { if [ "$2" = "$3" ]; then OK=$((OK+1)); echo "  PASS $1"; else BAD=$((BAD+1)); echo "  FAIL $1"; printf '    esperado: %q\n    obtenido: %q\n' "$2" "$3"; fi; }
mkdir -p "${T}/bin"
# copia con el entorno falso: sin /etc/profile, dispositivo wlan0 ficticio, get_setting desde variables FAKE_*
sed -e 's#^\. /etc/profile$#get_setting() { case "$1" in wifi.ssid) echo "${FAKE_SSID}";; wifi.key) echo "${FAKE_PSK}";; esac; }#' \
    -e 's#^WIFI_DEV=.*#WIFI_DEV=wlan0#' \
    -e 's#^\[ ! -d "/sys/class/net/${WIFI_DEV}" \] && exit 0#:#' "${SCRIPT}" >"${T}/wifictl"
printf '#!/bin/bash\nif [ "$1 $2" = "device list" ]; then echo wlan0; fi\nprintf "ARGS:" >>"%s/calls"; printf " [%%s]" "$@" >>"%s/calls"; echo >>"%s/calls"\n' "${T}" "${T}" "${T}" >"${T}/bin/iwctl"
for c in connmanctl iw rfkill; do printf '#!/bin/bash\nexit 1\n' >"${T}/bin/${c}"; done
printf '#!/bin/bash\n:\n' >"${T}/bin/sleep"
chmod +x "${T}/bin/"*

run() { : >"${T}/calls"; PATH="${T}/bin:${PATH}" FAKE_SSID="$1" FAKE_PSK="$2" bash "${T}/wifictl" connect >/dev/null 2>&1; echo $?; }
connect_calls() { grep 'connect' "${T}/calls" | grep -v 'device\|Mode'; }

rc="$(run "MiRed" "clave1234")"
check "normal: conecta" "ARGS: [--passphrase] [clave1234] [--] [station] [wlan0] [connect] [MiRed]" "$(connect_calls)"
rc="$(run "-oculta" "clave1234")"
check "SSID que empieza por -: tras --" "ARGS: [--passphrase] [clave1234] [--] [station] [wlan0] [connect] [-oculta]" "$(connect_calls)"
rc="$(run $'Mi\nRed' "clave1234")"
check "SSID con salto de linea: rechazado, sin llamar a iwctl" "1|" "${rc}|$(cat "${T}/calls")"
rc="$(run $'Mi\rRed' "clave1234")"
check "SSID con retorno de carro: rechazado" "1|" "${rc}|$(cat "${T}/calls")"
rc="$(run "MiRed" $'cla\nve')"
check "clave con salto de linea: rechazada" "1|" "${rc}|$(cat "${T}/calls")"
rc="$(run "Mi Red/5G" "clave1234")"
check "SSID con espacio y barra en modo estacion: se permite" "ARGS: [--passphrase] [clave1234] [--] [station] [wlan0] [connect] [Mi Red/5G]" "$(connect_calls)"
echo "  (${OK} passed, ${BAD} failed)"
[ "${BAD}" = 0 ]
