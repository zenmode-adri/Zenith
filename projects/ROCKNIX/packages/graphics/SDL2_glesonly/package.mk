# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2024 ROCKNIX (https://github.com/ROCKNIX)

# Inherit nearly everything
OPENGL_SUPPORT=no
. $(get_pkg_directory SDL2)/package.mk

PKG_NAME="SDL2_glesonly"
PKG_DEPENDS_UNPACK+=" SDL2"

makeinstall_target() {
  mkdir -p "${INSTALL}/usr/lib/glesonly"
  cp -a libSDL2-2.0.so.0.* "${INSTALL}/usr/lib/glesonly/"
}

post_makeinstall_target() {
  :
}


# GCC 14 promotes -Wincompatible-pointer-types to an error; SDL2 2.32.10 ALSA dynload
# declares snd_pcm_info_free as int(*) while alsa-lib exports void(*). Downgrade to warning.
eval "$(declare -f pre_configure_target | sed '1s/pre_configure_target ()/pre_configure_target_orig()/')"
pre_configure_target() {
  pre_configure_target_orig
  export CFLAGS="${CFLAGS} -Wno-error=incompatible-pointer-types"
}
