# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)
#
# Zenith EmulationStation themes (Minimal and Next, light and dark), original work by zenmode-adri. See NOTICE.md.

PKG_NAME="es-theme-zenith"
PKG_VERSION=""
PKG_LICENSE="GPL-2.0"
PKG_SITE="https://github.com/zenmode-adri"
PKG_URL=""
PKG_LONGDESC="Zenith EmulationStation themes: Minimal and Next, light and dark"
PKG_TOOLCHAIN="manual"

makeinstall_target() {
  # light theme
  mkdir -p ${INSTALL}/usr/share/themes/es-theme-zenith
  cp -rf ${PKG_DIR}/theme/* ${INSTALL}/usr/share/themes/es-theme-zenith/
  cp -f ${PKG_DIR}/NOTICE.md ${INSTALL}/usr/share/themes/es-theme-zenith/NOTICE.md

  # dark theme = light theme + the few files that differ (colors, tiles, splash)
  mkdir -p ${INSTALL}/usr/share/themes/es-theme-zenith-dark
  cp -rf ${PKG_DIR}/theme/* ${INSTALL}/usr/share/themes/es-theme-zenith-dark/
  cp -rf ${PKG_DIR}/overlay-dark/* ${INSTALL}/usr/share/themes/es-theme-zenith-dark/
  cp -f ${PKG_DIR}/NOTICE.md ${INSTALL}/usr/share/themes/es-theme-zenith-dark/NOTICE.md

  # Zenith Next = the same folders + one variable: zfront draws them with their own layout (ES draws them like the others)
  for variant in "" "-dark"; do
    cp -rf ${INSTALL}/usr/share/themes/es-theme-zenith${variant} ${INSTALL}/usr/share/themes/es-theme-zenith-next${variant}
    sed -i 's#<variables>#<variables>\n    <zenStyle>next</zenStyle>#' ${INSTALL}/usr/share/themes/es-theme-zenith-next${variant}/theme.xml
  done
}
