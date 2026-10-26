# SPDX-License-Identifier: GPL-2.0
# Zenith: zfront, el frontend propio (C++17, SDL2). Se instala AL LADO de EmulationStation: `system.frontend=zfront` en
# system.cfg lo activa (ver sources/docs/install.md). El codigo vive en sources/ (instantanea de zenith-frontend/cpp).

PKG_NAME="zfront"
PKG_VERSION="1.0"
PKG_LICENSE="GPL-2.0"
PKG_SITE="https://github.com/zenmode-adri"
PKG_URL=""
PKG_DEPENDS_TARGET="toolchain SDL2 freetype"
PKG_DEPENDS_HOST="gettext:host"
PKG_LONGDESC="zfront: Zenith frontend (replaces EmulationStation when selected)"
PKG_TOOLCHAIN="cmake"
PKG_BUILD_FLAGS="-gold"

# Sin tests ni tema duplicado (la imagen ya trae /usr/share/themes/es-theme-zenith{,-dark}).
PKG_CMAKE_OPTS_TARGET="-DZF_BUILD_TESTS=OFF -DZF_INSTALL_THEME=OFF"
# El msgfmt de los catalogos propios sale del gettext de la maquina de construccion (host)
pre_configure_target() {
  export PATH="${TOOLCHAIN}/bin:${PATH}"
  # id de desarrollador de ScreenScraper: como en ES, solo si esta declarado en el entorno de la compilacion
  if [ -z "${SCREENSCRAPER_DEV_LOGIN}" ]; then
    echo "WARNING: SCREENSCRAPER_DEV_LOGIN not declared, zfront's scraper will not start."
  fi
}
