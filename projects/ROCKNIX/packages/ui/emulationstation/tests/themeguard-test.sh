#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# es-themeguard.sh: ES solo recibe un ThemeSet que existe como tema suyo; la eleccion de zfront se guarda y se devuelve.
# Usage: themeguard-test.sh [es-themeguard.sh] [start_es.sh]
HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="${1:-${HERE}/../sources/es-themeguard.sh}"
START="${2:-${HERE}/../sources/start_es.sh}"
T="$(mktemp -d)"; trap 'rm -rf "${T}"' EXIT
OK=0; BAD=0
check() { if [ "$2" = "$3" ]; then OK=$((OK+1)); echo "  PASS $1"; else BAD=$((BAD+1)); echo "  FAIL $1"; printf '    esperado: %q\n    obtenido: %q\n' "$2" "$3"; fi; }
export ES_SETTINGS_CFG="${T}/es_settings.cfg" ES_THEMES_USER="${T}/user" ES_THEMES_SYS="${T}/sys"
mkdir -p "${T}/user" "${T}/sys"
for t in es-theme-zenith es-theme-zenith-dark; do mkdir -p "${T}/sys/${t}"; : >"${T}/sys/${t}/theme.xml"; done
mkdir -p "${T}/sys/Default/gtk-3.0"   # carpeta sin theme.xml (como /usr/share/themes/Default)
mkdir -p "${T}/user/mi tema & x"; : >"${T}/user/mi tema & x/theme.xml"
set_setting() { echo "$2" >"${T}/k_$1"; }
get_setting() { cat "${T}/k_$1" 2>/dev/null; }
del_setting() { rm -f "${T}/k_$1"; }
. "${LIB}"
mkcfg() { printf '<?xml version="1.0"?>\n<config>\n  <string name="Other" value="a&amp;b" />\n  <string name="ThemeSet" value="%s" />\n  <bool name="X" value="true" />\n</config>\n' "$1" >"${ES_SETTINGS_CFG}"; rm -f "${T}"/k_*; }
ts() { sed -n 's/.*name="ThemeSet" value="\(.*\)" \/>/\1/p' "${ES_SETTINGS_CFG}"; }
key() { cat "${T}/k_zfront.themeset_before_es" 2>/dev/null; }
fbk() { cat "${T}/k_zfront.themeset_es_fallback" 2>/dev/null; }

echo "ES: tema valido"
mkcfg es-theme-zenith-dark; cp "${ES_SETTINGS_CFG}" "${T}/ref"; themeguard_for_es
check "sistema: sin cambios" "$(cat "${T}/ref")" "$(cat "${ES_SETTINGS_CFG}")"; check "sistema: sin clave" "" "$(key)"
mkcfg "mi tema & x"; themeguard_for_es; check "usuario: ThemeSet igual" "mi tema & x" "$(ts)"; check "usuario: sin clave" "" "$(key)"
echo "ES: tema inexistente"
mkcfg es-theme-swiss; themeguard_for_es; check "sin dark -> zenith" es-theme-zenith "$(ts)"; check "clave guardada" es-theme-swiss "$(key)"; check "respaldo anotado" es-theme-zenith "$(fbk)"
mkcfg es-theme-zenith-aurora-dark; themeguard_for_es; check "con dark -> zenith-dark" es-theme-zenith-dark "$(ts)"; check "clave dark" es-theme-zenith-aurora-dark "$(key)"
mkcfg Default; themeguard_for_es; check "carpeta sin theme.xml cuenta como inexistente" es-theme-zenith "$(ts)"
mkcfg "../x/y"; themeguard_for_es; check "valor con / no se acepta" es-theme-zenith "$(ts)"
mkcfg "raro & nombre/con \\ barra"; themeguard_for_es; check "caracteres raros: cambia" es-theme-zenith "$(ts)"; check "caracteres raros: clave intacta" "raro & nombre/con \\ barra" "$(key)"
check "otras lineas intactas" 3 "$(grep -c 'a&amp;b\|name="X"\|<config>' "${ES_SETTINGS_CFG}")"
check "sin temporales sobrantes" 0 "$(ls "${T}" | grep -c '\.tg\.')"
echo "ES: sin fichero o sin linea"
rm -f "${ES_SETTINGS_CFG}" "${T}"/k_*; themeguard_for_es; check "sin fichero: nada" "" "$(key)"; [ ! -e "${ES_SETTINGS_CFG}" ]; check "sin fichero: no se crea" 0 $?
printf '<config>\n</config>\n' >"${ES_SETTINGS_CFG}"; themeguard_for_es; check "sin linea: nada" "" "$(key)"; check "sin linea: fichero igual" 2 "$(wc -l <"${ES_SETTINGS_CFG}")"
echo "ES: pieles de zfront (zf-*) -> Zenith claro/oscuro segun subset.color-scheme"
mkscheme() { { head -n 2 "${ES_SETTINGS_CFG}"; printf '  <string name="subset.color-scheme" value="%s" />\n' "$1"; tail -n +3 "${ES_SETTINGS_CFG}"; } >"${ES_SETTINGS_CFG}.n"; mv "${ES_SETTINGS_CFG}.n" "${ES_SETTINGS_CFG}"; }
sch() { sed -n 's/.*name="subset.color-scheme" value="\([^"]*\)".*/\1/p' "${ES_SETTINGS_CFG}"; }
mkcfg zf-swiss; mkscheme dark; themeguard_for_es; check "zf-swiss + dark -> zenith-dark" es-theme-zenith-dark "$(ts)"; check "clave guardada" zf-swiss "$(key)"; check "respaldo anotado" es-theme-zenith-dark "$(fbk)"; check "subset intacto" dark "$(sch)"
mkcfg zf-swiss; mkscheme light; themeguard_for_es; check "zf-swiss + light -> zenith" es-theme-zenith "$(ts)"; check "subset intacto (light)" light "$(sch)"
mkcfg zf-aurora; themeguard_for_es; check "zf-aurora sin clave -> zenith" es-theme-zenith "$(ts)"
mkcfg zf-aurora; mkscheme ""; themeguard_for_es; check "zf-aurora subset vacio -> zenith" es-theme-zenith "$(ts)"
mkcfg zf-industrial; mkscheme dark; themeguard_for_es; check "zf-industrial dark -> zenith-dark" es-theme-zenith-dark "$(ts)"
themeguard_for_zfront; check "zf-industrial dark: vuelta restaura" zf-industrial "$(ts)"; check "subset sigue dark" dark "$(sch)"
mv "${T}/sys/es-theme-zenith-dark" "${T}/zd.off"
mkcfg zf-x; mkscheme dark; themeguard_for_es; check "zf-x dark sin zenith-dark -> zenith" es-theme-zenith "$(ts)"
mv "${T}/zd.off" "${T}/sys/es-theme-zenith-dark"
mkcfg es-theme-swiss; mkscheme dark; themeguard_for_es; check "no-zf ignora subset: zenith" es-theme-zenith "$(ts)"
echo "ES: sin tema de respaldo en la imagen"
rm -f "${T}/sys/es-theme-zenith/theme.xml" "${T}/sys/es-theme-zenith-dark/theme.xml"; mkcfg es-theme-swiss; themeguard_for_es; check "no toca nada" es-theme-swiss "$(ts)"; check "no guarda clave" "" "$(key)"
for t in es-theme-zenith es-theme-zenith-dark; do : >"${T}/sys/${t}/theme.xml"; done
echo "vuelta a zfront"
mkcfg zf-swiss; themeguard_for_es; themeguard_for_zfront; check "restaura" zf-swiss "$(ts)"; check "borra la clave" "" "$(key)"; check "borra la clave del respaldo" "" "$(fbk)"
mkcfg zf-industrial; themeguard_for_es; themeguard_for_zfront; check "zf-industrial: ida y vuelta" zf-industrial "$(ts)"
mkcfg es-theme-swiss; themeguard_for_es; themeguard_for_zfront; check "no-zf: ida y vuelta" es-theme-swiss "$(ts)"
# limite conocido: si en ES el usuario elige justo el tema que puso la guarda (zenith), es indistinguible y se restaura
mkcfg zf-swiss; themeguard_for_es; themeguard_for_zfront; check "elige el mismo respaldo en ES: se restaura (limite documentado)" zf-swiss "$(ts)"
mkcfg zf-swiss; mkscheme dark; themeguard_for_es; sed -i 's/value="es-theme-zenith-dark"/value="es-theme-zenith"/' "${ES_SETTINGS_CFG}"; themeguard_for_zfront
check "zf-swiss, cambia de tema en ES: no restaura" es-theme-zenith "$(ts)"; check "...y borra claves" "" "$(key)$(fbk)"
mkcfg "raro & nombre/con \ barra"; themeguard_for_es; themeguard_for_zfront; check "restaura valor raro" "raro & nombre/con \ barra" "$(ts)"
mkcfg es-theme-swiss; themeguard_for_es; sed -i 's/value="es-theme-zenith"/value="es-theme-zenith-dark"/' "${ES_SETTINGS_CFG}"
themeguard_for_zfront; check "el usuario cambio el tema en ES: no restaura" es-theme-zenith-dark "$(ts)"; check "...y borra la clave" "" "$(key)"
mkcfg es-theme-zenith-dark; themeguard_for_zfront; check "sin clave: nada" es-theme-zenith-dark "$(ts)"
echo "start_es.sh: la guarda esta enganchada y no impide arrancar"
mkdir -p "${T}/b/bin" "${T}/b/log" "${T}/b/run"
cp "${LIB}" "${T}/b/bin/es-themeguard.sh"
printf '#!/bin/bash\necho es >>"%s/b/ran"\n' "${T}" >"${T}/b/bin/emulationstation"; chmod +x "${T}/b/bin/emulationstation"
printf 'get_setting() { :; }\nset_setting() { :; }\ndel_setting() { :; }\n' >"${T}/b/bin/es_settings"
sed -e "s|^emulationstation |${T}/b/bin/emulationstation |" -e "s|/run/|${T}/b/run/|g" -e "s|/var/log|${T}/b/log|g" -e "s|/sys/class/backlight|${T}/b/bl|g" "${START}" >"${T}/b/bin/start_es.sh"
mkcfg zf-swiss; bash "${T}/b/bin/start_es.sh" >/dev/null 2>&1
check "ES arranca" es "$(cat "${T}/b/ran" 2>/dev/null)"
check "start_es.sh pone el tema de ES" es-theme-zenith "$(ts)"
echo "start_es.sh con guarda rota: sigue arrancando"
rm -f "${T}/b/ran"; printf 'themeguard_for_es() { return 7; }\nexit 9\n' >"${T}/b/bin/es-themeguard.sh"
bash "${T}/b/bin/start_es.sh" >/dev/null 2>&1; check "ES arranca aunque la guarda falle" es "$(cat "${T}/b/ran" 2>/dev/null)"
echo; echo "pasan ${OK}, fallan ${BAD}"; [ "${BAD}" = 0 ]
