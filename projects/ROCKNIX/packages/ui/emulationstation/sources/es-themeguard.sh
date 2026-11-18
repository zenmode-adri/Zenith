#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Zenith: ThemeSet que ES entiende. zfront puede dejar en es_settings.cfg un tema que solo existe para el (por ejemplo "zf-swiss",
# o una variante de color de /usr/share/zfront). Si ES no lo encuentra elige en silencio el PRIMER directorio de /usr/share/themes por
# orden alfabetico (mayusculas primero): "Default", una carpeta GTK sin theme.xml -> sin tema. Aqui se evita:
#   themeguard_for_es      justo antes de lanzar ES: si ThemeSet no existe como tema de ES, lo guarda en zfront.themeset_before_es
#                          y pone un respaldo (y lo apunta en zfront.themeset_es_fallback): si el nombre empieza por "zf-" (pieles de zfront)
#                          es-theme-zenith-dark si subset.color-scheme vale "dark" en es_settings.cfg y es-theme-zenith en cualquier otro
#                          caso (light, ausente, vacio); si no es "zf-": es-theme-zenith-dark (si el nombre lleva "dark") o es-theme-zenith.
#                          Si el Zenith elegido no existe, el otro Zenith. subset.color-scheme no se toca.
#   themeguard_for_zfront  al arrancar zfront: devuelve el ThemeSet guardado si nadie lo cambio en ES; borra siempre la clave.
# Se "sourcea" desde start_es.sh; necesita get_setting/set_setting/del_setting. Nunca devuelve error ni debe impedir el arranque.
# Variables (pruebas): ES_SETTINGS_CFG, ES_THEMES_USER, ES_THEMES_SYS.

_tg_cfg() { echo "${ES_SETTINGS_CFG:-/storage/.config/emulationstation/es_settings.cfg}"; }

# valor actual de ThemeSet (vacio si no hay fichero o linea)
_tg_get() {
  local f; f="$(_tg_cfg)"
  [ -r "${f}" ] || return 0
  sed -n 's/.*<string name="ThemeSet" value="\([^"]*\)".*/\1/p' "${f}" 2>/dev/null | head -n 1
}

# pone ThemeSet=$1 (sin regex ni sed sobre el valor: vale con espacios, & o /); fichero temporal en el mismo directorio + mv
_tg_put() {
  local f tmp; f="$(_tg_cfg)"
  [ -w "${f}" ] || return 1
  tmp="$(mktemp "${f}.tg.XXXXXX" 2>/dev/null)" || return 1
  if NEWV="$1" awk '
      BEGIN { p = "<string name=\"ThemeSet\" value=\"" }
      !done && (i = index($0, p)) > 0 {
        r = substr($0, i + length(p)); j = index(r, "\"")
        if (j > 0) { print substr($0, 1, i - 1 + length(p)) ENVIRON["NEWV"] substr(r, j); done = 1; next }
      }
      { print }' "${f}" >"${tmp}" 2>/dev/null && [ -s "${tmp}" ]; then
    chmod --reference="${f}" "${tmp}" 2>/dev/null
    mv -f "${tmp}" "${f}" 2>/dev/null && return 0
  fi
  rm -f "${tmp}" 2>/dev/null
  return 1
}

_tg_valid() {   # ES lo reconoce: carpeta con theme.xml en el usuario o en el sistema
  case "$1" in ''|*/*|.*) return 1 ;; esac
  [ -f "${ES_THEMES_USER:-/storage/.config/emulationstation/themes}/$1/theme.xml" ] && return 0
  [ -f "${ES_THEMES_SYS:-/usr/share/themes}/$1/theme.xml" ] && return 0
  return 1
}

_tg_scheme() {   # valor de subset.color-scheme (vacio si no hay)
  local f; f="$(_tg_cfg)"
  [ -r "${f}" ] || return 0
  sed -n 's/.*<string name="subset.color-scheme" value="\([^"]*\)".*/\1/p' "${f}" 2>/dev/null | head -n 1
}

_tg_fallback() {   # tema de ES de respaldo para el ThemeSet $1 (Zenith claro u oscuro; si falta el elegido, el otro)
  local l want other; l="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
  want=es-theme-zenith
  case "$1" in
    zf-*) [ "$(_tg_scheme)" = dark ] && want=es-theme-zenith-dark ;;
    *) case "${l}" in *dark*) want=es-theme-zenith-dark ;; esac ;;
  esac
  if [ "${want}" = es-theme-zenith ]; then other=es-theme-zenith-dark; else other=es-theme-zenith; fi
  _tg_valid "${want}" || ! _tg_valid "${other}" || want="${other}"
  echo "${want}"
}

themeguard_for_es() {
  local cur new
  cur="$(_tg_get)"
  [ -n "${cur}" ] || return 0
  _tg_valid "${cur}" && return 0
  new="$(_tg_fallback "${cur}")"
  _tg_valid "${new}" || return 0   # ni el de respaldo existe: no se toca nada
  if _tg_put "${new}"; then
    set_setting zfront.themeset_before_es "${cur}" 2>/dev/null
    set_setting zfront.themeset_es_fallback "${new}" 2>/dev/null
  fi
  return 0
}

themeguard_for_zfront() {
  # Limite conocido: si en ES el usuario elige justo el tema que puso la guarda (p. ej. es-theme-zenith), es indistinguible y se restaura.
  local saved cur applied
  saved="$(get_setting zfront.themeset_before_es 2>/dev/null)"
  applied="$(get_setting zfront.themeset_es_fallback 2>/dev/null)"
  [ -n "${saved}" ] || { del_setting zfront.themeset_es_fallback 2>/dev/null; return 0; }
  [ -n "${applied}" ] || applied="$(_tg_fallback "${saved}")"   # claves de versiones anteriores
  cur="$(_tg_get)"
  if [ -n "${cur}" ] && [ "${cur}" = "${applied}" ]; then
    _tg_put "${saved}"
  fi
  del_setting zfront.themeset_before_es 2>/dev/null
  del_setting zfront.themeset_es_fallback 2>/dev/null
  return 0
}
