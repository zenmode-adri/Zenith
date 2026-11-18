# SPDX-License-Identifier: Apache-2.0
# Zenith: cargador de Vulkan (libvulkan.so.1) PRIVADO, en /usr/lib/r36-vk/. No se instala en /usr/lib ni en el sysroot a
# proposito: asi ningun otro programa de la imagen encuentra Vulkan por su cuenta (RetroArch, cores...). Solo lo usa quien lo
# pida con LD_LIBRARY_PATH (start_ppsspp.sh). Mismas opciones que el cargador medido en la Ultra (solo Wayland).
PKG_NAME="r36-vulkan-loader"
PKG_VERSION="1.4.347"
PKG_SHA256="4b6002e4200d59d76bad1058b677fdbfab9abdb2ef3b3856a31026bc445341f0"
PKG_LICENSE="Apache-2.0"
PKG_SITE="https://github.com/KhronosGroup/Vulkan-Loader"
PKG_URL="https://github.com/KhronosGroup/Vulkan-Loader/archive/v${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain Python3:host vulkan-headers:host wayland"  # cabeceras solo para compilar (el target meteria 19 MB de registry en la imagen)
PKG_LONGDESC="Vulkan ICD loader, private copy for the R36 Vulkan drivers"
PKG_TOOLCHAIN="cmake"

PKG_CMAKE_OPTS_TARGET="-DBUILD_TESTS=OFF \
                       -DCMAKE_BUILD_TYPE=Release \
                       -DBUILD_WSI_XCB_SUPPORT=OFF \
                       -DBUILD_WSI_XLIB_SUPPORT=OFF \
                       -DBUILD_WSI_WAYLAND_SUPPORT=ON"

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/lib/r36-vk
  cp -a loader/libvulkan.so.1* ${INSTALL}/usr/lib/r36-vk/
  ln -sf libvulkan.so.1 ${INSTALL}/usr/lib/r36-vk/libvulkan.so
}
