#!/bin/bash
# Zenith: modo de prueba de DTB ("probar el siguiente").
#
# Para consolas clon cuyo DTB no se puede deducir (el ADC de arranque es el mismo). Lo activa el usuario desde
# Zenith DTB Selector (PC): deja en la FAT /flash/dtbprobe/{state,list,index,extlinux.orig} y pone en extlinux.conf
# el primer candidato. En cada arranque con esa marca, autostart (ruta legacy, ANTES de esperar a multi-user/red y
# antes de la UI) llama a este script:
#   1. pinta la tarjeta "zenith-probe" y espera TIMEOUT s a que se MANTENGA pulsado un boton (mando o VOL+/VOL-);
#   2. con boton  -> el DTB actual se queda (extlinux.conf ya lo tiene), se borra el modo de prueba y el arranque sigue;
#   3. sin boton  -> se escribe el siguiente candidato en extlinux.conf, se avanza el indice y se reinicia.
# Si se agotan los candidatos se restaura el extlinux.conf original (extlinux.orig) y se reinicia una vez mas.
#
# Seguridad: sin /flash/dtbprobe/state NO hace nada. Si extlinux.conf ya no apunta al candidato esperado (el usuario
# lo cambio a mano o con el selector) cancela el modo de prueba. Tope de intentos para no entrar en bucle.
# Solo toca la linea FDT de extlinux.conf; nunca otra cosa. Escritura atomica (temporal + mv) y comprobada.
# /flash solo se remonta en lectura-escritura alrededor de las escrituras (no durante la espera).
# No se usa el boton de ENCENDIDO como confirmacion: puede disparar la suspension.
#
# Variables SOLO para pruebas: DTBPROBE_FLASH, DTBPROBE_TIMEOUT, DTBPROBE_POLL, DTBPROBE_REBOOT, DTBPROBE_EVTEST,
# DTBPROBE_INPUT_DIR, DTBPROBE_REMOUNT=0, DTBPROBE_IGNORE_FIRST, DTBPROBE_INPUT_WAIT, ZSPLASH.

FLASH="${DTBPROBE_FLASH:-/flash}"
PDIR="${FLASH}/dtbprobe"
EXTLINUX="${FLASH}/extlinux/extlinux.conf"
ZSPLASH="${ZSPLASH:-/usr/lib/autostart/firstboot/zenith-splash}"
TIMEOUT="${DTBPROBE_TIMEOUT:-12}"
POLL="${DTBPROBE_POLL:-0.1}"
REBOOT="${DTBPROBE_REBOOT:-reboot}"
EVTEST="${DTBPROBE_EVTEST:-evtest}"
INDIR="${DTBPROBE_INPUT_DIR:-/dev/input/by-path}"
REMOUNT="${DTBPROBE_REMOUNT:-1}"
IGNORE_FIRST="${DTBPROBE_IGNORE_FIRST:-2}"   # s iniciales sin leer botones (por si se mantenia pulsado el del reinicio)
INPUT_WAIT="${DTBPROBE_INPUT_WAIT:-10}"      # s maximos esperando a que aparezca el mando (su driver sale tarde)

LOGBUF=""
log() {
  echo "dtb-probe: $*"
  LOGBUF="${LOGBUF}$(date +%T) $*"$'\n'
}
flush_log() { [ -d "${PDIR}" ] && [ -n "${LOGBUF}" ] && printf '%s' "${LOGBUF}" >>"${PDIR}/log" 2>/dev/null; LOGBUF=""; }

rw() { [ "${REMOUNT}" = "1" ] && mount -o remount,rw "${FLASH}" 2>/dev/null; return 0; }
ro() { sync; [ "${REMOUNT}" = "1" ] && mount -o remount,ro "${FLASH}" 2>/dev/null; return 0; }

valid_name() { case "$1" in ""|*[!A-Za-z0-9._-]*|-*|.*) return 1 ;; *.dtb) return 0 ;; *) return 1 ;; esac; }

# FDT actual de un extlinux.conf (sin la barra inicial)
current_fdt() {   # $1 = fichero (por defecto extlinux.conf)
  awk '/^[ \t]*FDT[ \t]+/ { sub(/^[ \t]*FDT[ \t]+/, ""); sub(/[ \t\r]+$/, ""); sub(/^\//, ""); print; exit }' "${1:-${EXTLINUX}}" 2>/dev/null
}

# Cambia SOLO la primera linea FDT, conservando la sangria. Devuelve 0 si queda verificado. (Requiere /flash en rw.)
set_fdt() {
  local name="$1" tmp="${EXTLINUX}.tmp-zenith"
  valid_name "${name}" || return 1
  awk -v n="/${name}" '!d && /^[ \t]*FDT[ \t]+/ { match($0, /^[ \t]*/); print substr($0, 1, RLENGTH) "FDT " n; d=1; next } { sub(/\r$/, ""); print }' \
      "${EXTLINUX}" >"${tmp}" 2>/dev/null || { rm -f "${tmp}"; return 1; }
  [ "$(current_fdt "${tmp}")" = "${name}" ] || { rm -f "${tmp}"; return 1; }
  mv -f "${tmp}" "${EXTLINUX}" 2>/dev/null || { rm -f "${tmp}"; return 1; }
  [ "$(current_fdt)" = "${name}" ]
}

restore_original() {   # requiere rw
  if [ -f "${PDIR}/extlinux.orig" ]; then
    cp -f "${PDIR}/extlinux.orig" "${EXTLINUX}.tmp-zenith" 2>/dev/null && mv -f "${EXTLINUX}.tmp-zenith" "${EXTLINUX}" 2>/dev/null
  fi
}

end_probe() {   # $1 = resultado para dtbprobe-last.txt; el registro se conserva en dtbprobe-last.log. Requiere rw.
  flush_log
  echo "$1" >"${FLASH}/dtbprobe-last.txt" 2>/dev/null
  [ -f "${PDIR}/log" ] && cp -f "${PDIR}/log" "${FLASH}/dtbprobe-last.log" 2>/dev/null
  rm -rf "${PDIR}"
}

# Teclas que cuentan como "lo veo bien": botones del mando y VOL+/VOL- (adc-keys). Se mira si estan PULSADAS ahora mismo
# (hay que mantener pulsado ~1 s; un toque muy corto puede caer entre dos lecturas). NO se usa el boton de encendido.
pressed() {
  local dev base code
  for dev in "${INDIR}"/*event*; do
    [ -e "${dev}" ] || continue
    base="${dev##*/}"
    case "${base}" in
      *event-joystick) for code in BTN_EAST BTN_SOUTH BTN_NORTH BTN_WEST BTN_START BTN_SELECT; do
                         "${EVTEST}" --query "${dev}" EV_KEY "${code}"; [ $? -eq 10 ] && return 0; done ;;
      *adc-keys*|*gpio-keys*) for code in KEY_VOLUMEUP KEY_VOLUMEDOWN; do
                         "${EVTEST}" --query "${dev}" EV_KEY "${code}"; [ $? -eq 10 ] && return 0; done ;;
    esac
  done
  return 1
}

has_pad() { local d; for d in "${INDIR}"/*event-joystick; do [ -e "${d}" ] && return 0; done; return 1; }

[ -f "${PDIR}/state" ] || exit 0

if [ ! -f "${EXTLINUX}" ]; then log "no hay extlinux.conf: se cancela"; rw; end_probe "cancelado: sin extlinux.conf"; ro; exit 0; fi

# --- lista de candidatos (solo nombres validos y que existan en la tarjeta) ---
LIST=()
if [ -f "${PDIR}/list" ]; then
  while IFS= read -r line || [ -n "${line}" ]; do
    line="${line%$'\r'}"
    valid_name "${line}" && [ -f "${FLASH}/${line}" ] && LIST+=("${line}")
  done <"${PDIR}/list"
fi
N=${#LIST[@]}
IDX=0
if [ -f "${PDIR}/index" ]; then read -r IDX <"${PDIR}/index"; fi
case "${IDX}" in ""|*[!0-9]*) IDX=0 ;; esac
ATT=0
if [ -f "${PDIR}/attempts" ]; then read -r ATT <"${PDIR}/attempts"; fi
case "${ATT}" in ""|*[!0-9]*) ATT=0 ;; esac
ATT=$((ATT + 1))

log "arranque con modo de prueba: intento ${ATT}, candidato $((IDX + 1))/${N}"
rw
echo "${ATT}" >"${PDIR}/attempts" 2>/dev/null

if [ "${N}" -lt 1 ] || [ "${IDX}" -ge "${N}" ] || [ "${ATT}" -gt $((2 * N + 3)) ]; then
  log "lista vacia, indice fuera de rango o demasiados intentos: se restaura el extlinux.conf original"
  restore_original; end_probe "abortado: restaurado el original"; ro
  [ "${N}" -ge 1 ] && { sleep 1; ${REBOOT}; sleep 60; }
  exit 0
fi

CUR="${LIST[${IDX}]}"
if [ "$(current_fdt)" != "${CUR}" ]; then
  log "extlinux.conf apunta a '$(current_fdt)' y no a '${CUR}': alguien lo cambio, se cancela el modo de prueba"
  end_probe "cancelado: extlinux.conf cambiado por el usuario"; ro; exit 0
fi
flush_log
ro   # durante la espera /flash queda en solo lectura

# --- el driver del mando aparece unos segundos despues del kernel: esperar un tiempo acotado ---
w0=${SECONDS}
until has_pad || [ $((SECONDS - w0)) -ge "${INPUT_WAIT}" ]; do sleep 0.2; done
if has_pad; then log "mando detectado tras $((SECONDS - w0)) s"; else log "sin mando tras ${INPUT_WAIT} s: solo se podran usar las teclas VOL"; fi

# --- tarjeta de prueba y espera ---
"${ZSPLASH}" start zenith-probe >/dev/null 2>&1 || log "no se pudo pintar la tarjeta (sin zenith-splash o sin fotograma para esta resolucion)"
t0=${SECONDS}
confirmed=0
while [ $((SECONDS - t0)) -lt "${TIMEOUT}" ]; do
  if [ $((SECONDS - t0)) -ge "${IGNORE_FIRST}" ] && pressed; then confirmed=1; break; fi
  sleep "${POLL}"
done

rw
if [ "${confirmed}" = "1" ]; then
  log "confirmado: la pantalla se ve bien con ${CUR}"
  end_probe "elegido: ${CUR} (candidato $((IDX + 1)) de ${N}, ${ATT} arranques)"
  ro
  exit 0
fi

# --- sin boton: siguiente candidato ---
NEXT=$((IDX + 1))
if [ "${NEXT}" -ge "${N}" ]; then
  log "se agotaron los ${N} candidatos sin confirmar: se restaura el extlinux.conf original"
  restore_original; end_probe "sin resultado: restaurado el original tras ${N} candidatos"; ro
  sleep 1; ${REBOOT}; sleep 60; exit 0
fi
if ! set_fdt "${LIST[${NEXT}]}"; then
  log "no se pudo escribir ${LIST[${NEXT}]} en extlinux.conf: se restaura el original"
  restore_original; end_probe "error: no se pudo avanzar; restaurado el original"; ro; exit 0
fi
echo "${NEXT}" >"${PDIR}/index"
log "sin boton: se prueba el candidato $((NEXT + 1))/${N}: ${LIST[${NEXT}]}"
flush_log
ro
sleep 1
${REBOOT}
sleep 60
exit 0
