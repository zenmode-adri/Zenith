# SPDX-License-Identifier: GPL-2.0-or-later
# Zenith: Vulkan de Arm (libmali g29p1-12eac0, Vulkan 1.3.303) para la Mali-G31 de RK3326, como EXTRA.
# No sustituye la libmali g24p0 del sistema (GL): todo vive en /usr/lib/mali-vk/ y solo lo carga el cargador de Vulkan, por la ruta absoluta
# de mali.json (ver start_ppsspp.sh: VK_DRIVER_FILES, VK_IMPLICIT_LAYER_PATH, R36_NOBGRA=1; ya sin LD_LIBRARY_PATH=/usr/lib/mali-vk).
# Ojo: /usr/lib/mali-vk NO esta dentro de /usr/lib/mali, asi que gpudriver (bind_libs) no lo monta sobre /usr/lib.
#
# Procedencia (medida en la Ultra, datos/R21 y R22):
#  - blob: JeffyCN/mirrors, rama libmali-next, commit bf621d15 (lib/aarch64-linux-gnu/libmali-bifrost-g31-g29p1.so).
#  - envoltorio de Vulkan (libMaliVulkan.so.1): se GENERA al construir (make_target) desde un fuente vacio con su soname y
#    patchelf --add-needed del hook (como hace el meson de libmali-next con -Dwrappers=enabled). No hay binarios suyos en git.
#  - hook 1.10.0 (libmali-hook-vk.so.1): se COMPILA al construir desde sources/hook/hook.c (Rockchip, GPL-2.0-or-later; copia de
#    JeffyCN/mirrors bf621d15 hook/hook.c) con los mismos -D que activa el meson para este blob, y patchelf --add-needed del blob.
#    Con el hook 1.9.0 de la libmali del sistema PPSSPP/SDL falla con "Could not get EGL display"; hace falta el 1.10.
#  - sonames propios (libmali-vk.so.1, libmali-hook-vk.so.1) y RUNPATH $ORIGIN: conviven en un mismo proceso con el libmali g24p0 del sistema.
#  - sources/prebuilt/libVkLayer_window_system_integration.so: capa WSI de Wayland de Arm (data/vulkan/lib/aarch64 de libmali-next,
#    md5 37df3459...), binario cerrado bajo el EULA de Arm; se deja en git (sin otra copia descargable con hash) junto con el EULA.
#  - licencia del blob y de la capa WSI: EULA de Arm (licenses/Arm-Mali-EULA.txt), que tambien se instala en /usr/lib/mali-vk/.
#  - capa VK_LAYER_R36_nobgra (sources/nobgra.c): sin ella vkCreateSwapchainKHR falla (-3) con B8G8R8A8 en sway. Tambien espera a la GPU (vkQueueWaitIdle) antes de cada present: sin ello el Vulkan de Arm enseña fotogramas a medio dibujar.
PKG_NAME="libmali-vk"
PKG_VERSION="g29p1-12eac0"
PKG_LICENSE="Arm-Mali-EULA"
PKG_ARCH="aarch64"
PKG_SOURCE_NAME="libmali-bifrost-g31-g29p1-bf621d15.so"
PKG_URL="https://raw.githubusercontent.com/JeffyCN/mirrors/bf621d15f009509557d6b28f81427651695cac20/lib/aarch64-linux-gnu/libmali-bifrost-g31-g29p1.so"
PKG_SHA256="9a931238bf5dd65d58e30bc92d6ab4e8417f858b5778dc2fb7fb6430dc7d3b83"
PKG_DEPENDS_TARGET="toolchain wayland libdrm patchelf:host vulkan-headers:host"  # cabeceras solo para compilar la capa (vulkan-headers:target meteria 19 MB de registry en la imagen)
PKG_LONGDESC="Arm Mali Vulkan 1.3 userspace driver (g29p1) for RK3326, kept apart from the system libmali"
PKG_TOOLCHAIN="manual"
# patchelf-ed wrappers must not be stripped
PKG_BUILD_FLAGS="-strip"

unpack() {
  mkdir -p ${PKG_BUILD}
  cp ${SOURCES}/${PKG_NAME}/${PKG_SOURCE_NAME} ${PKG_BUILD}/libmali-bifrost-g31-g29p1.so
}

make_target() {
  # Nombres PROPIOS (libmali-vk.so.1 / libmali-hook-vk.so.1): PPSSPP lleva cargados al arrancar el libmali.so.1 y el libmali-hook.so.1 del
  # sistema (g24p0, por su NEEDED de libEGL/libGLESv2) y ld.so no carga dos bibliotecas con el mismo soname; con los nombres de siempre el
  # envoltorio de Vulkan reutilizaria el libmali de GL (o al reves). Con nombres distintos y RUNPATH $ORIGIN conviven sin LD_LIBRARY_PATH:
  # el motor de PPSSPP puede ser OpenGL (g24p0) o Vulkan (g29p1) segun el menu, con el mismo entorno (medido en la Ultra, datos/R35).
  local CFL_HOOK
  # blob con otro soname
  cp libmali-bifrost-g31-g29p1.so libmali-vk.so.1
  patchelf --set-soname libmali-vk.so.1 libmali-vk.so.1
  # hook de Rockchip (GPL-2.0-or-later). Mismos -D que el meson de libmali-next para g29p1: este blob exporta las 5 funciones gbm *_modifiers*.
  # LIBMALI_SO = el soname nuevo (el hook hace dlopen(LIBMALI_SO, RTLD_NOLOAD)); -Bsymbolic: sus llamadas internas (eglGetProcAddress...) no
  # pueden acabar en el hook de g24p0 ya cargado.
  CFL_HOOK="-DLIBMALI_SO=\"libmali-vk.so.1\" -DHAS_GBM -DHAS_EGL -DHAS_gbm_bo_get_modifier -DHAS_gbm_bo_create_with_modifiers     -DHAS_gbm_bo_create_with_modifiers2 -DHAS_gbm_surface_create_with_modifiers -DHAS_gbm_surface_create_with_modifiers2"
  ${CC} -shared -fPIC -O2 -Wl,-soname,libmali-hook-vk.so.1 -Wl,-Bsymbolic ${CFL_HOOK} -Ihook/include/GBM -Ihook/include -I${SYSROOT_PREFIX}/usr/include     hook/hook.c -o libmali-hook-vk.so.1
  patchelf --add-needed libmali-vk.so.1 --set-rpath '$ORIGIN' libmali-hook-vk.so.1

  # envoltorio vacio de Vulkan: soname libMaliVulkan.so.1 (no existe otro igual en el sistema) + NEEDED del hook propio
  : > dummy.c
  ${CC} -shared -fPIC -Wl,--as-needed -Wl,-soname,libMaliVulkan.so.1 dummy.c -o libMaliVulkan.so.1
  patchelf --add-needed libmali-hook-vk.so.1 --set-rpath '$ORIGIN' libMaliVulkan.so.1

  ${CC} -shared -fPIC -O2 -Wall -I${TOOLCHAIN}/include nobgra.c -o libVkLayer_r36_nobgra.so
  ${STRIP} --strip-unneeded libVkLayer_r36_nobgra.so
}

makeinstall_target() {
  local V=${INSTALL}/usr/lib/mali-vk
  mkdir -p ${V}
  # Ya NO se instalan los envoltorios EGL/GLES/gbm/OpenCL ni libwayland-egl de g29p1 (solo servian con LD_LIBRARY_PATH=/usr/lib/mali-vk, que
  # haria que OpenGL usara g29p1): el unico consumidor es el cargador de Vulkan, por la ruta absoluta de mali.json.
  cp -f libmali-vk.so.1 libmali-hook-vk.so.1 libMaliVulkan.so.1 ${V}/
  cp -f prebuilt/libVkLayer_window_system_integration.so ${V}/
  cp -f ${ROOT}/licenses/Arm-Mali-EULA.txt ${V}/END_USER_LICENCE_AGREEMENT.txt
  cp -f libVkLayer_r36_nobgra.so ${V}/
  cp -f mali.json VkLayer_window_system_integration.json VkLayer_r36_nobgra.json ${V}/
}
