# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)
#
# Third-party EmulationStation theme, bundled in Zenith. See NOTICE.md and the theme's own license file.

PKG_NAME="es-theme-techdweeb"
PKG_VERSION="4a27965ed279466c1b1d6f6e98ffc279e2d0f6d4"
PKG_LICENSE="CC-BY-NC-SA-2.0"
PKG_SITE="https://github.com/anthonycaccese/techdweeb-es"
PKG_URL="https://github.com/anthonycaccese/techdweeb-es/archive/${PKG_VERSION}.tar.gz"
PKG_LONGDESC="es-theme-techdweeb EmulationStation theme"
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/share/themes/${PKG_NAME}
    cp -rf * ${INSTALL}/usr/share/themes/${PKG_NAME}
    rm -rf ${INSTALL}/usr/share/themes/${PKG_NAME}/screenshots ${INSTALL}/usr/share/themes/${PKG_NAME}/.git*
    if [ -d ${PKG_DIR}/overlay ]; then
      cp -rf ${PKG_DIR}/overlay/* ${INSTALL}/usr/share/themes/${PKG_NAME}/
    fi
    # Zenith: include the volume popup layout (kept clear of the clock / status icons, same row as them)
    sed -i 's#</theme>#  <include>./zenith-volume.xml</include>\n  <include>./zenith-menu-icons.xml</include>\n</theme>#' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml
    # Zenith: menu icons for the entries this theme does not define (same line icons as the Zenith theme)
    cp -rf ${PKG_DIR}/../es-theme-zenith/theme/assets/menu ${INSTALL}/usr/share/themes/${PKG_NAME}/zenith-menu
    cp -f ${PKG_DIR}/NOTICE.md ${INSTALL}/usr/share/themes/${PKG_NAME}/NOTICE.md
}
