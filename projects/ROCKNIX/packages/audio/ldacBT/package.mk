# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)

. ${ROOT}/packages/audio/ldacBT/package.mk

PKG_CMAKE_OPTS_TARGET="-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
                       -DLDAC_SOFT_FLOAT=OFF"
