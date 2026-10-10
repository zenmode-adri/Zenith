# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)

PKG_NAME="es-theme-art-book-next"
PKG_VERSION="d3c82a3ff506efd846dfb2a48a0af45f224aa94d"
PKG_LICENSE="CUSTOM"
PKG_SITE="https://github.com/AveyondFly/art-book-next-es"
PKG_URL="https://github.com/AveyondFly/art-book-next-es/archive/${PKG_VERSION}.tar.gz"
PKG_LONGDESC="Art Book Next"
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/share/themes/${PKG_NAME}
    cp -rf * ${INSTALL}/usr/share/themes/${PKG_NAME}
    rm -rf ${INSTALL}/usr/share/themes/${PKG_NAME}/_inc/systems/{artwork-circuit,artwork-classic,artwork-nintendont,artwork-noir,artwork-outline}
    sed -i '/<include name="\(noir\|nintendont\|circuit\|outline\)"/d' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml
    # Zenith: only the splash screen is replaced (logo, no progress bar); the theme itself is the stock one
    if [ -d ${PKG_DIR}/overlay ]; then
      cp -rf ${PKG_DIR}/overlay/* ${INSTALL}/usr/share/themes/${PKG_NAME}/
    fi
    # Zenith: game list font a little smaller (default size) so long game names are not cut
    sed -i '/<textlist name="gamelist">/,/<\/textlist>/s#<fontSize ifSubset="font-size:default">0.0375</fontSize>#<fontSize ifSubset="font-size:default">0.0315</fontSize>#' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml
    # Zenith: menu icons for the entries this theme does not define (same line icons as the Zenith theme)
    cp -rf ${PKG_DIR}/../es-theme-zenith/theme/assets/menu ${INSTALL}/usr/share/themes/${PKG_NAME}/zenith-menu
    sed -i 's#</theme>#  <include>./zenith-menu-icons.xml</include>\n</theme>#' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml
}
