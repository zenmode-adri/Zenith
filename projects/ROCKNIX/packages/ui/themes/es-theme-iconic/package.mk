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
    # Zenith: contrast, help bar and long-text fixes found reviewing screenshots at 1:1, 4:3, 3:2 and 16:9
    UI=${INSTALL}/usr/share/themes/${PKG_NAME}/_inc/ui-components
    sed -i '/ifSubset="color-scheme:light"/,/<\/variables>/{s#<systemInfoSecondaryColor>a0a0a0#<systemInfoSecondaryColor>707070#;s#<menuFooterColor>a0a0a0#<menuFooterColor>6e6e6e#;s#<gamelistPrimaryColor>777777#<gamelistPrimaryColor>555555#;s#<gamelistSecondaryColor>777777#<gamelistSecondaryColor>555555#}' ${UI}/theme-variables.xml
    # smaller help text in the system view of wide screens so it does not run into the clock
    sed -i 's#<pos>0.48 0.967</pos>#<pos>0.48 0.967</pos><fontSize>0.019</fontSize>#' ${UI}/help.xml
    # the release date was cut with an ellipsis
    sed -i '/name="md_releasedate"/,/<\/datetime>/s#<size>0.08 0.024</size>#<size>0.12 0.024</size>#' ${UI}/game-info-details.xml
    # long game names were cut without an ellipsis
    sed -i 's#<fontSize>0.030333333333333</fontSize>#<fontSize>0.026</fontSize>#' ${UI}/gamelist-textlist.xml
    cp -f ${PKG_DIR}/NOTICE.md ${INSTALL}/usr/share/themes/${PKG_NAME}/NOTICE.md
}
