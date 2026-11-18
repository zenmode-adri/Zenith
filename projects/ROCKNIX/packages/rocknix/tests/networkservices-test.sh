#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Ejecuta autostart/099-networkservices de verdad con daemons y systemctl falsos: el marcador de servicio nunca desaparece
# si el servicio esta activo (vigila el directorio mientras se ejecuta) y se borra si esta apagado.
# Uso: networkservices-test.sh [ruta de 099-networkservices]
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="${1:-${HERE}/../autostart/099-networkservices}"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0
check() { if [ "$2" = "$3" ]; then OK=$((OK+1)); echo "  PASS $1"; else BAD=$((BAD+1)); echo "  FAIL $1: esperado '$2', obtenido '$3'"; fi; }
mkdir -p "${T}/bin" "${T}/daemons" "${T}/services"
# El guion trae rutas absolutas: se reescriben a la carpeta temporal en una copia
sed -e "s#/usr/lib/autostart/daemons#${T}/daemons#g" -e "s#/storage/.cache/services#${T}/services#g" \
    -e "s#^\. /etc/profile.d/001-functions#tocon() { :; }#" -e 's#nohup systemctl#systemctl#' "${SCRIPT}" >"${T}/run.sh"
printf '#!/bin/bash\n:\n' >"${T}/bin/systemctl"; chmod +x "${T}/bin/systemctl"
printf 'STATE=${FAKE_SSH}\nCONF="sshd.conf"\nDAEMONS=("sshd")\n' >"${T}/daemons/001-ssh"

# Registra cada instante en que el marcador no existe, con un "strace" casero: cambiamos rm/touch por envoltorios que anotan
# el estado del marcador justo despues de cada operacion.
for c in rm touch; do
  printf '#!/bin/bash\n/usr/bin/%s "$@"\n[ -e "%s/services/sshd.conf" ] && echo present >>"%s/trace" || echo absent >>"%s/trace"\n' "$c" "${T}" "${T}" "${T}" >"${T}/bin/$c"
  chmod +x "${T}/bin/$c"
done

: >"${T}/services/sshd.conf"; : >"${T}/trace"
PATH="${T}/bin:${PATH}" FAKE_SSH=1 bash "${T}/run.sh" >/dev/null 2>&1
check "ssh activo: el marcador existe al final" "yes" "$([ -e "${T}/services/sshd.conf" ] && echo yes || echo no)"
check "ssh activo: nunca hay un instante sin marcador" "0" "$(grep -c absent "${T}/trace")"
rm -f "${T}/services/sshd.conf"
PATH="${T}/bin:${PATH}" FAKE_SSH=1 bash "${T}/run.sh" >/dev/null 2>&1
check "ssh activo y sin marcador: se crea" "yes" "$([ -e "${T}/services/sshd.conf" ] && echo yes || echo no)"
: >"${T}/services/sshd.conf"
PATH="${T}/bin:${PATH}" FAKE_SSH=0 bash "${T}/run.sh" >/dev/null 2>&1
check "ssh apagado: el marcador se borra" "no" "$([ -e "${T}/services/sshd.conf" ] && echo yes || echo no)"
echo "  (${OK} passed, ${BAD} failed)"
[ "${BAD}" = 0 ]
