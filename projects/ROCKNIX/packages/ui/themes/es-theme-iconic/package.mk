# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)
#
# Third-party EmulationStation theme, bundled in Zenith. See NOTICE.md and the theme's own license file.

PKG_NAME="es-theme-iconic"
PKG_VERSION="120ed5c4910ff8e77cc634ded8505db97dbb3661"
PKG_LICENSE="CC0-1.0"
PKG_SITE="https://github.com/Delgan/iconic-es"
PKG_URL="https://github.com/Delgan/iconic-es/archive/${PKG_VERSION}.tar.gz"
PKG_LONGDESC="es-theme-iconic EmulationStation theme"
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/share/themes/${PKG_NAME}
    cp -rf * ${INSTALL}/usr/share/themes/${PKG_NAME}
    rm -rf ${INSTALL}/usr/share/themes/${PKG_NAME}/screenshots ${INSTALL}/usr/share/themes/${PKG_NAME}/.git*
    if [ -d ${PKG_DIR}/overlay ]; then
      cp -rf ${PKG_DIR}/overlay/* ${INSTALL}/usr/share/themes/${PKG_NAME}/
    fi
    # Zenith: menu icons for the entries this theme does not define (same line icons as the Zenith theme)
    cp -rf ${PKG_DIR}/../es-theme-zenith/theme/assets/menu ${INSTALL}/usr/share/themes/${PKG_NAME}/zenith-menu
    sed -i 's#</theme>#  <include>./zenith-menu-icons.xml</include>\n</theme>#' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml
    # Zenith: fade transition instead of instant
    sed -i 's/<theme defaultTransition="instant">/<theme defaultTransition="fade">/' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml
    # Zenith: darker secondary text in the light colour scheme (contrast)
    sed -i '/ifSubset="color-scheme:light"/,/<\/variables>/{s#<systemInfoSecondaryColor>a0a0a0#<systemInfoSecondaryColor>707070#;s#<menuFooterColor>a0a0a0#<menuFooterColor>6e6e6e#}' ${INSTALL}/usr/share/themes/${PKG_NAME}/_inc/ui-components/theme-variables.xml
    cp -f ${PKG_DIR}/NOTICE.md ${INSTALL}/usr/share/themes/${PKG_NAME}/NOTICE.md
}
