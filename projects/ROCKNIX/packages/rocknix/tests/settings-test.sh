#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Prueba set_setting / del_setting / copy_dir_atomic de profile.d/001-functions con ficheros falsos en un directorio temporal.
# Uso: settings-test.sh [ruta de 001-functions]   (por defecto, la del arbol; con la version vieja de git los casos nuevos FALLAN)
HERE="$(cd "$(dirname "$0")" && pwd)"
FUNCS="${1:-${HERE}/../profile.d/001-functions}"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0
# sync falso por defecto (el real espera al disco de toda la maquina); el caso de orden pone el suyo
mkdir -p "${T}/nosync"; printf '#!/bin/bash\n:\n' >"${T}/nosync/sync"; chmod +x "${T}/nosync/sync"; PATH="${T}/nosync:${PATH}"
check() { # nombre esperado obtenido
  if [ "$2" = "$3" ]; then OK=$((OK+1)); echo "  PASS $1"
  else BAD=$((BAD+1)); echo "  FAIL $1"; printf '    esperado: %q\n    obtenido: %q\n' "$2" "$3"; fi
}
# Solo las funciones que interesan (el fichero entero hace cosas de arranque: . /etc/os-release, PATH...)
for fn in wait_lock copy_dir_atomic _settings_drop_key del_setting set_setting; do
  sed -n "/^function ${fn}() {/,/^}/p" "${FUNCS}"
done >"${T}/funcs.sh"
# shellcheck disable=SC1091
. "${T}/funcs.sh"
J_CONF="${T}/cfg/system.cfg"; J_CONF_LOCK="${T}/lock"; GS_LOADED=1
mkdir -p "${T}/cfg"
reset() { printf "$1" >"${J_CONF}"; rm -f "${J_CONF_LOCK}"; }
cur() { cat "${J_CONF}"; }

echo "== set_setting"
reset 'a=1\nb=2\nc=3\n'; set_setting b 9
check "clave existente: sale de su sitio y va al final, el resto en orden" $'a=1\nc=3\nb=9' "$(cur)"
reset 'a=1\n'; set_setting n 5
check "clave nueva se anade al final" $'a=1\nn=5' "$(cur)"
reset 'a=1\nb=2\n'; set_setting a ""
check "valor vacio" $'b=2\na=' "$(cur)"
reset 'a=1\nb=2\n'; set_setting a default
check "valor default borra la clave" "b=2" "$(cur)"
reset 'k=1\n'; v='x y & / \ = $HOME `id` "q" '"'"'z'"'"' *'; set_setting k "${v}"
check "valor con caracteres raros intacto" "k=${v}" "$(cur)"
reset 'a.b=1\na.bc=2\naXb=3\n'; set_setting a.b 7
check "a.b no toca a.bc ni aXb (literal, no regex)" $'a.bc=2\naXb=3\na.b=7' "$(cur)"
reset 'a.b=1\na.bc=2\n'; set_setting a.bc 8
check "a.bc no toca a.b" $'a.b=1\na.bc=8' "$(cur)"
reset 'a=1\nb=2'; set_setting c 3
check "fichero sin salto final: no pega la clave a la ultima linea" $'a=1\nb=2\nc=3' "$(cur)"
rm -f "${J_CONF}"; set_setting z 1
check "fichero inexistente: se crea" "z=1" "$(cur)"
reset 'a=1\n'; set_setting "_no" 1
check "clave que no empieza por alfanumerico: se ignora" "a=1" "$(cur)"
reset 'a/b=1\nc=2\n'; set_setting a/b 4
check "clave con barra (sed la rompia)" $'c=2\na/b=4' "$(cur)"
reset 'a=1\n'; chmod 600 "${J_CONF}"; set_setting a 2
check "conserva permisos del fichero" "600" "$(stat -c %a "${J_CONF}")"
check "no deja temporal ni bloqueo" "" "$(ls "${T}/cfg" | grep -v '^system.cfg$'; ls "${J_CONF_LOCK}" 2>/dev/null)"
# una sola sustitucion visible: un lector concurrente nunca ve la clave ausente
reset 'keep=1\nflip=0\n'
( for i in $(seq 1 300); do set_setting flip $i; done ) &
miss=0
while kill -0 $! 2>/dev/null; do grep -q '^flip=' "${J_CONF}" || miss=$((miss+1)); done
wait
check "lector concurrente nunca ve la clave ausente" "0" "${miss}"

echo "== del_setting"
reset 'a.b=1\na.bc=2\n'; del_setting a.b
check "borra solo la clave exacta" "a.bc=2" "$(cur)"
reset 'a=1\nb=2'; del_setting b
check "borra la ultima sin salto final" "a=1" "$(cur)"
reset 'a=1\n'; del_setting nada
check "clave ausente: nada cambia" "a=1" "$(cur)"
reset 'a=1\na=2\nb=3\n'; del_setting a
check "borra todas las repeticiones" "b=3" "$(cur)"

echo "== copy_dir_atomic"
S="${T}/src"; D="${T}/dst"; mkdir -p "${S}" "${D}"
printf 'pass1' >"${S}/Mi Wifi.psk"; printf 'pass2' >"${S}/=4d79.psk"; printf 'x' >"${S}/a.b=c.open"; chmod 644 "${S}"/*
: >"${D}/Mi Wifi.psk"          # resto de un corte: 0 bytes
printf 'keep' >"${D}/=4d79.psk"; : >"${S}/vacio.psk"; printf 'old' >"${D}/vacio.psk"
copy_dir_atomic "${S}" "${D}"
check "0 bytes en destino se repara" "pass1" "$(cat "${D}/Mi Wifi.psk")"
check "nombre con = y hex" "pass2" "$(cat "${D}/=4d79.psk")"
check "nombre con punto e =" "x" "$(cat "${D}/a.b=c.open")"
check "origen vacio no pisa destino con contenido" "old" "$(cat "${D}/vacio.psk")"
check "permisos 0600" "600 600 600" "$(stat -c %a "${D}/Mi Wifi.psk" "${D}/=4d79.psk" "${D}/a.b=c.open" | paste -sd' ')"
check "sin temporales" "4" "$(ls -A "${D}" | wc -l | tr -d ' ')"
# orden: el temporal esta en la misma carpeta, hay sync antes del mv y el destino no se toca hasta el mv
mkdir -p "${T}/bin"; : >"${T}/calls"
printf '#!/bin/bash\necho "sync" >>"%s/calls"\n' "${T}" >"${T}/bin/sync"
printf '#!/bin/bash\necho "mv $(dirname "$2") $(basename "$3") dest=$(cat "$3" 2>/dev/null)" >>"%s/calls"\n/bin/mv "$@"\n' "${T}" >"${T}/bin/mv"
chmod +x "${T}/bin/"*
rm -rf "${S}" "${D}"; mkdir -p "${S}" "${D}"; printf 'new' >"${S}/n.psk"; printf 'old' >"${D}/n.psk"
PATH="${T}/bin:${PATH}" copy_dir_atomic "${S}" "${D}"
check "sync, mv desde la misma carpeta con el destino aun viejo, sync final" "sync
mv ${D} n.psk dest=old
sync" "$(cat "${T}/calls")"
check "origen inexistente no falla" "0" "$(copy_dir_atomic "${T}/nada" "${T}/d2"; echo $?)"

echo "  (${OK} passed, ${BAD} failed)"
[ "${BAD}" = 0 ]
