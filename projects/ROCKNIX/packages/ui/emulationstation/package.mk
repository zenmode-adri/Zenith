# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)

PKG_NAME="emulationstation"
PKG_VERSION="cada856d86e3115fbbf0bce09dd761b0ee8fa9fd"  # ROCKNIX/emulationstation-next 2026-09-28 (incluye merge de batocera/master del 16-sep)
PKG_LICENSE="GPL"
PKG_SITE="https://github.com/ROCKNIX/emulationstation-next"
PKG_URL="${PKG_SITE}.git"
PKG_DEPENDS_TARGET="boost toolchain SDL2 freetype curl freeimage bash rapidjson SDL2_mixer fping p7zip alsa vlc drm_tool pugixml"
PKG_NEED_UNPACK="busybox"
PKG_LONGDESC="Emulationstation emulator frontend"
PKG_BUILD_FLAGS="-gold"
GET_HANDLER_SUPPORT="git"
PKG_PATCH_DIRS+="${DEVICE}"

# El ES nuevo (cada856) exige UNA sola API grafica en CMake (GL/GLES/GLES2/GLES3 son excluyentes).
# ROCKNIX upstream compila con GLES3 (renderizador GLES30); lo mismo aqui.
if [ ! "${OPENGLES_SUPPORT}" = no ]; then
  PKG_DEPENDS_TARGET+=" ${OPENGLES}"
fi
PKG_DEPENDS_TARGET+=" poppler"
PKG_CMAKE_OPTS_TARGET+=" -DGLES3=1"

PKG_CMAKE_OPTS_TARGET+=" -DROCKNIX=1 \
                         -DDISABLE_KODI=1 \
                         -DENABLE_FILEMANAGER=0 \
                         -DCEC=0 \
                         -DENABLE_PULSE=1 \
                         -DUSE_SYSTEM_PUGIXML=1"

pre_configure_target() {
  for key in SCREENSCRAPER_DEV_LOGIN \
        GAMESDB_APIKEY \
        CHEEVOS_DEV_LOGIN
  do
    if [ -z "${!key}" ]
    then
      echo "WARNING: ${key} not declared, will not build support."
    else
      echo "USING: ${key} = ${!key}"
    fi
  done

  export DEVICE=$(echo ${DEVICE^^} | sed "s#-#_##g")
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/config/locale
  cp -rf ${PKG_BUILD}/locale/lang/* ${INSTALL}/usr/config/locale/

  # Pre-generate default (en_US.UTF-8) locale for lower-end devices to speed up first boot
  # This saves a minute or two on RK3326 in a cost of about 1 MB of SYSTEM size
  # Copy-paste of a locale generating part of es_settings script
  I18NPATH=$(get_install_dir glibc)/usr/share/i18n/locales/ \
    localedef --force --verbose --inputfile=en_US --charmap=UTF-8 \
    ${INSTALL}/usr/config/locale/en_US.UTF-8 || true

  # Zenith: precompile the locale of EVERY language the UI offers (zfront's 36 codes, same list as language_names() in
  # zfront/src/menu_ui.cpp) into a read-only dir of the image, so the first boot after choosing a language does not
  # run localedef on the console (~22 s on RK3326). es_settings uses it when ${LANG} is there and falls back to the
  # old on-device localedef when it is not. Same host localedef (+ glibc locale sources) as the en_US step above.
  ZENITH_LOCALES="ar_YE ca_ES cs_CZ cy_GB de_DE el_GR en_US en_GB es_ES es_MX eu_ES fi_FI fr_FR gl_ES he_IL hu_HU id_ID
                  it_IT ja_JP ko_KR nb_NO nl_NL nn_NO oc_FR pl_PL pt_BR pt_PT ro_RO ru_RU sk_SK sv_SE tr_TR uk_UA vi_VN
                  zh_CN zh_TW"
  mkdir -p ${INSTALL}/usr/lib/zenith-locale
  export I18NPATH=$(get_install_dir glibc)/usr/share/i18n/locales/
  for l in ${ZENITH_LOCALES}; do
    echo ${l}
  done | xargs -P ${CONCURRENCY_MAKE_LEVEL:-2} -I{} sh -c     "localedef --force --inputfile={} --charmap=UTF-8 ${INSTALL}/usr/lib/zenith-locale/{}.UTF-8 >/dev/null 2>&1 || true"
  # a failed/partial locale must not exist: es_settings would trust it
  for d in ${INSTALL}/usr/lib/zenith-locale/*.UTF-8; do
    [ -s "${d}/LC_NAME" ] || { echo "WARNING: zenith locale ${d##*/} not generated"; rm -rf "${d}"; }
  done
  unset I18NPATH

  mkdir -p ${INSTALL}/usr/config/emulationstation/resources
  cp -rf ${PKG_BUILD}/resources/* ${INSTALL}/usr/config/emulationstation/resources/
  rm -rf ${INSTALL}/usr/config/emulationstation/resources/logo.png

  mkdir -p ${INSTALL}/usr/bin
  cp ${PKG_BUILD}/es_settings ${INSTALL}/usr/bin
  chmod 0755 ${INSTALL}/usr/bin/es_settings

  # Zenith: ES event scripts (screensaver start/stop switch the backlight); linked into /storage by autostart/001-emulationstation
  mkdir -p ${INSTALL}/usr/share/zenith/es-scripts
  cp -rf ${PKG_DIR}/sources/es-scripts/* ${INSTALL}/usr/share/zenith/es-scripts/
  chmod 0755 ${INSTALL}/usr/share/zenith/es-scripts/*/*.sh

  cp ${PKG_BUILD}/start_es.sh ${INSTALL}/usr/bin
  chmod 0755 ${INSTALL}/usr/bin/start_es.sh
  cp ${PKG_BUILD}/es-themeguard.sh ${INSTALL}/usr/bin
  chmod 0644 ${INSTALL}/usr/bin/es-themeguard.sh

  cp ${PKG_BUILD}/serial_number_check ${INSTALL}/usr/bin
  chmod 0755 ${INSTALL}/usr/bin/serial_number_check

  mkdir -p ${INSTALL}/usr/bin
  #ln -sf /storage/.config/emulationstation/resources ${INSTALL}/usr/bin/resources
  cp -rf ${PKG_BUILD}/emulationstation ${INSTALL}/usr/bin

  mkdir -p ${INSTALL}/etc/emulationstation/
  ln -sf /storage/.config/emulationstation/themes ${INSTALL}/etc/emulationstation/

  cp -rf ${PKG_DIR}/config/common/*.cfg ${INSTALL}/usr/config/emulationstation

  # Enable navigation sounds by default
  sed -i 's|</config>|  <bool name="EnableSounds" value="true" />\n</config>|' \
    ${INSTALL}/usr/config/emulationstation/es_settings.cfg

  # Zenith: default theme (the others are linked by userconfig-setup and selectable in the menu)
  sed -i 's|</config>|  <string name="ThemeSet" value="es-theme-zenith-next-dark" />\n</config>|' \
    ${INSTALL}/usr/config/emulationstation/es_settings.cfg

  # Zenith: ES screensaver (black frame + backlight off via es-scripts) after 2 minutes instead of 5
  sed -i 's|</config>|  <int name="ScreenSaverTime" value="120000" />\n</config>|' \
    ${INSTALL}/usr/config/emulationstation/es_settings.cfg

  # Add device-specific settings for S905
  if [ "${DEVICE}" = "S905" ]; then
    sed -i 's|</config>|  <bool name="BackgroundJoystickInput" value="true" />\n</config>|' \
      ${INSTALL}/usr/config/emulationstation/es_settings.cfg
  fi

  # If we're not an emulation device, ES may still be installed so we need a default config.
  if [ "${EMULATION_DEVICE}" = "no" ] || \
     [ "${BASE_ONLY}" = "true" ]
  then
    cat <<EOF >${INSTALL}/usr/config/emulationstation/es_systems.cfg
<?xml version="1.0" encoding="UTF-8"?>
<systemList>
        <system>
                <name>tools</name>
                <fullname>Tools</fullname>
                <manufacturer>ROCKNIX</manufacturer>
                <release>2024</release>
                <hardware>system</hardware>
                <path>/storage/.config/modules</path>
                <extension>.sh</extension>
                <command>%ROM%</command>
                <platform>tools</platform>
                <theme>tools</theme>
        </system>
</systemList>
EOF
  fi

  ln -sf ${INSTALL}/usr/config/emulationstation/es_systems.cfg ${INSTALL}/etc/emulationstation/es_systems.cfg

  ln -sf /storage/.cache/system_timezone ${INSTALL}/etc/timezone

  #Delete all vulkan options from es_features when vulkan is not present
  if [ ! "${VULKAN_SUPPORT}" = "yes" ]
    then
      sed -i '/vulkan/d' ${INSTALL}/usr/config/emulationstation/es_features.cfg
  fi
}


post_install() {
  mkdir -p ${INSTALL}/usr/share
  ln -sf /storage/.config/locale ${INSTALL}/usr/share/locale

  mkdir -p ${INSTALL}/usr/lib
  ln -sf /usr/share/locale ${INSTALL}/usr/lib/locale

  ln -sf /usr/share/locale  ${INSTALL}/usr/config/emulationstation/locale
}
