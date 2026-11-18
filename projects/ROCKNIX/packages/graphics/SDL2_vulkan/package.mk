# SPDX-License-Identifier: GPL-2.0-or-later
# Zenith: SDL2 con soporte Vulkan (SDL_Vulkan_*), PRIVADO en /usr/lib/r36-vk/. La SDL2 del sistema va sin Vulkan y PPSSPP
# no abre ventana en modo Vulkan con ella. Mismo patron que SDL2_glesonly: hereda todo de SDL2 y solo cambia la instalacion
# (nada al sysroot ni a /usr/lib). El resto de programas siguen con la SDL2 normal.

. $(get_pkg_directory SDL2)/package.mk

PKG_NAME="SDL2_vulkan"
PKG_DEPENDS_UNPACK+=" SDL2"

# espacio final a proposito: el pre_configure_target de SDL2 pega sus -D sin espacio inicial
PKG_CMAKE_OPTS_TARGET+=" -DSDL_VULKAN=ON -DVIDEO_VULKAN=ON "

makeinstall_target() {
  mkdir -p "${INSTALL}/usr/lib/r36-vk"
  cp -a libSDL2-2.0.so.0.* "${INSTALL}/usr/lib/r36-vk/"
  ln -sf $(ls libSDL2-2.0.so.0.* | head -1) "${INSTALL}/usr/lib/r36-vk/libSDL2-2.0.so.0"
}

post_makeinstall_target() {
  :
}

# GCC 14 promotes -Wincompatible-pointer-types to an error (same workaround as SDL2_glesonly)
eval "$(declare -f pre_configure_target | sed '1s/pre_configure_target ()/pre_configure_target_orig()/')"
pre_configure_target() {
  pre_configure_target_orig
  export CFLAGS="${CFLAGS} -Wno-error=incompatible-pointer-types"
}
