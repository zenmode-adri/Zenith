# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)
#
# Third-party EmulationStation theme, bundled in Zenith. See NOTICE.md and the theme's own license file.

PKG_NAME="es-theme-xmb-fcamod"
PKG_VERSION="a88ffa3d15b23a8b820f8cd172292b788e3079c0"
PKG_LICENSE="CC-BY-NC-SA-4.0"
PKG_SITE="https://github.com/jovemlcxx/es-theme-xmb-fcamod"
PKG_URL="https://github.com/jovemlcxx/es-theme-xmb-fcamod/archive/${PKG_VERSION}.tar.gz"
PKG_LONGDESC="es-theme-xmb-fcamod EmulationStation theme"
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/share/themes/${PKG_NAME}
    cp -rf * ${INSTALL}/usr/share/themes/${PKG_NAME}
    rm -rf ${INSTALL}/usr/share/themes/${PKG_NAME}/screenshots ${INSTALL}/usr/share/themes/${PKG_NAME}/.git*
    if [ -d ${PKG_DIR}/overlay ]; then
      cp -rf ${PKG_DIR}/overlay/* ${INSTALL}/usr/share/themes/${PKG_NAME}/
    fi
    # Zenith: include the volume popup layout (kept clear of the clock / status icons, same row as them)
    sed -i 's#</theme>#  <include>./zenith-volume.xml</include>\n</theme>#' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml
    cp -f ${PKG_DIR}/NOTICE.md ${INSTALL}/usr/share/themes/${PKG_NAME}/NOTICE.md
}
