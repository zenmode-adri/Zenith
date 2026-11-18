#!/usr/bin/env python3
"""Genera 0013-es-help-width.patch sobre el arbol de ES ya parcheado (0001-0012)."""
import glob, os, shutil, subprocess, sys

SRC = glob.glob('/home/epsil/r36s-fork/build.ROCKNIX-RK3326.aarch64/build/emulationstation-cada*/')[0].rstrip('/')
WORK = '/home/epsil/es0013'
shutil.rmtree(WORK, ignore_errors=True)
F = 'es-core/src/components/HelpComponent.cpp'
for d in ('a', 'b'):
    os.makedirs(os.path.dirname(f'{WORK}/{d}/{F}'), exist_ok=True)
    shutil.copy(f'{SRC}/{F}', f'{WORK}/{d}/{F}')

OLD = '/mnt/c/tmp/zt/es/0013-es-help-width.patch'
if os.path.exists(OLD):
    for d in ('a', 'b'):
        subprocess.run(['patch', '-R', '-p1', '-s', '-f', '-i', OLD], cwd=f'{WORK}/{d}', check=False)


def edit(old, new):
    p = f'{WORK}/b/{F}'
    s = open(p, encoding='utf-8', newline='').read()
    if s.count(old) != 1:
        sys.exit(f'{s.count(old)} COINCIDENCIAS: {old[:80]!r}')
    open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))


edit('\t\twidth += icon->getSize().x() + lbl->getSize().x() + ICON_TEXT_SPACING + ENTRY_SPACING;\n\t\tif (width >= maxWidth)\n\t\t\tbreak;\n',
     '\t\t// Zenith: only count an entry that fits. Before, the width of the entry that did NOT fit was added too, so the bar\n'
     '\t\t// kept a box wider than its visible entries and, being centred on the box, looked pushed to the left.\n'
     '\t\tconst float entryWidth = icon->getSize().x() + lbl->getSize().x() + ICON_TEXT_SPACING + ENTRY_SPACING;\n'
     '\t\tif (width + entryWidth >= maxWidth)\n\t\t\tbreak;\n'
     '\t\twidth += entryWidth;\n')

r = subprocess.run(['diff', '-ruN', 'a', 'b'], cwd=WORK, capture_output=True, text=True)
patch = r.stdout.replace('\r\n', '\n')
header = ('Zenith 0013: help bar width. HelpComponent added the width of the entry that did not fit before dropping it, so the grid kept a\n'
          'width larger than the entries actually shown and the bar (centred on that box) appeared shifted to the left, e.g. at 640x480 where\n'
          '"SAVE STATES" is dropped. Only entries that fit are now counted.\n\n')
open('/mnt/c/tmp/zt/es/0013-es-help-width.patch', 'w', encoding='utf-8', newline='\n').write(header + patch)
print('patch bytes', len(patch))
