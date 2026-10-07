# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)

PKG_NAME="rk915"
PKG_VERSION="e2fd61651c272aab7bd7e76ba05e5682d7c1d211"
PKG_LICENSE="GPL"
PKG_SITE="https://github.com/AveyondFly/rk915"
PKG_URL="${PKG_SITE}/archive/${PKG_VERSION}.tar.gz"
PKG_LONGDESC="RK915 WiFi/BT SDIO driver"
PKG_TOOLCHAIN="manual"
PKG_IS_KERNEL_PKG="yes"

pre_make_target() {
  unset LDFLAGS
  # Ultra: el chip no despierta del sueno del LMAC de forma fiable; sin sleep por defecto (probado en hardware, 2026-10-07)
  sed -i 's/^unsigned int lpw_no_sleep = 0;/unsigned int lpw_no_sleep = 1;/' ${PKG_BUILD}/src/procfs.c
  # Ultra: descargar el firmware en el probe hace que el chip complete el reset (probado en hardware 2026-10-07; con 0 se quedaba mudo)
  sed -i 's/^unsigned int down_fw_in_probe = 0;/unsigned int down_fw_in_probe = 1;/' ${PKG_BUILD}/src/umac_if.c
}

make_target() {
  kernel_make -C $(kernel_path) M=${PKG_BUILD} \
    BUSIF=SDIO \
    ROM=true \
    CHIP=RK915
}

makeinstall_target() {
  mkdir -p ${INSTALL}/$(get_full_module_dir)/${PKG_NAME}
  cp ${PKG_BUILD}/*.ko ${INSTALL}/$(get_full_module_dir)/${PKG_NAME}

  # Install firmware to kernel overlay
  mkdir -p ${INSTALL}/$(get_full_firmware_dir)
  cp ${PKG_BUILD}/firmware/*.bin ${INSTALL}/$(get_full_firmware_dir)/
}
