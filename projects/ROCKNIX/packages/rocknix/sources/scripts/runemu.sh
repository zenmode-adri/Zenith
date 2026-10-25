#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2019-present Shanti Gilbert (https://github.com/shantigilbert)
# Copyright (C) 2023 JELOS (https://github.com/JustEnoughLinuxOS)

# Source predefined functions and variables
. /etc/profile
. /etc/os-release

# "runemu.sh --prepare <mismos argumentos>": zfront lo lanza cuando el cursor lleva ~1 s sobre un juego. Solo hace la
# parte de setsettings.sh (config de RetroArch) y deja una marca; el lanzamiento real la reutiliza si nada ha cambiado.
# No toca gobernadores, Bluetooth ni pantalla.
if [ "$1" = "--prepare" ]; then
  PREPARE=1
  shift
fi

### Switch to performance mode early to speed up configuration and reduce time it takes to get into games.
[ -n "${PREPARE}" ] || performance


# Command line schema
# $1 = Game/Port
# $2 = Platform
# $3 = Core
# $4 = Emulator

ARGUMENTS="$@"
PLATFORM="${ARGUMENTS##*-P}"  # read from -P onwards
PLATFORM="${PLATFORM%% *}"  # until a space is found
CORE="${ARGUMENTS##*--core=}"  # read from --core= onwards
CORE="${CORE%% *}"  # until a space is found
EMULATOR="${ARGUMENTS##*--emulator=}"  # read from --emulator= onwards
EMULATOR="${EMULATOR%% *}"  # until a space is found
ROMNAME="$1"
BASEROMNAME=${ROMNAME##*/}
GAMEFOLDER="${ROMNAME//${BASEROMNAME}}"

### Define the variables used throughout the script
BLUETOOTH_STATE=${ get_setting controllers.bluetooth.enabled; }
ES_CONFIG="/storage/.emulationstation/es_settings.cfg"
VERBOSE=false
LOG_DIRECTORY="/var/log"
LOG_FILE="exec.log"
RUN_SHELL="/usr/bin/bash"
RETROARCH_TEMP_CONFIG="/storage/.config/retroarch/retroarch.cfg"
RETROARCH_APPEND_CONFIG="/tmp/.retroarch.cfg"
NETWORK_PLAY="No"
SET_SETTINGS_TMP="/tmp/shader"
OUTPUT_LOG="${LOG_DIRECTORY}/${LOG_FILE}"
SCRIPT_NAME="${0##*/}"   # sin basename (un proceso menos)

### Export Game Guide Path
GAME_GUIDE_PATH_CHECK="${1%.*}.txt"
if [ ! -f "${GAME_GUIDE_PATH_CHECK}" ]; then
  GAME_GUIDE_PATH_CHECK="No Game Guide Found"
fi
  # game-guides-tool con argumento en linea (arrancaba un bash completo + /etc/profile, ~65 ms, solo
  # para comprobar si existe el .txt): mismo efecto sobre /storage/.config/sdl2text/game_guide.path
  [ -d /storage/.config/sdl2text ] || mkdir -p /storage/.config/sdl2text
  if [ -f "${GAME_GUIDE_PATH_CHECK}" ]; then
    echo "${GAME_GUIDE_PATH_CHECK}" > /storage/.config/sdl2text/game_guide.path
  else
    [ -e /storage/.config/sdl2text/game_guide.path ] && rm -f /storage/.config/sdl2text/game_guide.path
  fi

### Function Library
function log() {
        if [ ${LOG} == true ]
        then
                if [[ ! -d "$LOG_DIRECTORY" ]]
                then
                        mkdir -p "$LOG_DIRECTORY"
                fi
                # sin fork de tee (11 por lanzamiento): imprime y anade al fichero
                local _m="${SCRIPT_NAME}: $*"
                echo "${_m}"
                echo "${_m}" >> ${LOG_DIRECTORY}/${LOG_FILE}
        else
                echo "${SCRIPT_NAME}: $*"
        fi
}

function loginit() {
        if [ ${LOG} == true ]
        then
                # sin rm ni cat (dos procesos): el redireccionamiento ya trunca el fichero
                while IFS= read -r _l; do printf '%s\n' "${_l}"; done <<EOF >${LOG_DIRECTORY}/${LOG_FILE}
Emulation Run Log - Started at $(printf '%(%a %b %e %H:%M:%S %Z %Y)T' -1)

ARG1: $1
ARG2: $2
ARG3: $3
ARG4: $4
ARGS: $*
EMULATOR: ${EMULATOR}
PLATFORM: ${PLATFORM}
CORE: ${CORE}
ROM NAME: ${ROMNAME}
BASE ROM NAME: ${ROMNAME##*/}
USING CONFIG: ${RETROARCH_TEMP_CONFIG}
USING APPENDCONFIG : ${RETROARCH_APPEND_CONFIG}
GAME GUIDE PATH: ${GAME_GUIDE_PATH_CHECK}

EOF
        else
                log $0 "Emulation Run Log - Started at $(printf '%(%a %b %e %H:%M:%S %Z %Y)T' -1)"
        fi
}

function quit() {
        ${VERBOSE} && log $0 "Cleaning up and exiting"
        bluetooth enable
        set_kill set "emulationstation"
        clear_screen
        DEVICE_CPU_GOVERNOR=${ get_setting system.cpugovernor; }
        ${DEVICE_CPU_GOVERNOR}
        exit $1
}

function clear_screen() {
        ${VERBOSE} && log $0 "Clearing screen"
        printf '\033[H\033[2J\033[3J'   # lo que hace "clear" (ncurses), sin lanzar el proceso (13 ms x2)
}

function bluetooth() {
        if [ "$1" == "disable" ]
        then
                ${VERBOSE} && log $0 "Disabling BT"
                if [[ "${BLUETOOTH_STATE}" == "1" ]]
                then
                        NPID=$(pgrep -f rocknix-bluetooth-agent)
                        if [[ ! -z "$NPID" ]]; then
                                kill "$NPID"
                        fi
                fi
        elif [ "$1" == "enable" ]
        then
                ${VERBOSE} && log $0 "Enabling BT"
                if [[ "${BLUETOOTH_STATE}" == "1" ]]
                then
                        systemd-run rocknix-bluetooth-agent
                fi
        fi
}

### --- Preparacion en reposo de la config de RetroArch -------------------------------------------------------------
PREP_DIR="/run/zf-prep"
PREP_LOCK="/tmp/.zf-prep.lock"

# CONTROLLERCONFIG / SNAPSHOT / AUTOSAVE a partir de los argumentos (RetroArch)
function ra_parse_args() {
  CONTROLLERCONFIG="${ARGUMENTS#*--controllers=*}"

  if [[ "${ARGUMENTS}" == *"-state_slot"* ]]
  then
    CONTROLLERCONFIG="${CONTROLLERCONFIG%% -state_slot*}"  # until -state is found
    SNAPSHOT="${ARGUMENTS#*-state_slot *}" # -state_slot x
    SNAPSHOT="${SNAPSHOT%% -*}"
      if [[ "${ARGUMENTS}" == *"-autosave"* ]]; then
        CONTROLLERCONFIG="${CONTROLLERCONFIG%% -autosave*}"  # until -autosave is found
        AUTOSAVE="${ARGUMENTS#*-autosave *}" # -autosave x
        AUTOSAVE="${AUTOSAVE%% -*}"
      else
        AUTOSAVE=""
      fi
  else
    CONTROLLERCONFIG="${CONTROLLERCONFIG%% --*}"  # until a -- is found
    SNAPSHOT=""
    AUTOSAVE=""
  fi
}

# Ejecuta setsettings.sh en proceso y deja su salida en EXTRAOPTS
function run_setsettings() {
  ${VERBOSE} && log $0 "Execute setsettings (${PLATFORM} ${ROMNAME} ${CORE} --controllers=${CONTROLLERCONFIG} --autosave=${AUTOSAVE} --snapshot=${SNAPSHOT})"
  # setsettings.sh se carga en proceso (subshell): sin bash nuevo, sin /etc/profile ni gs_load otra vez (~150 ms)
  (SETSETTINGS_INPROC=1; . /usr/bin/setsettings.sh "${PLATFORM}" "${ROMNAME}" "${CORE}" --controllers="${CONTROLLERCONFIG}" --autosave="${AUTOSAVE}" --snapshot="${SNAPSHOT}" >${SET_SETTINGS_TMP})

  ### If setsettings wrote data in the background, grab it and assign it to EXTRAOPTS
  if [ -e "${SET_SETTINGS_TMP}" ]
  then
    EXTRAOPTS=$(<${SET_SETTINGS_TMP})
    ${VERBOSE} && log $0 "Extra Options: ${EXTRAOPTS}"
  fi
}

# 0 si hay una preparacion de ESTOS argumentos y ni system.cfg, ni retroarch.cfg, ni es_settings.cfg, ni la config
# generada han cambiado desde que acabo (la marca se escribe la ultima: su fecha es la referencia).
function prep_valid() {
  local n=0 p f
  while [ -d "${PREP_LOCK}" ] && [ ${n} -lt 60 ]   # preparacion en curso: espera (<= ~1 s)
  do
    p=""; [ -r "${PREP_LOCK}/pid" ] && read -r p <"${PREP_LOCK}/pid"
    [ -n "${p}" ] && [ ! -d "/proc/${p}" ] && break   # el preparador murio: bloqueo huerfano
    read -rt 0.02 <> <(:) ||:
    n=$((n+1))
  done
  [ -f "${PREP_DIR}/marker" ] && [ -f "${PREP_DIR}/extraopts" ] && [ -e "${RETROARCH_APPEND_CONFIG}" ] || return 1
  read -r p <"${PREP_DIR}/marker"
  [ "${p}" = "${ARGUMENTS}" ] || return 1
  for f in "${J_CONF}" "${RETROARCH_TEMP_CONFIG}" "${ES_CONFIG}" "${RETROARCH_APPEND_CONFIG}"
  do
    [ -e "${f}" ] && [ "${f}" -nt "${PREP_DIR}/marker" ] && return 1
  done
  return 0
}

if [ -n "${PREPARE}" ]; then
  # solo RetroArch, y sin los casos con efectos fuera de la config (atomiswave borra ficheros, scummvm cambia de
  # directorio, netplay toca system.cfg): esos se configuran en el lanzamiento como siempre
  [ "${EMULATOR}" = "retroarch" ] || exit 0
  case "${PLATFORM}" in atomiswave|scummvm) exit 0 ;; esac
  case "${ARGUMENTS}" in *"--host"*|*"--connect"*|*"--netplaymode"*) exit 0 ;; esac
  mkdir "${PREP_LOCK}" 2>/dev/null || exit 0   # otra preparacion en curso
  echo "$$" >"${PREP_LOCK}/pid"
  mkdir -p "${PREP_DIR}"
  rm -f "${PREP_DIR}/marker"
  del_setting netplay.mode
  ra_parse_args
  run_setsettings
  printf '%s' "${EXTRAOPTS}" >"${PREP_DIR}/extraopts"
  printf '%s\n' "${ARGUMENTS}" >"${PREP_DIR}/marker"
  rm -f "${PREP_LOCK}/pid"
  rmdir "${PREP_LOCK}"
  exit 0
fi

if [[ ${PLATFORM} == "psx" ]]; then
	ROMNAMETMP=${ROMNAME%.*}
	if [ -f "${ROMNAMETMP}.bios" ]; then
		cp -f "${ROMNAMETMP}.bios" "/storage/roms/bios/scph101.bin"
		cp -f "${ROMNAMETMP}.bios" "/storage/roms/bios/scph5500.bin"
		cp -f "${ROMNAMETMP}.bios" "/storage/roms/bios/scph5501.bin"
		cp -f "${ROMNAMETMP}.bios" "/storage/roms/bios/scph5502.bin"
		cp -f "${ROMNAMETMP}.bios" "/storage/roms/bios/psxonpsp660.bin"
	else
		cp -f "/usr/share/psxbios/scph7001.bios" "/storage/roms/bios/scph101.bin"
		cp -f "/usr/share/psxbios/scph5500.bios" "/storage/roms/bios/scph5500.bin"
		cp -f "/usr/share/psxbios/scph5501.bios" "/storage/roms/bios/scph5501.bin"
		cp -f "/usr/share/psxbios/scph5502.bios" "/storage/roms/bios/scph5502.bin"
		cp -f "/usr/share/psxbios/psxonpsp660.bios" "/storage/roms/bios/psxonpsp660.bin"
	fi
fi

### Enable logging
case ${ get_setting system.loglevel; } in
  off|none)
    LOG=false
  ;;
  verbose)
    LOG=true
    VERBOSE=true
  ;;
  *)
    LOG=true
  ;;
esac

### Prepare to load our emulator and game.
loginit "$1" "$2" "$3" "$4"
clear_screen
bluetooth disable
set_kill stop

### Determine which emulator we're launching and make appropriate adjustments before launching.
${VERBOSE} && log $0 "Configuring for ${EMULATOR}"
case ${EMULATOR} in
  mednafen)
    set_kill set "-9 mednafen"
    RUNTHIS='${RUN_SHELL} /usr/bin/start_mednafen.sh "${ROMNAME}" "${CORE}" "${PLATFORM}"'
  ;;
  retroarch)
    # Make sure NETWORK_PLAY isn't defined before we start our tests/configuration.
    del_setting netplay.mode

    case ${ARGUMENTS} in
      *"--host"*)
        ${VERBOSE} && log $0 "Setup netplay host."
        NETWORK_PLAY="${ARGUMENTS##*--host}"  # read from --host onwards
        NETWORK_PLAY="${NETWORK_PLAY%%--nick*}"  # until --nick is found
        NETWORK_PLAY="--host ${NETWORK_PLAY} --nick"
        set_setting netplay.mode "host"
      ;;
      *"--connect"*)
        ${VERBOSE} && log $0 "Setup netplay client."
        NETWORK_PLAY="${ARGUMENTS##*--connect}"  # read from --connect onwards
        NETWORK_PLAY="${NETWORK_PLAY%%--nick*}"  # until --nick is found
        NETWORK_PLAY="--connect ${NETWORK_PLAY} --nick"
        set_setting netplay.mode "client"
      ;;
      *"--netplaymode spectator"*)
        ${VERBOSE} && log $0 "Setup netplay spectator."
        set_setting "netplay.mode" "spectator"
      ;;
    esac

    ### Set set_kill to kill the appropriate retroarch
    set_kill set "retroarch retroarch32"

    ### Sin complementos de libdecor: los juegos van a pantalla completa y no necesitan decoraciones de ventana.
    ### Sin esto libdecor carga libdecor-gtk.so (GTK3, glib, xkbcommon...) en cada arranque de RetroArch:
    ### ~0,5 s de CPU en la R36 (medido con perf: libgtk-3 13 % de la fase de ventana). Un directorio sin
    ### plugins hace que libdecor responda "No plugins found, falling back on no decorations".
    ### Solo RetroArch: es lo que se ha validado (los emuladores independientes no se han probado).
    export LIBDECOR_PLUGIN_DIR="${LIBDECOR_PLUGIN_DIR:-/usr/lib/libdecor/no-plugins}"

    ### Assume we're running 64bit Retroarch
    RABIN="retroarch"

    case ${HW_ARCH} in
      aarch64)
        if [[ "${CORE}" =~ pcsx_rearmed32 ]] || \
           [[ "${CORE}" =~ gpsp ]] || \
           [[ "${CORE}" =~ desmume ]] || \
	   [[ "${CORE}" == *"_32b"*  ]]
        then
          ### Configure for 32bit Retroarch
          ${VERBOSE} && log $0 "Configuring for 32bit cores."
          export RABIN="retroarch32"
        fi
      ;;
    esac


    ### Configure specific emulator requirements
    case ${CORE} in
      freej2me*)
        ${VERBOSE} && log $0 "Setup freej2me requirements."
        /usr/bin/freej2me.sh
        JAVA_HOME='/storage/jdk'
        export JAVA_HOME
        PATH="$JAVA_HOME/bin:$PATH"
        export PATH
        export _JAVA_OPTIONS="-Djava.awt.headless=true"
      ;;
      easyrpg*)
        # easyrpg needs runtime files to be downloaded on the first run
        ${VERBOSE} && log $0 "Setup easyrpg requirements."
        /usr/bin/easyrpg.sh
      ;;
    esac


    RUNTHIS='${EMUPERF} /usr/bin/${RABIN} -L /tmp/cores/${CORE}_libretro.so --config ${RETROARCH_TEMP_CONFIG} --appendconfig ${RETROARCH_APPEND_CONFIG} "${ROMNAME}"'

    ra_parse_args

    # Configure platform specific requirements
    case ${PLATFORM} in
      "atomiswave")
        rm ${ROMNAME}.nvmem*
      ;;
      "scummvm")
        GAMEDIR=$(cat "${ROMNAME}" | awk 'BEGIN {FS="\""}; {print $2}')
        cd "${GAMEDIR}"
        RUNTHIS='${RUN_SHELL} /usr/bin/start_scummvm.sh libretro .'
      ;;
    esac

    ### Configure retroarch (si zfront ya lo preparo con --prepare y nada ha cambiado, se reutiliza)
    if prep_valid
    then
      EXTRAOPTS=$(<"${PREP_DIR}/extraopts")
      ${VERBOSE} && log $0 "Using prepared RetroArch settings: ${EXTRAOPTS}"
    else
      run_setsettings
    fi

    if [[ ${EXTRAOPTS} != 0 ]]; then
      RUNTHIS="${RUNTHIS/--config/${EXTRAOPTS} --config}"   # sin echo|sed (dos procesos)
    fi
  ;;
  *)
    case ${PLATFORM} in
      "setup")
        RUNTHIS='${RUN_SHELL} "${ROMNAME}"'
      ;;
      "gamecube"|"triforce")
        RUNTHIS='${RUN_SHELL} /usr/bin/start_dolphin_gc.sh "${ROMNAME}" "${PLATFORM}" "${CORE}"'
      ;;
      "wii"|"wiiware")
        RUNTHIS='${RUN_SHELL} /usr/bin/start_dolphin_wii.sh "${ROMNAME}" "${PLATFORM}" "${CORE}"'
      ;;
      "ports")
        RUNTHIS='${EMUPERF} ${RUN_SHELL} "${ROMNAME}"'
	      sed -i "/^ACTIVE_GAME=/c\ACTIVE_GAME=\"${ROMNAME}\"" /storage/.config/PortMaster/mapper.txt
        sed -i "/^ACTIVE_PLATFORM=/c\ACTIVE_PLATFORM=\"${PLATFORM}\"" /storage/.config/PortMaster/mapper.txt
      ;;
      "windows")
        RUNTHIS='${EMUPERF} ${RUN_SHELL} "${ROMNAME}"'
        # Hook into Portmaster control mapping
        sed -i "/^ACTIVE_GAME=/c\ACTIVE_GAME=\"${ROMNAME}\"" /storage/.config/PortMaster/mapper.txt
        sed -i "/^ACTIVE_PLATFORM=/c\ACTIVE_PLATFORM=\"${PLATFORM}\"" /storage/.config/PortMaster/mapper.txt
      ;;
      "shell")
        RUNTHIS='${RUN_SHELL} "${ROMNAME}"'
      ;;
      "nds"|"OpenBOR"|"dreamcast"|"atomiswave"|"naomi")
        RUNTHIS='${RUN_SHELL} "/usr/bin/start_${EMULATOR}.sh" "${ROMNAME}" "${PLATFORM}" "${CORE}" "${EMULATOR}"'
      ;;
      *)
        RUNTHIS='${RUN_SHELL} "/usr/bin/start_${CORE%-*}.sh" "${ROMNAME}" "${PLATFORM}" "${CORE}" "${EMULATOR}"'
      ;;
    esac
  ;;
esac

### Execution time.
clear_screen
${VERBOSE} && log $0 "executing game: ${ROMNAME}"
${VERBOSE} && log $0 "script to execute: ${RUNTHIS}"

### Set the cores to use
CORES=${ get_setting "cores" "${PLATFORM}" "${ROMNAME##*/}"; }
${VERBOSE} && log $0 "Configure big.little (${CORES})"
case ${CORES} in
  little)
    EMUPERF="${SLOW_CORES}"
  ;;
  big)
    EMUPERF="${FAST_CORES}"
  ;;
  *)
    unset EMUPERF
  ;;
esac

### We need the original system cooling profile later so get it now!
COOLINGPROFILE=${ get_setting cooling.profile; }

### Configure GPU performance mode
GPUPERF=${ get_setting "gpuperf" "${PLATFORM}" "${ROMNAME##*/}"; }
if [ ! -z ${GPUPERF} ]
then
  ${VERBOSE} && log $0 "Set GPU performance to (${GPUPERF})"
  gpu_performance_level ${GPUPERF}
  get_gpu_performance_level >/tmp/.gpu_performance_level
fi

if [ "${DEVICE_HAS_FAN}" = "true" ]
then
  ### Set any custom fan profile (make this better!)
  GAMEFAN=${ get_setting "cooling.profile" "${PLATFORM}" "${ROMNAME##*/}"; }
  if [ ! -z "${GAMEFAN}" ]
  then
    ${VERBOSE} && log $0 "Set fan profile to (${GAMEFAN})"
    set_setting cooling.profile ${GAMEFAN}
    systemctl restart fancontrol
  fi
fi

### Display mode for emulation
DISPLAY_MODE=${ get_setting "display_mode" "${PLATFORM}" "${ROMNAME##*/}"; }
if [ ! -z "${DISPLAY_MODE}" ] && [ "${DISPLAY_MODE}" != "default" ]
then
  set_refresh_rate "${DISPLAY_MODE}"
fi

FORCEPACK=${ get_setting "forcepack" "${PLATFORM}" "${ROMNAME##*/}"; }
if [ ! -z "${FORCEPACK}" ] && [ "${FORCEPACK}" = "On" ]
then
    ${VERBOSE} && log $0 "Enabling panfrost forcepack"
    export PAN_MESA_DEBUG=forcepack
fi

### Offline all but the number of threads we need for this game if configured.
NUMTHREADS=${ get_setting "threads" "${PLATFORM}" "${ROMNAME##*/}"; }
if [ -n "${NUMTHREADS}" ] &&
   [ ! ${NUMTHREADS} = "default" ]
then
  ${VERBOSE} && log $0 "Configure active cores (${NUMTHREADS})"
  onlinethreads ${NUMTHREADS} 0
fi

### Set the governor mode for emulation
CPU_GOVERNOR=${ get_setting "cpugovernor" "${PLATFORM}" "${ROMNAME##*/}"; }
${VERBOSE} && log $0 "Set emulation performance mode to (${CPU_GOVERNOR})"
${CPU_GOVERNOR}

### Check whether MangoHud is supported and enabled
if [ "${DEVICE_MANGOHUD_SUPPORT}" == "true" ]; then
  MANGOHUD_ENABLED=${ get_setting "rocknix.mangohud.enabled"  "${PLATFORM}" "${ROMNAME##*/}"; }
  if [ "${MANGOHUD_ENABLED}" = "1" ]; then
    # Enable GPU profiling and MangoHud
    gpu_profiling "on"
    RUNTHIS="/usr/bin/mangohud ${RUNTHIS}"
    ${VERBOSE} && log $0 "Enabling MangoHud"
  fi
fi

# If the rom is a shell script just execute it, useful for DOSBOX and ScummVM scan scripts
if [[ "${ROMNAME}" == *".sh" ]] && [ ! "${PLATFORM}" = "ports" ] && [ ! "${PLATFORM}" = "windows" ]; then
        ${VERBOSE} && log $0 "Executing shell script ${ROMNAME}"
        "${ROMNAME}" &>>${OUTPUT_LOG}
        ret_error=$?
else
        ${VERBOSE} && log $0 "Executing $(eval echo ${RUNTHIS})"
        eval ${RUNTHIS} &>>${OUTPUT_LOG}
        ret_error=$?
fi

### Switch back to performance mode to clean up
performance

clear_screen

### Disable touch on the secondary screen for dual screen devices
if [[ "${DEVICE_HAS_DUAL_SCREEN}" == "true" ]]; then
  # Disable touch events for Retroid Pocket devices to prevent focus loss
  if [[ "${QUIRK_DEVICE}" == "Retroid Pocket 5" || "${QUIRK_DEVICE}" == "Retroid Pocket Flip2" || "${QUIRK_DEVICE}" == "Retroid Pocket Mini" || "${QUIRK_DEVICE}" == "Retroid Pocket Mini V2" ]]; then
    swaymsg input "0:0:generic_ft5x06_(a0)" events disabled
    swaymsg input "0:0:generic_ft5x06_(8d)" events disabled
  fi
fi

### Go back to system display mode , if we had specialized mode defined
DISPLAY_MODE=${ get_setting "display_mode" "${PLATFORM}" "${ROMNAME##*/}"; }
if [ ! -z "${DISPLAY_MODE}" ] && [ "${DISPLAY_MODE}" != "default" ]
then
  DISPLAY_MODE=${ get_setting "system.display_mode"; }
  DISPLAY_OUTPUT=$(/usr/bin/wlr-randr | awk 'NR==1{print $1;}')
  if [ -z "${DISPLAY_MODE}" ]; then
    # if we have no system mode use the displays preferred mode
    /usr/bin/wlr-randr --output ${DISPLAY_OUTPUT} --preferred
  else
    # If we have user specifed system mode set that
    set_refresh_rate "${DISPLAY_MODE}"
  fi
fi

### Restore cooling profile.
if [ "${DEVICE_HAS_FAN}" = "true" ]
then
  ${VERBOSE} && log $0 "Restore system cooling profile (${COOLINGPROFILE})"
  set_setting cooling.profile ${COOLINGPROFILE}
  systemctl restart fancontrol &
fi

### Restore system GPU performance mode
GPUPERF=${ get_setting "system.gpuperf"; }
if [ ! -z ${GPUPERF} ]
then
  ${VERBOSE} && log $0 "Restore system GPU performance mode (${GPUPERF})"
  gpu_performance_level ${GPUPERF} &
else
  ${VERBOSE} && log $0 "Restore system GPU performance mode (auto)"
  gpu_performance_level auto &
fi
[ -e /tmp/.gpu_performance_level ] && rm -f /tmp/.gpu_performance_level 2>/dev/null

### Reset the number of cores to use.
NUMTHREADS=${ get_setting "system.threads"; }
${VERBOSE} && log $0 "Restore active threads (${NUMTHREADS})"
if [ -n "${NUMTHREADS}" ]
then
        onlinethreads ${NUMTHREADS} 0 &
else
        onlinethreads all 1 &
fi

### Disable GPU profiling
gpu_profiling "off"

### Backup save games
CLOUD_BACKUP=${ get_setting "cloud.backup"; }
if [ "${CLOUD_BACKUP}" = "1" ]
then
  INETUP=$(/usr/bin/amionline >/dev/null 2>&1)
  if [ $? == 0 ]
  then
    log $0 "backup saves to the cloud."
    /usr/bin/run /usr/bin/cloud_backup
  fi
fi

${VERBOSE} && log $0 "Checking errors: ${ret_error} "
if [ "${ret_error}" == "0" ]
then
        quit 0
else
        log $0 "exiting with ${ret_error}"
        quit 1
fi
