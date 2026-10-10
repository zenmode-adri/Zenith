"""Prepara el paquete es-theme-zenith (claro + oscuro) en C:/tmp/zt/pkg/es-theme-zenith/"""
import hashlib
import os
import shutil

BASE = 'C:/Users/epsil/Documents/r36-kernel-mainline/SESION-2026-10-04-OS-DEFINITIVO/temas-zenith/'
LIGHT = BASE + 'zenith-switch-claro/'
DARK = BASE + 'zenith-oscuro-WIP/'
OUT = 'C:/tmp/zt/pkg/es-theme-zenith/'
shutil.rmtree('C:/tmp/zt/pkg', ignore_errors=True)
os.makedirs(OUT + 'theme')
os.makedirs(OUT + 'overlay-dark')

UNUSED = ('assets/pill.png', 'assets/pill-wide.png')


def skip(rel):
    if rel in UNUSED:
        return True
    n = os.path.basename(rel)
    return rel.startswith('assets/icons/') and (n.startswith('menu-icon-') or n.startswith('menu-textinput') or n == 'gamepad.svg')


def files(root):
    out = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f).replace('\\', '/')
            rel = p[len(root):]
            if not skip(rel):
                out[rel] = p
    return out


def md5(p):
    return hashlib.md5(open(p, 'rb').read()).hexdigest()


L = files(LIGHT)
D = files(DARK)
for rel, p in L.items():
    os.makedirs(os.path.dirname(OUT + 'theme/' + rel), exist_ok=True)
    shutil.copy(p, OUT + 'theme/' + rel)
changed = []
for rel, p in D.items():
    if rel not in L or md5(p) != md5(L[rel]):
        os.makedirs(os.path.dirname(OUT + 'overlay-dark/' + rel), exist_ok=True)
        shutil.copy(p, OUT + 'overlay-dark/' + rel)
        changed.append(rel)
print('claro:', len(L), 'ficheros;', 'oscuro difiere en', len(changed))
print('\n'.join(sorted(changed)))
tot = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(OUT) for f in fs)
print('tamano paquete', round(tot / 1e6, 1), 'MB')

open(OUT + 'package.mk', 'w', encoding='utf-8', newline='\n').write('''# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2024-present ROCKNIX (https://github.com/ROCKNIX)
#
# Zenith EmulationStation themes (light and dark), original work by zenmode-adri. See NOTICE.md.

PKG_NAME="es-theme-zenith"
PKG_VERSION=""
PKG_LICENSE="GPL-2.0"
PKG_SITE="https://github.com/zenmode-adri"
PKG_URL=""
PKG_LONGDESC="Zenith EmulationStation themes (light and dark)"
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
}
''')

open(OUT + 'NOTICE.md', 'w', encoding='utf-8', newline='\n').write('''# Zenith theme - notice

Zenith (light and dark) is an original EmulationStation theme by **zenmode-adri**, released under **GPL-2.0**
like the rest of this tree. It is not derived from any other theme.

## Third-party parts bundled inside
- **Console logos** (`assets/logos/*.webp`, except the Tools, Music, Books, Favorites, All games, Last played,
  Never played and Collections icons, which are original): taken from the *Iconic* EmulationStation theme by
  Delgan (https://github.com/Delgan/iconic-es), based on *iconic-es-de* by Siddy212, released under **CC0 1.0**.
  The console names and logos shown in them are trademarks of their respective owners.
- **Star and battery/Wi-Fi icons** (`assets/icons/*.svg`): from the same Iconic theme (CC0 1.0).
- **Font**: *Rubik* (`assets/fonts/Rubik-Regular.ttf`), Copyright 2015 The Rubik Project Authors
  (https://github.com/googlefonts/rubik), licensed under the **SIL Open Font License 1.1**.

## Notes
- The structure of the carousel (`itemTemplate`) follows properties already used by Iconic, only as a
  compatibility reference for this EmulationStation build; no layout or artwork was copied.
- Layout values for 1:1, 4:3, 3:2 and 16:9 screens are generated into `layout.xml`.
''')
