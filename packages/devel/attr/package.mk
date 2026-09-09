# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2010-2011 Roman Weber (roman@openelec.tv)
# Copyright (C) 2009-2016 Stephan Raue (stephan@openelec.tv)
# Copyright (C) 2020-present Team LibreELEC (https://libreelec.tv)

PKG_NAME="attr"
PKG_VERSION="2.5.2"
PKG_SHA256="f2e97b0ab7ce293681ab701915766190d607a1dba7fae8a718138150b700a70b"
PKG_LICENSE="GPL"
PKG_SITE="https://savannah.nongnu.org/projects/attr"
PKG_URL="https://deb.debian.org/debian/pool/main/a/attr/attr_2.5.2.orig.tar.xz"
PKG_DEPENDS_TARGET="autotools:host gcc:host"
PKG_LONGDESC="Extended Attributes Of Filesystem Objects."
PKG_BUILD_FLAGS="+pic"

PKG_CONFIGURE_OPTS_TARGET="OPTIMIZER= \
                           CONFIG_SHELL=/bin/bash \
                           INSTALL_USER=root INSTALL_GROUP=root \
                           --disable-shared --enable-static"

if build_with_debug; then
  PKG_CONFIGURE_OPTS_TARGET+=" DEBUG=-DDEBUG"
else
  PKG_CONFIGURE_OPTS_TARGET+=" DEBUG=-DNDEBUG"
fi

post_makeinstall_target() {
  safe_remove ${INSTALL}/etc
  safe_remove ${INSTALL}/usr/bin
}
