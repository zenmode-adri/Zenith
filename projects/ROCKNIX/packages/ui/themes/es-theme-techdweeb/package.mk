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
    # Zenith: include the volume popup layout (kept clear of the clock / status icons, same row as them) and a translucent
    # dark pill behind the status icons (they were unreadable over light covers)
    sed -i 's#</theme>#  <include>./zenith-volume.xml</include>\n  <include>./zenith-menu-icons.xml</include>\n  <include>./zenith-status.xml</include>\n  <include>./zenith-grid.xml</include>\n</theme>#' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml
    # Zenith: the theme has no grid view. Add "grid" to the shared views (status indicators, help, gamelist background and
    # logo) so the grid designed in zenith-grid.xml gets them too; the default (automatic) view is not changed.
    sed -i 's#<view name="system,basic,detailed,gamecarousel">#<view name="system,basic,detailed,gamecarousel,grid">#;s#<view name="basic,detailed,gamecarousel">#<view name="basic,detailed,gamecarousel,grid">#' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml ${INSTALL}/usr/share/themes/${PKG_NAME}/aspect-ratio-*.xml ${INSTALL}/usr/share/themes/${PKG_NAME}/zenith-status.xml
    # Zenith: game list font 0.0229 -> 0.019 (the theme's own "small" size) so 30-character names are not cut
    sed -i '/<textlist name="gamelist">/,/<\/textlist>/s#<fontSize ifSubset="font-size:default">0.02291667</fontSize>#<fontSize ifSubset="font-size:default">0.019</fontSize>#' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml
    # Zenith: menu icons for the entries this theme does not define (same line icons as the Zenith theme)
    cp -rf ${PKG_DIR}/../es-theme-zenith/theme/assets/menu ${INSTALL}/usr/share/themes/${PKG_NAME}/zenith-menu
    cp -f ${PKG_DIR}/NOTICE.md ${INSTALL}/usr/share/themes/${PKG_NAME}/NOTICE.md
}
