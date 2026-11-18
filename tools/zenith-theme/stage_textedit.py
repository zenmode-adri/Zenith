"""Copia los PNG del campo de texto al tema Zenith del fork y anade <menuTextEdit> a theme.xml (claro) y overlay-dark/theme.xml.
Ejecutar en WSL tras mk_textedit.py."""
import os, shutil

B = '/home/epsil/r36s-fork/projects/ROCKNIX/packages/ui/themes/es-theme-zenith'
SRC = '/mnt/c/tmp/zt/te-assets'
for variant, dest in (('light', f'{B}/theme/assets/menu'), ('dark', f'{B}/overlay-dark/assets/menu')):
    os.makedirs(dest, exist_ok=True)
    for f in ('textinput.png', 'textinput-active.png'):
        shutil.copy(f'{SRC}/{variant}/{f}', f'{dest}/{f}')

BLOCK = ['    <menuTextEdit name="menutextedit">',
         '      <active>./assets/menu/textinput-active.png</active>',
         '      <inactive>./assets/menu/textinput.png</inactive>',
         '    </menuTextEdit>']
for path in (f'{B}/theme/theme.xml', f'{B}/overlay-dark/theme.xml'):
    raw = open(path, 'rb').read().decode('utf-8')
    if '<menuTextEdit' in raw:
        print('ya tiene menuTextEdit:', path)
        continue
    eol = '\r\n' if '\r\n' in raw else '\n'
    lines = raw.split(eol)
    idx = max(i for i, l in enumerate(lines) if '</menuGroup>' in l)
    lines[idx + 1:idx + 1] = BLOCK
    open(path, 'wb').write(eol.join(lines).encode('utf-8'))
    print('ok', path)
