#!/bin/bash

. /etc/profile
. /usr/lib/rocknix/functions

hidecursor
ROM_DIR="/storage/roms"
RACFG="/storage/.config/retroarch/retroarch.cfg"

UPDATE_MODE="$1"

function Get_display_rotation() {
    local fbcon_rotate

    if [ -r /sys/class/graphics/fbcon/rotate ]; then
        read -r fbcon_rotate </sys/class/graphics/fbcon/rotate
        case "$fbcon_rotate" in
            1) echo 90; return ;;
            2) echo 180; return ;;
            3) echo 270; return ;;
        esac
    fi

    echo 0
}

function Test_Button_A(){
  evtest --query $event_dev $event_type $event_btn_a
}

function Test_Button_B(){
  evtest --query $event_dev $event_type $event_btn_b
}


function Set_system() {
    SYSCFG="/storage/.config/system/configs/system.cfg"
    sed -i -e '/system.hostname\=/c\system.hostname\='"${1}"'' ${SYSCFG}
    sed -i -e '/system\.display_mode_hdmi\=/d' ${SYSCFG}
    sed -i -e '/system\.display_mode\=/d' ${SYSCFG}

    if [ "$UPDATE_MODE" != "device_change" ]; then
        sed -i -e '/audio.volume\=/c\audio.volume\=60' ${SYSCFG}
        sed -i -e '/rotate.root.password\=/c\rotate.root.password\=0' ${SYSCFG}
        sed -i -e '/samba.enabled\=/c\samba.enabled\=0' ${SYSCFG}
        [ -e /usr/config/zenith-debug-build ] || sed -i -e '/ssh.enabled\=/c\ssh.enabled\=0' ${SYSCFG}
        sed -i -e '/updates.enabled\=/c\updates.enabled\=0' ${SYSCFG}
        sed -i -e '/system.autohotkeys\=/c\system.autohotkeys\=0' ${SYSCFG}
        sed -i -e '/global.retroarch.menu_driver\=/c\global.retroarch.menu_driver\=ozone' ${SYSCFG}
    fi
}


function Set_system_cfg_entry() {
    local key="${1}"
    local value="${2}"
    local syscfg="/storage/.config/system/configs/system.cfg"
    local escaped_key="${key//./\.}"

    if grep -q "^${escaped_key}=" "${syscfg}"; then
        sed -i -e "/^${escaped_key}=/c\\${key}=${value}" "${syscfg}"
    else
        printf '%s=%s\n' "${key}" "${value}" >> "${syscfg}"
    fi
}

function Set_ra_ext() {
	gamecontrollerdb="/storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt"

	# 通过joyguid获取GUID
	guid=$(joyguid 2>/dev/null | tr -d '\n')

	# 异常处理
	if [ -z "$guid" ]; then
		echo "错误：无法获取Joystick GUID，请检查joyguid工具" >&2
		exit 1
	fi

	# 查找匹配行
	mapping_line=$(grep -m1 "^${guid}," "$gamecontrollerdb")
	if [ -z "$mapping_line" ]; then
		echo "错误：未找到GUID $guid 对应的控制器配置" >&2
		exit 1
	fi

	# 解析并生成带前缀的变量
	eval "$(
	echo "$mapping_line" | awk -F, '
	{
		for(i=1; i<=NF; i++) {
			if($i ~ /^[a-zA-Z]+:b[0-9]+$/) {
				split($i, pair, ":")
				key = pair[1]
				value = substr(pair[2],2)  # 去掉b前缀
				printf "declare -g mapped_%s=%d\n", key, value  # 添加前缀
			}
		}
	}'
	)"

	if [ "$HOTKEY" = "guide" ] && [ ! -z "${mapped_guide}" ]; then
		sed -i -e '/input_enable_hotkey_btn\ \=/c\input_enable_hotkey_btn\ \=\ \"'${mapped_guide}'\"' ${RACFG}
	else
		sed -i -e '/input_enable_hotkey_btn\ \=/c\input_enable_hotkey_btn\ \=\ \"'${mapped_back}'\"' ${RACFG}
	fi
	sed -i -e '/input_menu_toggle_btn\ \=/c\input_menu_toggle_btn\ \=\ \"'${mapped_x}'\"' ${RACFG}
	sed -i -e '/input_exit_emulator_btn\ \=/c\input_exit_emulator_btn\ \=\ \"'${mapped_start}'\"' ${RACFG}
	sed -i -e '/input_toggle_fast_forward_btn\ \=/c\input_toggle_fast_forward_btn\ \=\ \"'${mapped_righttrigger}'\"' ${RACFG}
	sed -i -e '/input_toggle_slowmotion_btn\ \=/c\input_toggle_slowmotion_btn\ \=\ \"'${mapped_lefttrigger}'\"' ${RACFG}
	if [ ! -z "${mapped_leftstick}" ]; then
		sed -i -e '/input_rewind_btn\ \=/c\input_rewind_btn\ \=\ \"'${mapped_leftstick}'\"' ${RACFG}
	else
		sed -i -e '/input_rewind_btn\ \=/c\input_rewind_btn\ =' ${RACFG}
	fi
	sed -i -e '/input_pause_toggle_btn\ \=/c\input_pause_toggle_btn\ \=\ \"'${mapped_a}'\"' ${RACFG}
	sed -i -e '/input_load_state_btn\ \=/c\input_load_state_btn\ \=\ \"'${mapped_rightshoulder}'\"' ${RACFG}
	sed -i -e '/input_save_state_btn\ \=/c\input_save_state_btn\ \=\ \"'${mapped_leftshoulder}'\"' ${RACFG}
	sed -i -e '/input_state_slot_increase_btn\ \=/c\input_state_slot_increase_btn\ \=\ \"'${mapped_dpup}'\"' ${RACFG}
	sed -i -e '/input_state_slot_decrease_btn\ \=/c\input_state_slot_decrease_btn\ \=\ \"'${mapped_dpdown}'\"' ${RACFG}
	sed -i -e '/input_screenshot_btn\ \=/c\input_screenshot_btn\ \=\ \"'${mapped_b}'\"' ${RACFG}
	sed -i -e '/input_fps_toggle_btn\ \=/c\input_fps_toggle_btn\ \=\ \"'${mapped_y}'\"' ${RACFG}
	sed -i -e '/menu_scale_factor\ \=/c\menu_scale_factor\ \=\ \"'${1}'\"' ${RACFG}
	sed -i -e '/menu_widget_scale_factor\ \=/c\menu_widget_scale_factor\ \=\ \"'${2}'\"' ${RACFG}
	if grep -qE '^[[:space:]]*video_gpu_screenshot[[:space:]]*=' ${RACFG}; then
		sed -i -e 's/^[[:space:]]*video_gpu_screenshot[[:space:]]*=.*/video_gpu_screenshot = "true"/' ${RACFG}
	else
		printf 'video_gpu_screenshot = "true"\n' >> ${RACFG}
	fi
}

function Setup_drastic_cheat() {
  local drastic_dir="/storage/.config/drastic"
  local cheat_file="${drastic_dir}/usrcheat.dat"
  local chs_file="${drastic_dir}/usrcheat_chs.dat"
  local backup_file="${drastic_dir}/usrcheat.dat.bak"
  local language

  language="$(get_setting language 2>/dev/null)"
  case "${language}" in
    zh_*|zh-*)
      ;;
    *)
      return 0
      ;;
  esac

  mkdir -p "${drastic_dir}"

  if [ ! -e "${chs_file}" ] && [ -e "/usr/config/drastic/usrcheat_chs.dat" ]; then
    cp -f "/usr/config/drastic/usrcheat_chs.dat" "${chs_file}"
  fi

  if [ ! -e "${chs_file}" ]; then
    return 0
  fi

  if [ -L "${cheat_file}" ] && [ "$(readlink "${cheat_file}")" = "usrcheat_chs.dat" ]; then
    return 0
  fi

  if [ -f "${cheat_file}" ] && [ ! -L "${cheat_file}" ]; then
    mv -f "${cheat_file}" "${backup_file}"
  elif [ -e "${cheat_file}" ]; then
    rm -f "${cheat_file}"
  fi

  ln -sf usrcheat_chs.dat "${cheat_file}"
}


# Zenith: pantalla de primer arranque (fotogramas zenith-*.png y script zenith-splash junto a este fichero).
# El trap EXIT hace sync al salir en cualquier caso (incluido el exit 1 de Set_ra_ext).
ZSPLASH="/usr/lib/autostart/firstboot/zenith-splash"
TTY0="/dev/tty0"
if [ "$UPDATE_MODE" != "device_change" ] && [ -x "$ZSPLASH" ]; then
    trap '"$ZSPLASH" stop; sync' EXIT
    echo "$(date): firstboot: zenith-splash start" >>/var/log/boot.log
    "$ZSPLASH" start zenith-1-install
    TTY0="/dev/null"   # los avisos de texto en tty0 se pintarian encima de la imagen (fbcon comparte fb0)
fi

if [ ! -e "/roms/bios/jdk" ]; then
  echo "Installing JDK." >"$TTY0"
  unzip -oq /usr/share/java/jdk.zip -d /roms/bios &>/dev/null
  chmod +x /roms/bios/jdk/bin/*
fi
ln -sf /roms/bios/jdk /storage/jdk

if [ ! -e "/storage/bezels" ]; then
  echo "Copy bezels." >"$TTY0"
  mkdir -p /storage/bezels
  cp /usr/share/misc/bezels_*.zip /storage/bezels/
fi

# 处理 ppsspp 字体软链接
PPSSPP_FONTS="/storage/.config/ppsspp/assets"
if [ -d "$PPSSPP_FONTS" ]; then
  for font in Roboto_Condensed-Bold.ttf Roboto_Condensed-Italic.ttf Roboto_Condensed-Light.ttf Roboto_Condensed-Regular.ttf; do
    rm -f "$PPSSPP_FONTS/$font"
    ln -s Roboto-Condensed.ttf "$PPSSPP_FONTS/$font"
  done
fi

unzip -oq /usr/share/misc/datas.zip -d /storage

if [ "$UPDATE_MODE" != "device_change" ] && { [ "$HW_DEVICE" = "RK3566" ] || [ "$HW_DEVICE" = "RK356X" ]; } && [ -f "/usr/config/modules/MOD_TOOLS/FixShutdown.sh" ]; then
  bash "/usr/config/modules/MOD_TOOLS/FixShutdown.sh"
fi

if [ -f "/usr/config/modules/MOD_TOOLS/Reset Drastic Cfg.sh" ]; then
  bash "/usr/config/modules/MOD_TOOLS/Reset Drastic Cfg.sh"
fi

if [ "$UPDATE_MODE" != "device_change" ] && [ -x "$ZSPLASH" ]; then
    "$ZSPLASH" show zenith-2-apply
fi

# Zenith: pregunta de idioma como pantalla (fotograma zenith-lang). Botones: A = English, B = Chinese, X = Espanol.
# Sin mando o sin pantalla: ingles. A los 59 s sin pulsar: ingles. (La deteccion del mando es la del original.)
event_type="EV_KEY"
event_btn_a="BTN_EAST"
event_btn_b="BTN_SOUTH"
event_btn_x="BTN_NORTH"

event_dev=""

if [ -e /dev/input/by-path/platform-*event-joystick ]; then
    joystick_dev=$(eval echo /dev/input/by-path/platform-*event-joystick)
    if [ -e "$joystick_dev" ]; then
        event_dev=$(readlink -f "$joystick_dev")
    fi
elif [ -e /dev/input/by-path/platform-*-event-mouse ]; then
    mouse_dev=$(eval echo /dev/input/by-path/platform-*-event-mouse)
    if [ -e "$mouse_dev" ]; then
        event_dev=$(readlink -f "$mouse_dev")
    fi
fi

function Test_Button_X(){
  evtest --query $event_dev $event_type $event_btn_x
}

function Zenith_set_language() {
    local cfg="/storage/.config/system/configs/system.cfg"
    sed -i -e '/system\.language\=/c system\.language\='"$1" "${cfg}"
    if [ -n "$2" ]; then
        sed -i -e '/system\.timezone\=/c system\.timezone\='"$2" "${cfg}"
    fi
    sync
}

if [ "$UPDATE_MODE" != "device_change" ]; then
    if [ -z "$event_dev" ] || [ ! -x "$ZSPLASH" ] || ! "$ZSPLASH" show zenith-lang; then
        # sin mando o sin poder pintar la pantalla: no se puede preguntar
        if [ -e "/flash/zh_CN" ]; then
            Zenith_set_language zh_CN Asia/Shanghai
        else
            Zenith_set_language en_US
        fi
    else
        zenith_lang_t0=$SECONDS
        while true
        do
            Test_Button_A
            if [ "$?" -eq "10" ]; then Zenith_set_language en_US; break; fi
            Test_Button_B
            if [ "$?" -eq "10" ]; then Zenith_set_language zh_CN Asia/Shanghai; break; fi
            Test_Button_X
            if [ "$?" -eq "10" ]; then Zenith_set_language es_ES; break; fi
            if [ $((SECONDS - zenith_lang_t0)) -ge 59 ]; then Zenith_set_language en_US; break; fi
            sleep 0.1
        done
        "$ZSPLASH" show zenith-2-apply
    fi
fi

Setup_drastic_cheat

# 获取当前输出分辨率并交换宽高的功能
TARGET_RES="1920x1080 1080x1920 1024x768 1280x720 720x1280 960x720 720x960 854x480 480x854 544x960 960x544 720x720 480x640 640x480 480x320 320x480"
RESOLUTION_RE='(1920x1080|1080x1920|1280x720|1024x768|960x720|720x960|720x1280|854x480|480x854|960x544|544x960|720x720|640x480|480x640|480x320|320x480)'

normalize_resolution() {
    local res="$1"
    local width height

    [ -n "$res" ] || return 1

    case " $TARGET_RES " in
        *" $res "*) ;;
        *) return 1 ;;
    esac

    IFS='x' read -r width height <<< "$res"
    if (( width < height )); then
        res="${height}x${width}"
    fi

    echo "$res"
}

detect_current_resolution() {
    local width height res state candidate

    if [ -r /sys/class/display/vinfo ]; then
        width="$(sed -n 's/^[[:space:]]*width:[[:space:]]*//p' /sys/class/display/vinfo | head -n1)"
        height="$(sed -n 's/^[[:space:]]*height:[[:space:]]*//p' /sys/class/display/vinfo | head -n1)"
        if [[ "$width" =~ ^[0-9]+$ && "$height" =~ ^[0-9]+$ ]] &&
           res="$(normalize_resolution "${width}x${height}")"; then
            echo "$res"
            return
        fi
    fi

    for state in /sys/kernel/debug/dri/*/state; do
        [ -r "$state" ] || continue
        while read -r candidate; do
            if res="$(normalize_resolution "$candidate")"; then
                echo "$res"
                return
            fi
        done < <(sed -n 's/.*name:\[\([0-9]\+x[0-9]\+\)p[0-9].*/\1/p' "$state")
    done

    for candidate in $(grep -oE "$RESOLUTION_RE" /sys/class/graphics/fb0/modes 2>/dev/null); do
        if res="$(normalize_resolution "$candidate")"; then
            echo "$res"
            return
        fi
    done
}

detected_res="$(detect_current_resolution)"

# Default scale factors
MENU_SCALE_FACTOR="0.400000"
MENU_WIDGET_SCALE_FACTOR="0.300000"

case "$detected_res" in
    "1920x1080")
        echo "1920x1080"
        sed -i -e '/gba.shaderset\=/c\gba.shaderset\=zfast_lcd_standard.glslp' /storage/.config/system/configs/system.cfg
        sed -i -e '/gbah.shaderset\=/c\gbah.shaderset\=zfast_lcd_standard.glslp' /storage/.config/system/configs/system.cfg
        ;;
    "1280x720")
        echo "1280x720"
        sed -i -e '/gba.shaderset\=/c\gba.shaderset\=zfast_lcd_standard.glslp' /storage/.config/system/configs/system.cfg
        sed -i -e '/gbah.shaderset\=/c\gbah.shaderset\=zfast_lcd_standard.glslp' /storage/.config/system/configs/system.cfg
        ;;
    "1024x768")
        echo "1024x768"
        Set_system_cfg_entry "famicom.ratio" "8/7"
        Set_system_cfg_entry "famicom.integerscale" "1"
        Set_system_cfg_entry "fds.ratio" "8/7"
        Set_system_cfg_entry "fds.integerscale" "1"
        Set_system_cfg_entry "nes.ratio" "8/7"
        Set_system_cfg_entry "nes.integerscale" "1"
        Set_system_cfg_entry "nesh.ratio" "8/7"
        Set_system_cfg_entry "nesh.integerscale" "1"
        Set_system_cfg_entry "ngp.integerscale" "1"
        Set_system_cfg_entry "ngpc.integerscale" "1"
        Set_system_cfg_entry "sfc.ratio" "8/7"
        Set_system_cfg_entry "sfc.integerscale" "1"
        Set_system_cfg_entry "snes.ratio" "8/7"
        Set_system_cfg_entry "snes.integerscale" "1"
        Set_system_cfg_entry "snesh.ratio" "8/7"
        Set_system_cfg_entry "snesh.integerscale" "1"
        Set_system_cfg_entry "wonderswancolor.integerscale" "1"
        ;;
    "960x720")
        echo "960x720"
        MENU_SCALE_FACTOR="0.550000"
        MENU_WIDGET_SCALE_FACTOR="0.550000"
        Set_system_cfg_entry "famicom.ratio" "8/7"
        Set_system_cfg_entry "famicom.integerscale" "1"
        Set_system_cfg_entry "fds.ratio" "8/7"
        Set_system_cfg_entry "fds.integerscale" "1"
        Set_system_cfg_entry "nes.ratio" "8/7"
        Set_system_cfg_entry "nes.integerscale" "1"
        Set_system_cfg_entry "nesh.ratio" "8/7"
        Set_system_cfg_entry "nesh.integerscale" "1"
        Set_system_cfg_entry "ngp.integerscale" "1"
        Set_system_cfg_entry "ngpc.integerscale" "1"
        Set_system_cfg_entry "sfc.ratio" "8/7"
        Set_system_cfg_entry "sfc.integerscale" "1"
        Set_system_cfg_entry "snes.ratio" "8/7"
        Set_system_cfg_entry "snes.integerscale" "1"
        Set_system_cfg_entry "snesh.ratio" "8/7"
        Set_system_cfg_entry "snesh.integerscale" "1"
        Set_system_cfg_entry "wonderswancolor.integerscale" "1"
        ;;
    "854x480")
        echo "854x480"
        Set_system_cfg_entry "famicom.ratio" "8/7"
        Set_system_cfg_entry "famicom.integerscale" "1"
        Set_system_cfg_entry "fds.ratio" "8/7"
        Set_system_cfg_entry "fds.integerscale" "1"
        Set_system_cfg_entry "nes.ratio" "8/7"
        Set_system_cfg_entry "nes.integerscale" "1"
        Set_system_cfg_entry "nesh.ratio" "8/7"
        Set_system_cfg_entry "nesh.integerscale" "1"
        Set_system_cfg_entry "ngp.integerscale" "1"
        Set_system_cfg_entry "ngpc.integerscale" "1"
        Set_system_cfg_entry "sfc.ratio" "8/7"
        Set_system_cfg_entry "sfc.integerscale" "1"
        Set_system_cfg_entry "snes.ratio" "8/7"
        Set_system_cfg_entry "snes.integerscale" "1"
        Set_system_cfg_entry "snesh.ratio" "8/7"
        Set_system_cfg_entry "snesh.integerscale" "1"
        Set_system_cfg_entry "wonderswancolor.integerscale" "1"
        ;;
    "960x544")
        echo "960x544"
        ;;
    "720x720")
        echo "720x720"
        Set_system_cfg_entry "atarilynx.ratio" "custom"
        Set_system_cfg_entry "atarilynx.integerscale" "0"
        Set_system_cfg_entry "dreamcast.ratio" "custom"
        Set_system_cfg_entry "dreamcast.integerscale" "0"
        Set_system_cfg_entry "famicom.ratio" "custom"
        Set_system_cfg_entry "famicom.integerscale" "0"
        Set_system_cfg_entry "fds.ratio" "custom"
        Set_system_cfg_entry "fds.integerscale" "0"
        Set_system_cfg_entry "gb.ratio" "core"
        Set_system_cfg_entry "gb.integerscale" "1"
        Set_system_cfg_entry "gbc.ratio" "core"
        Set_system_cfg_entry "gbc.integerscale" "1"
        Set_system_cfg_entry "gbch.ratio" "core"
        Set_system_cfg_entry "gbch.integerscale" "1"
        Set_system_cfg_entry "gbh.ratio" "core"
        Set_system_cfg_entry "gbh.integerscale" "1"
        Set_system_cfg_entry "genesis.ratio" "custom"
        Set_system_cfg_entry "genesis.integerscale" "0"
        Set_system_cfg_entry "genh.ratio" "custom"
        Set_system_cfg_entry "genh.integerscale" "0"
        Set_system_cfg_entry "megadrive.ratio" "custom"
        Set_system_cfg_entry "megadrive.integerscale" "0"
        Set_system_cfg_entry "nes.ratio" "custom"
        Set_system_cfg_entry "nes.integerscale" "0"
        Set_system_cfg_entry "nesh.ratio" "custom"
        Set_system_cfg_entry "nesh.integerscale" "0"
        Set_system_cfg_entry "ngp.ratio" "core"
        Set_system_cfg_entry "ngp.integerscale" "1"
        Set_system_cfg_entry "ngpc.ratio" "core"
        Set_system_cfg_entry "ngpc.integerscale" "1"
        Set_system_cfg_entry "pc.ratio" "custom"
        Set_system_cfg_entry "pc.integerscale" "0"
        Set_system_cfg_entry "psx.ratio" "custom"
        Set_system_cfg_entry "psx.integerscale" "0"
        Set_system_cfg_entry "saturn.ratio" "custom"
        Set_system_cfg_entry "saturn.integerscale" "0"
        Set_system_cfg_entry "segacd.ratio" "custom"
        Set_system_cfg_entry "segacd.integerscale" "0"
        Set_system_cfg_entry "sfc.ratio" "custom"
        Set_system_cfg_entry "sfc.integerscale" "0"
        Set_system_cfg_entry "snes.ratio" "custom"
        Set_system_cfg_entry "snes.integerscale" "0"
        Set_system_cfg_entry "snesh.ratio" "custom"
        Set_system_cfg_entry "snesh.integerscale" "0"
        Set_system_cfg_entry "wonderswan.ratio" "custom"
        Set_system_cfg_entry "wonderswan.integerscale" "0"
        Set_system_cfg_entry "wonderswancolor.ratio" "custom"
        Set_system_cfg_entry "wonderswancolor.integerscale" "0"
        ;;
    "640x480")
        echo "640x480"
        Set_system_cfg_entry "msx.ratio" "custom"
        Set_system_cfg_entry "msx.integerscale" "0"
        Set_system_cfg_entry "pico-8.ratio" "custom"
        Set_system_cfg_entry "pico-8.integerscale" "0"
        Set_system_cfg_entry "pokemini.ratio" "custom"
        Set_system_cfg_entry "pokemini.integerscale" "0"
        Set_system_cfg_entry "psp.ratio" "custom"
        Set_system_cfg_entry "psp.integerscale" "0"
        Set_system_cfg_entry "virtualboy.ratio" "custom"
        Set_system_cfg_entry "virtualboy.integerscale" "0"
        Set_system_cfg_entry "gbc.integerscale" "1"
        Set_system_cfg_entry "gb.integerscale" "1"
        ;;
    "480x320")
        echo "480x320"
        ;;
    *)
        echo "$detected_res"
        ;;
esac

if [ -f "/usr/config/modules/MOD_TOOLS/Bezels Installer.sh" ]; then
    bash "/usr/config/modules/MOD_TOOLS/Bezels Installer.sh" --skip-existing "${detected_res}"
fi



if [ "$HW_DEVICE" = "RK3326" ] || [ "$HW_DEVICE" = "RK3326S" ] || [ "$HW_DEVICE" = "S905" ]; then
    sed -i -e '/gba.shaderset\=/c\gba.shaderset\=handheld/dot.glslp' /storage/.config/system/configs/system.cfg
    sed -i -e '/gbah.shaderset\=/c\gbah.shaderset\=handheld/dot.glslp' /storage/.config/system/configs/system.cfg
fi

# 获取 QUIRK_DEVICE
if [ -z "$QUIRK_DEVICE" ]; then
    QUIRK_DEVICE="$(tr -d '\0' </sys/firmware/devicetree/base/model 2>/dev/null)"
    if [ -z "$QUIRK_DEVICE" ]; then
        QUIRK_DEVICE="$(tr -d '\0' </sys/class/dmi/id/sys_vendor 2>/dev/null) $(tr -d '\0' </sys/class/dmi/id/product_name 2>/dev/null)"
    fi
    QUIRK_DEVICE="$(echo ${QUIRK_DEVICE} | sed -e "s#[/]#-#g")"
fi

case "${QUIRK_DEVICE}" in
    "Anbernic RG ARC-D")
        echo "${QUIRK_DEVICE}"
        Set_system "RGARC-D"
    ;;
    "Anbernic RG ARC-S")
        echo "${QUIRK_DEVICE}"
        Set_system "RGARC-S"
    ;;
    "Anbernic RG353P")
        echo "${QUIRK_DEVICE}"
        Set_system "RG353P"
    ;;
    "Anbernic RG353PS")
        echo "${QUIRK_DEVICE}"
        Set_system "RG353PS"
        Set_system "RG353M"
    ;;
    "Anbernic RG353M"|RG353Mm)
        echo "${QUIRK_DEVICE}"
    ;;
    "Anbernic RG353V")
        echo "${QUIRK_DEVICE}"
        Set_system "RG353V"
    ;;
    "Anbernic RG353VS")
        echo "${QUIRK_DEVICE}"
        Set_system "RG353VS"
    ;;
    "Anbernic RG503")
        echo "${QUIRK_DEVICE}"
        Set_system "RG503"
    ;;
    "Anbernic RG552")
        echo "${QUIRK_DEVICE}"
        Set_system "RG552"
    ;;
    "Anbernic Win600")
        echo "${QUIRK_DEVICE}"
        Set_system "Win600"
    ;;
    "Powkiddy RK2023")
        echo "${QUIRK_DEVICE}"
        Set_system "RK2023"
    ;;
    "Powkiddy RGB20P")
        echo "${QUIRK_DEVICE}"
        Set_system "RGB20PRO"
    ;;
    "Powkiddy RGB30")
        echo "${QUIRK_DEVICE}"
        Set_system "RGB30"
    ;;
    "Powkiddy RGB20SX")
        echo "${QUIRK_DEVICE}"
        Set_system "RGB20SX"
    ;;
    "Powkiddy RGB10 MAX 3 Pro")
        echo "${QUIRK_DEVICE}"
        Set_system "RGB10MAX3PRO"
    ;;
    "Powkiddy RGB10 Max 3")
        echo "${QUIRK_DEVICE}"
        Set_system "X55"
    ;;
    "Powkiddy x35s")
        echo "${QUIRK_DEVICE}"
        Set_system "X35H"
    ;;
    "RGBMAX4")
        echo "${QUIRK_DEVICE}"
        Set_system "RGBMAX4"
    ;;
    "MINILOONG Pocket1")
        echo "${QUIRK_DEVICE}"
        Set_system "MINILOONG"
	set_setting key.hotkey.b=BTN_MODE
    ;;
#----------------------------------------以下已验证-------------------------------------#
# H700设备
    "Anbernic RG34XX")
        echo "${QUIRK_DEVICE}"
        Set_system "RG34XX"
    ;;
    "Anbernic RG CubeXX")
        echo "${QUIRK_DEVICE}"
        Set_system "RGCUBEXX"
    ;;
    "Anbernic RG40XX V")
        echo "${QUIRK_DEVICE}"
        Set_system "RG40XX-V"
    ;;
    "Anbernic RG40XX H")
        echo "${QUIRK_DEVICE}"
        Set_system "RG40XX-H"
    ;;
    "Anbernic RG28XX")
        echo "${QUIRK_DEVICE}"
        Set_system "RG28XX"
    ;;
    "Anbernic RG35XX 2024")
        echo "${QUIRK_DEVICE}"
        Set_system "RG35XX-2024"
    ;;
    "Anbernic RG35XX H")
        echo "${QUIRK_DEVICE}"
        Set_system "RG35XX-H"
    ;;
    "Anbernic RG35XX Plus")
        echo "${QUIRK_DEVICE}"
        Set_system "RG35XX-P"
    ;;
    "Anbernic RG35XX SP")
        echo "${QUIRK_DEVICE}"
        Set_system "RG35XX-SP"
    ;;
    "Anbernic RG34XX-SP")
        echo "${QUIRK_DEVICE}"
        Set_system "RG40XX-H"
    ;;
    "Anbernic RG35XX Pro")
        echo "${QUIRK_DEVICE}"
        Set_system "RG35XX-Pro"
    ;;
# 3326设备
# 稀范科技
    "XiFan MyMini")
        echo "${QUIRK_DEVICE}"
        Set_system "MyMini"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
    "XiFan Mini40")
        echo "${QUIRK_DEVICE}"
        Set_system "Mini40"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
    "XiFan XF35H")
        echo "${QUIRK_DEVICE}"
        Set_system "XF35H"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
    "XiFan R36Max")
        echo "${QUIRK_DEVICE}"
        Set_system "R36Max"
    ;;
    "XiFan R36Pro")
        echo "${QUIRK_DEVICE}"
        Set_system "R36Pro"
    ;;
    "XiFan XF40H")
        echo "${QUIRK_DEVICE}"
        Set_system "XF40H"
    ;;
    "XiFan XF40V")
        echo "${QUIRK_DEVICE}"
        Set_system "XF40V"
    ;;
    "XiFan XF28")
        echo "${QUIRK_DEVICE}"
        Set_system "XF28H"
    ;;
    "XiFan DC35V")
        echo "${QUIRK_DEVICE}"
        Set_system "DC35V"
    ;;
    "XiFan DC40V")
        echo "${QUIRK_DEVICE}"
        Set_system "DC40V"
    ;;
    "XiFan DC45V")
        echo "${QUIRK_DEVICE}"
        Set_system "DC45V"
    ;;
    "XiFan R36Max2")
        echo "${QUIRK_DEVICE}"
        Set_system "R36Max2"
    ;;
    "XiFan RF35H")
        echo "${QUIRK_DEVICE}"
        Set_system "RF35H"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
    "XiFan RF40H")
        echo "${QUIRK_DEVICE}"
        Set_system "RF40H"
    ;;
    "XiFan RF45V")
        echo "${QUIRK_DEVICE}"
        Set_system "RF45V"
    ;;
    "XiFan XF45V")
        echo "${QUIRK_DEVICE}"
        Set_system "XF45V"
    ;;
# 安伯尼克
    "Anbernic RG351M")
        echo "${QUIRK_DEVICE}"
        Set_system "RG351M"
    ;;
    "Anbernic RG351V")
        echo "${QUIRK_DEVICE}"
        Set_system "RG351V"
    ;;
# 亿米创
    "YMC A10Mini")
        echo "${QUIRK_DEVICE}"
        Set_system "A10mini"
    ;;
    "YMC A10Mini V4")
        echo "${QUIRK_DEVICE}"
        Set_system "A10miniv2"
    ;;
# 迪优米
    "Diium D-R28S")
        echo "${QUIRK_DEVICE}"
        Set_system "DR28S"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
    "Diium D007")
        echo "${QUIRK_DEVICE}"
        Set_system "D007"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
# Magicx
    "MagicX XU10")
        echo "${QUIRK_DEVICE}"
        Set_system "XU10"
    ;;
    "MagicX XU Mini M")
        echo "${QUIRK_DEVICE}"
        Set_system "XUMiniM"
    ;;
# 泡机堂
    "Powkiddy RGB10")
        echo "${QUIRK_DEVICE}"
        Set_system "RGB10"
        amixer -c 0 -M cset name="Playback Mux" HP
	set_setting key.dpad.events 1
    ;;
    "Powkiddy RGB20S")
        echo "${QUIRK_DEVICE}"
        amixer -c 0 -M cset name="Playback Mux" HP
        Set_system "RGB20S"
    ;;
    "Powkiddy RGB10X")
        echo "${QUIRK_DEVICE}"
        amixer -c 0 -M cset name="Playback Mux" HP
        Set_system "RGB10X"
    ;;
# 曼特科技
    "GameMT E6")
        echo "${QUIRK_DEVICE}"
        Set_system "E6"
        amixer -c 0 -M cset name="${DEVICE_PLAYBACK_PATH}" $DEVICE_PLAYBACK_PATH_SPK
    ;;
    "GameMT E6Plus")
        echo "${QUIRK_DEVICE}"
        Set_system "E6Plus"
    ;;
    "GameMT E5Plus")
        echo "${QUIRK_DEVICE}"
        Set_system "e5plus"
    ;;
# Odroid
    "ODROID-GO Super")
        echo "${QUIRK_DEVICE}"
        Set_system "OGS"
    ;;
# Game Console
    "Game Console R36S")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
    ;;
    "Game Console R36S Panel1")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
    ;;
    "Game Console R36S Panel2")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
    ;;
    "Game Console R36S Panel3")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
    ;;
    "Game Console R36S Panel4")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
    ;;
    "Game Console R50S")
        echo "${QUIRK_DEVICE}"
        Set_system "R50S"
    ;;
    "Game Console R33S")
        echo "${QUIRK_DEVICE}"
        Set_system "R33S"
    ;;
    "Game Console R36sPlus")
        echo "${QUIRK_DEVICE}"
        Set_system "R36sPlus"
    ;;
    "Game Console R45H")
        echo "${QUIRK_DEVICE}"
        Set_system "R45H"
    ;;
    "Game Console R46H")
        echo "${QUIRK_DEVICE}"
        Set_system "R46H"
    ;;
    "Game Console R40XX")
        echo "${QUIRK_DEVICE}"
        Set_system "R40XX"
    ;;
    "Game Console R40XX ProMax")
        echo "${QUIRK_DEVICE}"
        Set_system "R40xxProMax"
    ;;
# R36s克隆机
    "Clone R36s Type 2 With Amplifier")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
    "Clone R36s Type 2 Without Amplifier")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
    "Clone R36s Type 3 Panel1")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
    "Clone R36s Type 3 Panel2")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
    "Clone R36s Type 4")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
# R36s酱油机
    "Sauce R36s Panel1")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
    "Sauce R36s Panel2")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
    "Sauce R36s Panel3")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
    "Sauce R36s Panel4")
        echo "${QUIRK_DEVICE}"
        Set_system "R36S"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
# K36
    "Game Console K36")
        echo "${QUIRK_DEVICE}"
        Set_system "K36"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
# AISLPC
    "AISLPC K36S")
        echo "${QUIRK_DEVICE}"
        Set_system "K36S"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
    "AISLPC R36T")
        echo "${QUIRK_DEVICE}"
        Set_system "R36T"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
    "AISLPC R36TMax")
        echo "${QUIRK_DEVICE}"
        Set_system "R36TMax"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
# BatleXP
    "BatleXP G350")
        echo "${QUIRK_DEVICE}"
        Set_system "G350"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
# 其他无牌
    "Game Console XGB36")
        echo "${QUIRK_DEVICE}"
        Set_system "XGB36"
        amixer -c 0 -M cset name="Playback Mux" HP
    ;;
    "Game Console R36Ultra")
        echo "${QUIRK_DEVICE}"
        Set_system "R36Ultra"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
    "Game Console T16Max")
        echo "${QUIRK_DEVICE}"
        Set_system "T16Max"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
    "Game Console HG36")
        echo "${QUIRK_DEVICE}"
        Set_system "HG36"
        amixer -c 0 -M cset name="Playback Mux" SPK
    ;;
    "Game Console U8")
        echo "${QUIRK_DEVICE}"
        Set_system "U8"
    ;;
    "Game Console RX6H")
        echo "${QUIRK_DEVICE}"
        Set_system "RX6H"
    ;;
    "Game Console R40S")
        echo "${QUIRK_DEVICE}"
        Set_system "R40S"
    ;;
    "Game Console RG36")
        echo "${QUIRK_DEVICE}"
        Set_system "RG36"
    ;;
    "Game Console RG36PRO")
        echo "${QUIRK_DEVICE}"
        Set_system "RG36PRO"
    ;;
# 兜底
    *)
        echo "${QUIRK_DEVICE}"
        Set_system "Aurknix"
    ;;
esac

cp -f /usr/config/retroarch/retroarch.cfg ${RACFG}
Set_ra_ext "$MENU_SCALE_FACTOR" "$MENU_WIDGET_SCALE_FACTOR"

if [ "$(systemctl is-active input)" = "active" ]
then
  systemctl restart input
fi

# RK356X: sync stock eMMC bootloader and EmuELEC boot assets to the TF card.
if [ "${HW_DEVICE}" = "RK356X" ]; then
  _sync_uboot="/usr/config/modules/MOD_TOOLS/Sync eMMC Bootloader to TF.sh"
  [ -x "${_sync_uboot}" ] || _sync_uboot="/storage/.config/modules/MOD_TOOLS/Sync eMMC Bootloader to TF.sh"
  if [ -x "${_sync_uboot}" ]; then
    echo "Sync eMMC bootloader to TF..." >>/var/log/boot.log
    "${_sync_uboot}" >>/var/log/boot.log 2>&1 || true
  fi
fi

sync

# Zenith: ultimo fotograma y liberar la pantalla (el trap EXIT lo hace tambien si se sale antes).
if [ "$UPDATE_MODE" != "device_change" ] && [ -x "$ZSPLASH" ]; then
    "$ZSPLASH" show zenith-3-done
    sleep 2
    "$ZSPLASH" stop
fi
