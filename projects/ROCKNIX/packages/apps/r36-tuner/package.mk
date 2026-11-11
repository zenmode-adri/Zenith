# SPDX-License-Identifier: MIT
# R36 Tuner Next (zenmode-adri) adapted for Zenith: https://github.com/zenmode-adri/r36-tuner-next

PKG_NAME="r36-tuner"
PKG_VERSION="2.0-zenith1"
PKG_LICENSE="MIT"
PKG_SITE="https://github.com/zenmode-adri/r36-tuner-next"
PKG_URL=""
PKG_DEPENDS_TARGET="toolchain SDL2 SDL2_ttf gamecontrollerdb dtc"
PKG_LONGDESC="R36 Tuner Next: SDL2 tuner for RK3326 handhelds (governor, frequencies, turbo, benchmarks, monitor, DTB undervolt/OC)"
PKG_TOOLCHAIN="make"

pre_make_target() {
  cp -f ${PKG_DIR}/Makefile ${PKG_BUILD}
  cp -f ${PKG_DIR}/sources/*.c ${PKG_DIR}/sources/*.h ${PKG_BUILD}
  CFLAGS+=" -I$(get_build_dir SDL2)/include -D_REENTRANT"
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp -f ${PKG_BUILD}/r36-tuner ${PKG_BUILD}/r36_cpubench ${PKG_BUILD}/r36_rambench ${PKG_BUILD}/r36_stress ${INSTALL}/usr/bin
  chmod 0755 ${INSTALL}/usr/bin/r36-tuner ${INSTALL}/usr/bin/r36_cpubench ${INSTALL}/usr/bin/r36_rambench ${INSTALL}/usr/bin/r36_stress

  # phase 2: working copy of the DTB (/flash/tuned.dtb) + boot safety net (autostart calls boot-start / boot-ok)
  install -m 0755 ${PKG_DIR}/sources/r36-dtb ${INSTALL}/usr/bin/r36-dtb

  # clock limits chosen in the tuner, put back at boot
  install -m 0755 ${PKG_DIR}/sources/r36-tuner-limits ${INSTALL}/usr/bin/r36-tuner-limits
  mkdir -p ${INSTALL}/usr/lib/autostart/common
  install -m 0755 ${PKG_DIR}/sources/autostart-096-r36-tuner-limits ${INSTALL}/usr/lib/autostart/common/096-r36-tuner-limits

  mkdir -p ${INSTALL}/usr/config/modules
  cp -rf ${PKG_DIR}/scripts/* ${INSTALL}/usr/config/modules
  chmod 0755 ${INSTALL}/usr/config/modules/*
}

post_install() {
  # phase 2: confirms or undoes a tuned DTB very early in the boot (see sources/r36-dtb)
  enable_service r36-dtb-boot.service
}
