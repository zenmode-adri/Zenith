# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2023 JELOS (https://github.com/JustEnoughLinuxOS)

PKG_NAME="rocknix"
PKG_VERSION=""
PKG_LICENSE="GPLv2"
PKG_SITE=""
PKG_URL=""
PKG_DEPENDS_TARGET="toolchain autostart"
PKG_LONGDESC="ROCKNIX Meta Package"
PKG_TOOLCHAIN="make"

make_target() {
  :
}

makeinstall_target() {

  mkdir -p ${INSTALL}/usr/config/
  rsync -av ${PKG_DIR}/config/* ${INSTALL}/usr/config/
  ln -sf /storage/.config/system ${INSTALL}/system
  find ${INSTALL}/usr/config/system/ -type f -exec chmod o+x {} \;

  mkdir -p ${INSTALL}/usr/bin/

  ### Semilla de entropia: acredita una semilla guardada al crng del kernel (adelanta 'crng init done')
  ${CC} ${CFLAGS} ${LDFLAGS} -O2 -o ${INSTALL}/usr/bin/crng-seed ${PKG_DIR}/sources/crng-seed.c
  chmod 0755 ${INSTALL}/usr/bin/crng-seed

  ### Compatibility links for ports
  ln -s /storage/roms ${INSTALL}/roms

  ### Add some quality of life customizations for hardworking devs.
  if [ -n "${LOCAL_SSH_KEYS_FILE}" ]
  then
    mkdir -p ${INSTALL}/usr/config/ssh
    cp ${LOCAL_SSH_KEYS_FILE} ${INSTALL}/usr/config/ssh/authorized_keys
  fi

  if [ -n "${LOCAL_WIFI_SSID}" ]
  then
    sed -i "s#wifi.enabled=0#wifi.enabled=1#g" ${INSTALL}/usr/config/system/configs/system.cfg
    mkdir -p ${INSTALL}/usr/config/iwd
    cat <<EOF >> ${INSTALL}/usr/config/iwd/${LOCAL_WIFI_SSID}.psk
[Security]
Passphrase=${LOCAL_WIFI_KEY}
EOF
  fi
  ### Zenith debug build: copy ready-made iwd network files from LOCAL_IWD_DIR (kept OUTSIDE the repository), exactly as
  ### iwd stores them (an SSID with dots or spaces is stored under an "=<hex>.psk" name, which LOCAL_WIFI_SSID cannot
  ### produce), and keep Wi-Fi and SSH on. The marker makes firstboot/update.sh leave ssh.enabled alone.
  if [ -n "${LOCAL_IWD_DIR}" ] && [ -d "${LOCAL_IWD_DIR}" ]
  then
    mkdir -p ${INSTALL}/usr/config/iwd
    cp -a ${LOCAL_IWD_DIR}/. ${INSTALL}/usr/config/iwd/
    sed -i "s#wifi.enabled=0#wifi.enabled=1#g;s#ssh.enabled=0#ssh.enabled=1#g" ${INSTALL}/usr/config/system/configs/system.cfg
    touch ${INSTALL}/usr/config/zenith-debug-build
  fi
  # Always install the update script
  mkdir -p $INSTALL/usr/share/bootloader
  find_file_path bootloader/detect_subdevice.sh && cp -av ${FOUND_PATH} ${INSTALL}/usr/share/bootloader
  find_file_path bootloader/update.sh && cp -av ${FOUND_PATH} ${INSTALL}/usr/share/bootloader
}

post_install() {
  ln -sf rocknix.target ${INSTALL}/usr/lib/systemd/system/default.target

  if [ ! -d "${INSTALL}/usr/share" ]
  then
    mkdir "${INSTALL}/usr/share"
  fi
  cp ${PKG_DIR}/sources/post-update ${INSTALL}/usr/share
  chmod 755 ${INSTALL}/usr/share/post-update

  # Issue banner
  cat <<EOF >> ${INSTALL}/etc/issue
... Zenith ${OS_VERSION} (based on AURKNIX)
... Built: ${BUILD_DATE}

EOF
  cp ${PKG_DIR}/sources/motd ${INSTALL}/etc
  cat ${INSTALL}/etc/issue >> ${INSTALL}/etc/motd

  cp ${PKG_DIR}/sources/scripts/* ${INSTALL}/usr/bin
  chmod 0755 ${INSTALL}/usr/bin/* 2>/dev/null ||:

  ### Fix and migrate to autostart package
  enable_service rocknix-autostart.service
  
  ### ZRAM/Swap and Memory Manager Service
  enable_service rocknix-memory-manager.service

  ### Take a backup of the system configuration on shutdown
  enable_service save-sysconfig.service

  ### Semilla de entropia (carga en sysinit, guarda una nueva en multi-user)
  enable_service crng-seed.service
  enable_service crng-seed-save.service

  ### Calienta la cache de wifictl has_ap_mode (Network Settings abre rapido)
  enable_service wifi-apcache.service

  ### Guarda el log de arranque en la FAT (/flash/bootlog) para depurar desde Windows
  enable_service bootlog.service

  ### Zenith: pantalla del primer arranque durante la configuracion inicial (solo si /storage/.configured no existe)
  enable_service zenith-splash.service

  ### Apaga de forma limpia si la bateria baja de ~3,2 V sin cargar (proteccion de la bateria)
  enable_service battery-guard.service

  # carga temprana del driver SDIO (WiFi) antes de iwd/connman
  enable_service sdio-modprobe.service

  sed -i "s#@DEVICENAME@#${DEVICE}#g" ${INSTALL}/usr/config/system/configs/system.cfg

  ### Defaults for non-main builds.
  BUILD_BRANCH="$(git branch --show-current)"
  if [ ! "${BUILD_BRANCH}" = "main" ]
  then
    sed -i "s#samba.enabled=0#samba.enabled=1#g" ${INSTALL}/usr/config/system/configs/system.cfg
    sed -i "s#ssh.enabled=0#ssh.enabled=1#g" ${INSTALL}/usr/config/system/configs/system.cfg
    sed -i "s#wifi.enabled=0#wifi.enabled=1#g" ${INSTALL}/usr/config/system/configs/system.cfg
    sed -i "s#system.loglevel=none#system.loglevel=verbose#g" ${INSTALL}/usr/config/system/configs/system.cfg
  fi

  ### Enable HDMI hotplug service on H700
  if [ "${DEVICE}" = "H700" ]
  then
    enable_service hdmi-hotplug.path
  fi

}
