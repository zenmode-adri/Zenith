#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2023 JELOS (https://github.com/JustEnoughLinuxOS)

. /etc/profile
set_kill set "-9 ppsspp"

SOURCE_DIR="/usr/config/ppsspp"
CONF_DIR="/storage/.config/ppsspp"
PPSSPP_INI="PSP/SYSTEM/ppsspp.ini"

# Check if conf dir exists
if [ ! -d "${CONF_DIR}" ]
then
  cp -rf ${SOURCE_DIR} ${CONF_DIR}
fi

# Check if savestate dir exists
if [ ! -d "/storage/roms/savestates/psp/ppsspp-sa" ]; then
  mkdir -p "/storage/roms/savestates/psp/ppsspp-sa"
fi

#Emulation Station Features
GAME=$(echo "${1}"| sed "s#^/.*/##")
PLATFORM=$(echo "${2}"| sed "s#^/.*/##")
ASKIP=$(get_setting auto_frame_skip "${PLATFORM}" "${GAME}")
FPS=$(get_setting show_fps "${PLATFORM}" "${GAME}")
IRES=$(get_setting internal_resolution "${PLATFORM}" "${GAME}")
GRENDERER=$(get_setting graphics_backend "${PLATFORM}" "${GAME}")
SKIPB=$(get_setting skip_buffer_effects "${PLATFORM}" "${GAME}")
VSYNC=$(get_setting vsync "${PLATFORM}" "${GAME}")
CLOCK_SPEED=$(get_setting clock_speed "${PLATFORM}" "${GAME}")

#Set the cores to use
CORES=$(get_setting "cores" "${PLATFORM}" "${GAME}")
if [ "${CORES}" = "little" ]; then
  EMUPERF="${SLOW_CORES}"
elif [ "${CORES}" = "big" ]; then
  EMUPERF="${FAST_CORES}"
else
  ### All..
  unset EMUPERF
fi

  #Auto Frame Skip
  # PPSSPP: iFrameSkip (FrameSkip) = salto FIJO de N imagenes (0 = ninguno); con AutoFrameSkip solo salta cuando va tarde Y exige iFrameSkip > 0
  # (Core/HLE/sceDisplay.cpp: doFrameSkip = iFrameSkip != 0, y el tope de saltos seguidos es iFrameSkip). Por eso:
  #  - auto_frame_skip=1: AutoFrameSkip = True y, si FrameSkip es 0, 3 (tope); un valor ya puesto por el usuario se respeta;
  #  - si no: AutoFrameSkip = False y FrameSkip = 0 UNA vez (el ini de fabrica antiguo traia FrameSkip = 3, que sin salto automatico
  #    es "dibuja 1 de cada 4"); con el marcador puesto no se toca, asi lo que el usuario elija en el menu de PPSSPP se respeta.
  ZFS_INI="${CONF_DIR}/${PPSSPP_INI}"
  ZFS_MARK="${CONF_DIR}/.zenith-frameskip-migrated"
	if [ "${ASKIP}" = "1" ]; then
		sed -i '/^AutoFrameSkip =/c\AutoFrameSkip = True' "${ZFS_INI}" 2>/dev/null
		ZFS_CUR=$(sed -n 's/^FrameSkip = *//p' "${ZFS_INI}" 2>/dev/null | head -n 1)
		case "${ZFS_CUR}" in ''|0|*[!0-9]*)
			if grep -q '^FrameSkip =' "${ZFS_INI}" 2>/dev/null; then
				sed -i '/^FrameSkip =/c\FrameSkip = 3' "${ZFS_INI}"
			else
				sed -i '/^\[Graphics\]/a\FrameSkip = 3' "${ZFS_INI}" 2>/dev/null
			fi ;;
		esac
	else
		sed -i '/^AutoFrameSkip =/c\AutoFrameSkip = False' "${ZFS_INI}" 2>/dev/null
		if [ ! -e "${ZFS_MARK}" ]; then
			sed -i '/^FrameSkip =/c\FrameSkip = 0' "${ZFS_INI}" 2>/dev/null
			# si no se puede escribir el marcador (tarjeta llena) el juego arranca igual (`:` en sh abortaria el lanzador: printf)
			{ printf '' >"${ZFS_MARK}"; } 2>/dev/null || logger -t start_ppsspp "no se pudo escribir ${ZFS_MARK} (/storage lleno?): FrameSkip = 0 se repetira en cada arranque"
		fi
        fi

  # Filtro anisotropico: el ini de fabrica antiguo traia AnisotropyLevel = 4 (16x), caro en la Mali-G31. UNA vez (marcador) ese 4 pasa a 0;
  # cualquier otro valor se respeta, y con el marcador puesto lo que el usuario elija en el menu de PPSSPP no se toca.
  ZAN_MARK="${CONF_DIR}/.zenith-aniso-migrated"
	if [ ! -e "${ZAN_MARK}" ]; then
		sed -i 's/^AnisotropyLevel = 4[[:space:]]*$/AnisotropyLevel = 0/' "${ZFS_INI}" 2>/dev/null
		{ printf '' >"${ZAN_MARK}"; } 2>/dev/null || logger -t start_ppsspp "no se pudo escribir ${ZAN_MARK} (/storage llena o solo lectura)"
	fi

  #Graphics Backend
  # El motor lo elige el usuario en PPSSPP (Ajustes -> Graficos -> Backend) y queda en su ini: el lanzador YA NO lo impone en cada arranque.
  # Unica excepcion: un ajuste EXPLICITO por juego/sistema (graphics_backend = opengl | vulkan en system.cfg; ninguna interfaz de zfront lo ofrece).
        if [ "${GRENDERER}" = "opengl" ]; then
                sed -i '/^GraphicsBackend =/c\GraphicsBackend = 0 (OPENGL)' ${CONF_DIR}/${PPSSPP_INI}
        elif [ "${GRENDERER}" = "vulkan" ]; then
                sed -i '/^GraphicsBackend =/c\GraphicsBackend = 3 (VULKAN)' ${CONF_DIR}/${PPSSPP_INI}
        fi

  #Internal Resolution
	if [ "${IRES}" = "2" ]; then
		sed -i '/^InternalResolution/c\InternalResolution = 2' ${CONF_DIR}/${PPSSPP_INI}
	elif [ "${IRES}" = "3" ]; then
		sed -i '/^InternalResolution/c\InternalResolution = 3' ${CONF_DIR}/${PPSSPP_INI}
	elif [ "${IRES}" = "4" ]; then
                sed -i '/^InternalResolution/c\InternalResolution = 4' ${CONF_DIR}/${PPSSPP_INI}
	else
		sed -i '/^InternalResolution/c\InternalResolution = 1' ${CONF_DIR}/${PPSSPP_INI}
        fi

  #Show FPS
	if [ "${FPS}" = "1" ]; then
		sed -i '/^iShowStatusFlags =/c\iShowStatusFlags = 2' ${CONF_DIR}/${PPSSPP_INI}
	else
		sed -i '/^iShowStatusFlags =/c\iShowStatusFlags = 0' ${CONF_DIR}/${PPSSPP_INI}
	fi

  #Skip Buffer Effects
	if [ "${SKIPB}" = "1" ]; then
		sed -i '/^SkipBufferEffects =/c\SkipBufferEffects = True' ${CONF_DIR}/${PPSSPP_INI}
	else
		sed -i '/^SkipBufferEffects =/c\SkipBufferEffects = False' ${CONF_DIR}/${PPSSPP_INI}
	fi

  #VSYNC
	if [ "${VSYNC}" = "1" ]; then
		sed -i '/^VSyncInterval =/c\VSyncInterval = True' ${CONF_DIR}/${PPSSPP_INI}
	else
		sed -i '/^VSyncInterval =/c\VSyncInterval = False' ${CONF_DIR}/${PPSSPP_INI}
	fi

  #Clock Speed
	if [ "${CLOCK_SPEED}" = "222" ]; then
		sed -i '/^CPUSpeed =/c\CPUSpeed = 222' ${CONF_DIR}/${PPSSPP_INI}
  elif [ "${CLOCK_SPEED}" = "333" ]; then
		sed -i '/^CPUSpeed =/c\CPUSpeed = 333' ${CONF_DIR}/${PPSSPP_INI}
	else
		sed -i '/^CPUSpeed =/c\CPUSpeed = 0' ${CONF_DIR}/${PPSSPP_INI}
	fi

### Zenith RK3326 (la imagen lleva /usr/lib/mali-vk y /usr/lib/r36-vk): audio por PipeWire, Vulkan visible desde PPSSPP y 30 fps en God of War GoS
if [ -d "${R36_MALI_VK_DIR:-/usr/lib/mali-vk}" ]; then
  # PipeWire directo en los dos drivers (medido en GoS: +3 a +5 % frente a pulseaudio). Con la lista, si el backend de PipeWire
  # de SDL no arranca se prueba pulseaudio (igual que el valor global de 050-sway.conf)
  export SDL_AUDIODRIVER=pipewire,pulseaudio

  ZINI="${CONF_DIR}/${PPSSPP_INI}"
  ZFAILED="${CONF_DIR}/PSP/SYSTEM/FailedGraphicsBackends.txt"

  # MOTOR GRAFICO (OpenGL / Vulkan): lo elige el usuario en el menu de PPSSPP (Ajustes -> Graficos -> Backend) y queda en su ppsspp.ini;
  # el lanzador solo lo LEE (fabrica = OpenGL). PPSSPP ofrece Vulkan en ese menu solo si, al arrancar, puede cargar un Vulkan que funcione
  # (VulkanMayBeAvailable, se ejecuta siempre, tambien con el ini en OpenGL). Por eso el entorno de Vulkan (cargador + ICD del driver cargado)
  # se prepara en TODOS los arranques, sea cual sea el motor del ini; y como PPSSPP, al cambiar de motor en el menu, se reinicia a si mismo
  # con execv (mismo proceso, mismo entorno, sin argumentos: vuelve al menu principal), ese mismo entorno sirve para el motor nuevo.
  # OpenGL no cambia: con Arm, el libmali de GL sigue siendo el del sistema (g24p0) y el de Vulkan (g29p1) vive en /usr/lib/mali-vk con
  # nombres propios (libmali-vk.so.1, libmali-hook-vk.so.1) y RUNPATH $ORIGIN, sin LD_LIBRARY_PATH sobre ese directorio (con el, libEGL/libGLESv2
  # de g29p1 sustituirian a los de GL).
  pvk_ini_vulkan() { grep -q '^GraphicsBackend *= *3' "${ZINI}" 2>/dev/null; }
  # Escribe GraphicsBackend en el ini; si la linea no existe la anade en [Graphics] (sin ini no hace nada: PPSSPP usara su valor por defecto)
  pvk_set_backend() {
    [ -f "${ZINI}" ] || return 0
    if grep -q '^GraphicsBackend =' "${ZINI}"; then
      sed -i "/^GraphicsBackend =/c\GraphicsBackend = $1" "${ZINI}"
    elif grep -q '^\[Graphics\]' "${ZINI}"; then
      sed -i "/^\[Graphics\]/a\GraphicsBackend = $1" "${ZINI}"
    else
      printf '\n[Graphics]\nGraphicsBackend = %s\n' "$1" >> "${ZINI}"
    fi
  }

  # MIGRACION UNICA del ajuste antiguo ppsspp.vulkan (Tool "PSP Vulkan", ya retirada): 1 -> Vulkan en el ini, 0 -> OpenGL, sin poner -> el ini
  # como esta. Despues se borran el ajuste y su aviso. Con el marcador puesto no se vuelve a tocar nada: manda lo que el usuario elija en PPSSPP.
  ZBMARK="${CONF_DIR}/.zenith-backend-migrated"
  if [ ! -e "${ZBMARK}" ]; then
    case "$(get_setting ppsspp.vulkan)" in
      1) pvk_set_backend "3 (VULKAN)" ;;
      0) pvk_set_backend "0 (OPENGL)" ;;
    esac
    del_setting ppsspp.vulkan
    del_setting ppsspp.vulkan_autooff
    # si no se puede escribir el marcador (tarjeta llena) el juego arranca igual; la migracion es idempotente (sin ajuste no hace nada)
    { printf '' >"${ZBMARK}"; } 2>/dev/null || logger -t start_ppsspp "no se pudo escribir ${ZBMARK} (/storage lleno?)"
  fi

  # Red de seguridad de Vulkan. PPSSPP escribe FailedGraphicsBackends.txt (con el motor en uso) al arrancar y lo borra a los 10 fotogramas,
  # asi que el fichero solo no basta: tambien esta ahi si el usuario sale del juego en los primeros segundos. Por eso el lanzador deja un
  # marcador propio (.vk-running, persistente: un cuelgue con apagado forzado tambien cuenta) y lo borra cuando PPSSPP termina "limpio".
  # El cierre normal del sistema (combinacion de salida y zfront) manda SIGTERM a ppsspp (set_kill de zg_run, mas abajo; si no se puede
  # montar el vigilante queda el `killall -9 ppsspp` de siempre). PPSSPP sale entonces con codigo 0 y guarda su cache de sombreadores,
  # asi que el codigo ya no distingue «salida normal» de «salida pedida desde fuera»: zg_run avisa a zvk_after_run (2.o argumento = 1)
  # cuando el cierre vino de fuera, y entonces «temprano» = el fichero de fallo sigue ahi (menos de 10 fotogramas). SIGKILL (137, el
  # cierre antiguo o el escalado del vigilante) no prueba un fallo por si solo. Reglas al lanzar, con el ini en Vulkan y el fichero de fallo
  # presente mencionando VULKAN:
  #  - sin marcador (salida 0 o SIGTERM por el menu de PPSSPP, o cierre pedido desde fuera tras dibujar 10 fotogramas, registrado por zvk_after_run):
  #    salida limpia, se borra el fichero y se sigue en Vulkan;
  #  - con marcador (137, reinicio brusco o cuelgue): 1 punto; con "crash" dentro (señal de fallo: SEGV, ABRT...): 2 puntos;
  #  - a 2 puntos seguidos el lanzador escribe OpenGL en el ini (el usuario lo vuelve a elegir en el menu de PPSSPP) y deja constancia:
  #    ajuste ppsspp.vulkan_autooff = early|crash + linea en el diario (journalctl -t start_ppsspp). Se borra al arrancar de nuevo en Vulkan.
  # El marcador solo lo deja un arranque EN Vulkan: si el usuario cambia a Vulkan dentro de una sesion en OpenGL y se cierra al instante,
  # no cuenta (un cierre con codigo de fallo si deja el marcador `crash`: eso si lo ve el arranque siguiente).
  # Cualquier arranque posterior sin fichero de fallo (el motor dibujo 10 fotogramas) pone la cuenta a 0.
  ZRUN="${CONF_DIR}/.vk-running"
  ZSTRK="${CONF_DIR}/.vk-strikes"
  if pvk_ini_vulkan && [ -f "${ZFAILED}" ] && grep -q VULKAN "${ZFAILED}" && [ -f "${ZRUN}" ]; then
    ZN=0; [ -f "${ZSTRK}" ] && read ZN < "${ZSTRK}"
    case "${ZN}" in ''|*[!0-9]*) ZN=0 ;; esac
    ZN=$((ZN + 1))
    grep -q crash "${ZRUN}" 2>/dev/null && ZN=$((ZN + 1))
    if [ "${ZN}" -ge 2 ]; then
      grep -q crash "${ZRUN}" 2>/dev/null && ZWHY=crash || ZWHY=early
      pvk_set_backend "0 (OPENGL)"
      rm -f "${ZSTRK}"
      if ! pvk_ini_vulkan; then
        # constancia: ajuste (para que zfront o una Tool lo muestren) y diario; se borra al volver a arrancar en Vulkan
        set_setting ppsspp.vulkan_autooff "${ZWHY}"
        MSG="Vulkan desactivado AUTOMATICAMENTE (${ZWHY}, ${ZN} puntos): GraphicsBackend = OpenGL en ppsspp.ini. Se reactiva en PPSSPP: Ajustes > Graficos > Backend"
        logger -t start_ppsspp "${MSG}"
        echo "${MSG}"
      else
        # /storage lleno o sin permiso: no se pudo escribir el ini; PPSSPP lo reintentara con su propia lista de motores fallidos
        MSG="Vulkan fallo en el arranque anterior (${ZN} puntos) pero no se pudo escribir ppsspp.ini (/storage lleno?)"
        logger -t start_ppsspp "${MSG}"
        echo "${MSG}"
      fi
    else
      # si no se puede escribir la cuenta (/storage lleno) simplemente no persiste: nunca llega a 2
      echo "${ZN}" 2>/dev/null >"${ZSTRK}"
      logger -t start_ppsspp "Vulkan: salida sospechosa antes de dibujar (1 punto); a la segunda seguida se pasa a OpenGL"
    fi
  else
    rm -f "${ZSTRK}"
  fi
  rm -f "${ZFAILED}" "${ZRUN}" 2>/dev/null

  # Se llama tras ppsspp con su codigo de salida y, opcional, 1 si el cierre se pidio desde fuera (combinacion de salida, zfront, bateria):
  # 0 y 143 (SIGTERM) = limpia, SALVO cierre pedido desde fuera con el fichero de fallo todavia ahi (salio antes de 10 fotogramas: marcador
  # se queda = 1 punto, igual que el 137 de antes); 137 (SIGKILL) = ambigua (marcador se queda); cualquier otro (fallo) = marcador con "crash".
  zvk_after_run() {
    case "$1" in
      0|143)
        if [ "$2" = 1 ] && [ -f "${ZFAILED}" ] && grep -q VULKAN "${ZFAILED}" 2>/dev/null; then
          logger -t start_ppsspp "Vulkan: cierre pedido desde fuera antes de 10 fotogramas (queda el marcador: 1 punto)"
        else
          rm -f "${ZRUN}"
        fi ;;
      137) ;;
      *) { echo crash >"${ZRUN}"; } 2>/dev/null ;;
    esac
  }

  # el driver se mira por el modulo cargado de verdad, no por el ajuste gpu.driver (puede haber cambiado sin reiniciar)
  if [ -d "${R36_SYS_MODULE:-/sys/module}/mali_kbase" ]; then
    PVK_MODE="arm"
  elif [ -d "${R36_SYS_MODULE:-/sys/module}/panfrost" ]; then
    PVK_MODE="panvk"
  else
    PVK_MODE=""
  fi
  # Vulkan EN ESTE ARRANQUE = el ini lo pide y el driver lo permite (si el ini veta Vulkan a mano con DisabledGraphicsBackends, PPSSPP no lo usara)
  PVK_ON=""
  if [ -n "${PVK_MODE}" ] && pvk_ini_vulkan && ! grep -q '^DisabledGraphicsBackends *=.*VULKAN' "${ZINI}" 2>/dev/null; then
    PVK_ON=1
    # el aviso de "apagado automatico" ya no vale (sin ajuste no hace nada)
    del_setting ppsspp.vulkan_autooff
  fi

  # entorno de Vulkan del driver cargado, SIEMPRE (el menu de PPSSPP solo ofrece Vulkan si puede cargarlo)
  if [ -n "${PVK_MODE}" ]; then
    export DISABLE_MANGOHUD=1
    if [ "${PVK_MODE}" = "arm" ]; then
      # Vulkan 1.3 de Arm (libmali g29p1, con sonames propios; cargador y SDL2 con Vulkan en r36-vk) + capa WSI de Wayland + capa que oculta
      # B8G8R8A8 (sin ella la swapchain falla en sway). SIN /usr/lib/mali-vk en LD_LIBRARY_PATH: asi OpenGL sigue con el libmali del sistema.
      export LD_LIBRARY_PATH=/usr/lib/r36-vk${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}
      export VK_DRIVER_FILES=/usr/lib/mali-vk/mali.json
      export VK_IMPLICIT_LAYER_PATH=/usr/lib/mali-vk
      export R36_NOBGRA=1
    else
      # PanVK (Panfrost) con nuestros arreglos: PANVK_OPT=4 (bloques de memoria mayores) y minmax de indices en CPU
      export LD_LIBRARY_PATH=/usr/lib/r36-vk${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}
      export VK_DRIVER_FILES=/usr/lib/r36-vk/panfrost_icd.json
      export PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1
      export PANVK_OPT="${PANVK_OPT:-4}"
      export PANVK_CPU_INDEX_MINMAX="${PANVK_CPU_INDEX_MINMAX:-1}"
    fi
  fi

  # marcador de ejecucion (solo si este arranque va en Vulkan; en OpenGL no hay nada que vigilar). Si no se puede escribir (/storage lleno) el juego
  # arranca igual (OJO: `:` es un "special builtin" y un fallo de redireccion en sh mataria el lanzador, por eso printf) y sin
  # marcador esta salida no cuenta puntos ni puede apagar Vulkan.
  if [ -n "${PVK_ON}" ]; then
    { printf '' >"${ZRUN}"; } 2>/dev/null || logger -t start_ppsspp "no se pudo escribir ${ZRUN} (/storage lleno?): la red de seguridad de Vulkan no vigila este arranque"
  fi

  # God of War: Ghost of Sparta a 30 fps (FramerateHack30 de PPSSPP, solo estos juegos). Ajuste ppsspp.gos30 (1 por defecto).
  # Solo se escribe/borra un compat.ini propio (marcado con la linea zenith-gos30): uno del usuario no se toca.
  COMPAT_INI="${CONF_DIR}/PSP/SYSTEM/compat.ini"
  GOS30=$(get_setting ppsspp.gos30)
  [ -z "${GOS30}" ] && GOS30=1
  if [ ! -f "${COMPAT_INI}" ] || grep -q '^# zenith-gos30' "${COMPAT_INI}"; then
    if [ "${GOS30}" = "1" ]; then
      cat > "${COMPAT_INI}" <<COMPAT
# zenith-gos30 (generado por start_ppsspp.sh; quita esta linea para que no se vuelva a escribir)
[FramerateHack30]
UCUS98737 = true
UCAS40323 = true
NPHG00092 = true
NPEG00044 = true
NPEG00045 = true
NPJG00120 = true
NPUG80508 = true
UCJS10114 = true
UCES01401 = true
UCES01473 = true
NPJG90095 = true
NPEG90035 = true
NPUG70125 = true
COMPAT
    else
      rm -f "${COMPAT_INI}"
    fi
  fi
fi

#Retroachievements
/usr/bin/cheevos_ppsspp.sh

ARG=${1//[\\]/}

if [ "${HW_DEVICE}" = "S905" ]; then
  # Avoid underruns on the vendor HDMI PCM without changing global PipeWire latency.
  export PULSE_LATENCY_MSEC=80
  export PIPEWIRE_LATENCY=2048/48000
fi

# Debugging info:
  echo "GAME set to: ${GAME}"
  echo "PLATFORM set to: ${PLATFORM}"
  echo "CONF DIR: ${CONF_DIR}/${PPSSPP_INI}"
  echo "CPU CORES set to: ${EMUPERF}"
  echo "AUTO FRAME SKIP set to: ${ASKIP}"
  echo "GRAPHICS RENDERER set to: ${GRENDERER}"
  echo "INTERNAL RESOLUTION set to: ${IRES}"
  echo "FPS set to: ${FPS}"
  echo "SKIP BUFFER EFFECTS set to: ${SKIPB}"
  echo "VSYNC set to: ${VSYNC}"
  echo "Launching /usr/bin/ppsspp ${ARG}"

### Zenith exit guard (inicio)
# Objetivo: que PPSSPP guarde su cache de sombreadores al cerrarlo desde fuera (combinacion de salida, QUIT GAME de zfront, bateria), sin
# dejarlo nunca colgado. El cierre de siempre era `killall -9 ppsspp`: PPSSPP no puede atrapar SIGKILL y nunca guardaba. Ahora el fichero de
# muerte dice `-TERM ppsspp` (PPSSPP sale limpio en 1-4 s: SDL convierte SIGTERM en SDL_QUIT) y este vigilante garantiza que muere:
#  - BALIZA: un proceso hermano que se llama «zppsspp-exit» (script en ${ZG_DIR}) y va en el fichero de muerte con ppsspp y vive mientras viva este lanzador. `killall -TERM ppsspp zppsspp-exit` (que no
#    cambia en input_sense ni en zfront) alcanza a la baliza y a PPSSPP a la vez; la baliza no atrapa la senal, asi que muere al instante y
#    el vigilante sabe que alguien pidio cerrar (o lo ve pendiente, si zfront la tenia parada con SIGSTOP en el menu de pausa).
#  - VIGILANTE: si PPSSPP no ha salido ZG_GRACE segundos despues, `kill -9` (tambien a uno parado con SIGSTOP o colgado).
# Si no se puede crear la baliza queda el `-9 ppsspp` de siempre (set_kill del principio).
ZG_DIR="${ZG_DIR:-/tmp/.zppsspp-guard}"
ZG_GRACE="${ZG_GRACE:-7}"
ZG_BPID=""; ZG_WPID=""; ZG_LPID=""; ZG_TERMED=0

# vivo = existe y no es zombi (un hijo muerto sin recoger responde a kill -0)
zg_alive() {
  ZG_ST=$(sed -n 's/^State:[[:space:]]*\(.\).*/\1/p' "/proc/$1/status" 2>/dev/null)
  [ -n "${ZG_ST}" ] && [ "${ZG_ST}" != Z ]
}

zg_beacon() {
  mkdir -p "${ZG_DIR}" 2>/dev/null || return 1
  { cat >"${ZG_DIR}/zppsspp-exit" <<'ZGBEACON'
#!/bin/sh
# baliza de start_ppsspp.sh: vive mientras viva el lanzador ($1); se llama "zppsspp-exit" y va en el fichero de muerte junto a ppsspp
while [ -d "/proc/$1" ]; do sleep 1; done
ZGBEACON
  } 2>/dev/null || return 1
  chmod 755 "${ZG_DIR}/zppsspp-exit" 2>/dev/null || return 1
  "${ZG_DIR}/zppsspp-exit" "${ZG_LPID}" </dev/null >/dev/null 2>&1 &
  ZG_BPID=$!
  # si no arranca (p. ej. /tmp montado noexec) quedaria zombi y parecería «alguien pidio cerrar»: se comprueba y se descarta
  sleep 0.5
  if ! zg_alive "${ZG_BPID}"; then
    kill -9 "${ZG_BPID}" 2>/dev/null
    ZG_BPID=""
    return 1
  fi
  return 0
}

# 0 = alguien pidio cerrar: la baliza murio, o tiene SIGTERM pendiente (parada con SIGSTOP)
zg_requested() {
  [ -n "${ZG_BPID}" ] || return 1
  zg_alive "${ZG_BPID}" || return 0
  ZG_PND=$(sed -n 's/^ShdPnd:[[:space:]]*//p' "/proc/${ZG_BPID}/status" 2>/dev/null)
  [ -n "${ZG_PND}" ] && [ $(( 0x${ZG_PND} & 0x4000 )) -ne 0 ]
}

zg_watch() {
  while zg_alive "$1"; do
    zg_alive "${ZG_LPID}" || return 0
    if zg_requested; then
      # zfront para (SIGSTOP) a todo lo del fichero de muerte mientras el menu Home esta abierto y un proceso parado no atiende SIGTERM:
      # se reanuda ya, para que salga limpio y guarde el cache, y solo entonces empieza la gracia
      kill -CONT "$1" "${ZG_BPID}" 2>/dev/null
      ZG_I=0
      while [ "${ZG_I}" -lt "${ZG_GRACE}" ] && zg_alive "$1"; do sleep 1; ZG_I=$((ZG_I + 1)); done
      if zg_alive "$1"; then
        logger -t start_ppsspp "ppsspp sigue vivo ${ZG_GRACE} s despues de pedirle que cierre: kill -9"
        kill -9 "$1" 2>/dev/null
      fi
      return 0
    fi
    sleep 1
  done
}

zg_cleanup() {
  [ -n "${ZG_WPID}" ] && kill "${ZG_WPID}" 2>/dev/null
  [ -n "${ZG_BPID}" ] && kill -9 "${ZG_BPID}" 2>/dev/null
  ZG_WPID=""; ZG_BPID=""
}

# zg_run COMANDO...: lanza PPSSPP bajo el vigilante; deja su codigo en RC y ZG_TERMED=1 si alguien pidio cerrar
zg_run() {
  ZG_LPID=$$; ZG_TERMED=0
  trap 'zg_cleanup' EXIT
  trap 'exit 143' TERM HUP INT
  if zg_beacon; then
    set_kill set "-TERM ppsspp zppsspp-exit"
  else
    logger -t start_ppsspp "no se pudo crear la baliza ${ZG_DIR}/zppsspp-exit: cierre con -9 como antes (sin cache de sombreadores)"
  fi
  "$@" &
  ZG_PID=$!
  if [ -n "${ZG_BPID}" ]; then
    zg_watch "${ZG_PID}" &
    ZG_WPID=$!
  fi
  wait "${ZG_PID}"
  RC=$?
  zg_requested && ZG_TERMED=1
  zg_cleanup
}
### Zenith exit guard (fin)

zg_run ${EMUPERF} ppsspp --pause-menu-exit "${ARG}"
command -v zvk_after_run >/dev/null 2>&1 && zvk_after_run "${RC}" "${ZG_TERMED}"
exit "${RC}"
