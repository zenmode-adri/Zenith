# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)
#
# Zenith theme (by zenmode-adri): derived from "Art Book Next" by Anthony Caccese
# (https://github.com/anthonycaccese/art-book-next-es), licensed CC BY-NC-SA.
# Same license applies here: credit to the original author, share-alike, non-commercial.

PKG_NAME="es-theme-zenith"
PKG_VERSION="d3c82a3ff506efd846dfb2a48a0af45f224aa94d"
PKG_LICENSE="CC-BY-NC-SA"
PKG_SITE="https://github.com/AveyondFly/art-book-next-es"
PKG_URL="https://github.com/AveyondFly/art-book-next-es/archive/${PKG_VERSION}.tar.gz"
PKG_LONGDESC="Zenith theme (derived from Art Book Next)"
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/share/themes/${PKG_NAME}
    cp -rf * ${INSTALL}/usr/share/themes/${PKG_NAME}
    rm -rf ${INSTALL}/usr/share/themes/${PKG_NAME}/_inc/systems/{artwork-circuit,artwork-classic,artwork-nintendont,artwork-noir,artwork-outline}
    sed -i '/<include name="\(noir\|nintendont\|circuit\|outline\)"/d' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml
    # Zenith changes: colors, splash screen (logo, no progress bar), fade transition
    cp -rf ${PKG_DIR}/overlay/* ${INSTALL}/usr/share/themes/${PKG_NAME}/
    cp -f ${PKG_DIR}/NOTICE.md ${INSTALL}/usr/share/themes/${PKG_NAME}/NOTICE.md
}
