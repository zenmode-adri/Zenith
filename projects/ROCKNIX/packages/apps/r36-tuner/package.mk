# SPDX-License-Identifier: MIT
# R36 Tuner Next (zenmode-adri) adapted for Zenith: https://github.com/zenmode-adri/r36-tuner-next

PKG_NAME="r36-tuner"
PKG_VERSION="1.9-zenith1"
PKG_LICENSE="MIT"
PKG_SITE="https://github.com/zenmode-adri/r36-tuner-next"
PKG_URL=""
PKG_DEPENDS_TARGET="toolchain SDL2 SDL2_ttf gamecontrollerdb"
PKG_LONGDESC="R36 Tuner Next: SDL2 tuner for RK3326 handhelds (governor, frequencies, turbo, benchmarks, monitor)"
PKG_TOOLCHAIN="make"

pre_make_target() {
  cp -f ${PKG_DIR}/Makefile ${PKG_BUILD}
  cp -f ${PKG_DIR}/sources/*.c ${PKG_BUILD}
  CFLAGS+=" -I$(get_build_dir SDL2)/include -D_REENTRANT"
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin
  cp -f ${PKG_BUILD}/r36-tuner ${PKG_BUILD}/r36_cpubench ${PKG_BUILD}/r36_rambench ${INSTALL}/usr/bin
  chmod 0755 ${INSTALL}/usr/bin/r36-tuner ${INSTALL}/usr/bin/r36_cpubench ${INSTALL}/usr/bin/r36_rambench

  mkdir -p ${INSTALL}/usr/config/modules
  cp -rf ${PKG_DIR}/scripts/* ${INSTALL}/usr/config/modules
  chmod 0755 ${INSTALL}/usr/config/modules/*
}
