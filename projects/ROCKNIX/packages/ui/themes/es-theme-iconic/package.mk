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
    # the full date (10 characters) overflowed its box into the next one at 640x480: the year is enough for retro games
    sed -i '/name="md_releasedate"/,/<\/datetime>/s#<format>%Y-%m-%d</format>#<format>%Y</format>#' ${UI}/game-info-details.xml
    # metadata text boxes were as tall as the font size and cut the descenders (g, p, y); ES clips auto-scrolling text to its box
    sed -i 's#\(<size>[0-9.]* \)0\.024</size>#\10.038</size>#' ${UI}/game-info-details.xml
    # md_name had the same box (0.024 high): taller box (0.038 = the pill in game-details-panel-*.png) with the same vertical centre (0.488 + 0.012 = 0.481 + 0.019 = 0.500)
    sed -i '/name="md_name"/,/<\/text>/s#<pos>0.05 0.488</pos>#<pos>0.05 0.481</pos>#' ${UI}/game-info-details.xml
    # long game names were cut without an ellipsis
    sed -i 's#<fontSize>0.030333333333333</fontSize>#<fontSize>0.026</fontSize>#' ${UI}/gamelist-textlist.xml
    # wider list column so that long titles (30+ characters) are not cut: same 0.0115 gap to the panel body (x 0.5469-0.9635 in gamelist-panel-*.png) on both sides
    sed -i 's#<pos>0.575 0.09</pos>#<pos>0.5584 0.09</pos>#;s#<size>0.362 0.7519</size>#<size>0.3936 0.7519</size>#' ${UI}/gamelist-textlist.xml
    # ES scales theme fonts x1.31 when the short side is 320-719 px and truncates to whole pixels: 4:3 (640x480) and 1:1 (720x720) still lost the last letters of
    # "Kirby: Nightmare in Dream Land" (30 chars) with a 1-2 px margin, so one pixel less there (15 -> 14 px at 640x480, 18 -> 17 px at 720x720)
    sed -i 's#<fontSize>0.026</fontSize>#<fontSize>0.026</fontSize>\n      <fontSize ifSubset="aspect-ratio:4-3|4-3-auto">0.0245</fontSize>\n      <fontSize ifSubset="aspect-ratio:1-1|1-1-auto">0.0249</fontSize>#' ${UI}/gamelist-textlist.xml
    # Zenith: art for systems this theme does not name but that are the same machine as one it does (same folder, no metadata so name/year stay ES's own)
    TH=${INSTALL}/usr/share/themes/${PKG_NAME}/_inc
    for pair in gbav:gba ggh:gamegear megadriveh:megadrive ndsiware:nds pspminis:psp hbmame:mame vic20:c20 moto:thomson; do
      a=${pair%%:*}; b=${pair##*:}
      for d in logos backgrounds controllers overlays; do
        if [ -f ${TH}/${d}/${b}.webp ] && [ ! -e ${TH}/${d}/${a}.webp ]; then ln -s ${b}.webp ${TH}/${d}/${a}.webp; fi
      done
      if [ -f ${TH}/logos-svg/${b}.svg ] && [ ! -e ${TH}/logos-svg/${a}.svg ]; then ln -s ${b}.svg ${TH}/logos-svg/${a}.svg; fi
    done
    cp -f ${PKG_DIR}/NOTICE.md ${INSTALL}/usr/share/themes/${PKG_NAME}/NOTICE.md
}
