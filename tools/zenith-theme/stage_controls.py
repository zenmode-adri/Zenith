"""Copia los recursos de controles al tema Zenith del fork y anade los bloques menuSwitch/menuSlider/menuButton/menuGroup
a theme/theme.xml (claro) y overlay-dark/theme.xml (oscuro), tras </menuIcons>. Ejecutar en WSL tras mkcontrols.py."""
import os, shutil, subprocess, sys

B = '/home/epsil/r36s-fork/projects/ROCKNIX/packages/ui/themes/es-theme-zenith'
SRC = '/mnt/c/tmp/zt/ctl-assets/assets/menu'
os.makedirs(f'{B}/theme/assets/menu', exist_ok=True)
for f in sorted(os.listdir(SRC)):
    shutil.copy(f'{SRC}/{f}', f'{B}/theme/assets/menu/{f}')


def block(group, on, off, band):
    return [
        '    <menuSwitch name="menuswitch">',
        '      <pathOn>./assets/menu/switch-on.svg</pathOn>',
        '      <pathOff>./assets/menu/switch-off.svg</pathOff>',
        f'      <colorOn>{on}</colorOn>',
        f'      <colorOff>{off}</colorOff>',
        '    </menuSwitch>',
        '    <menuSlider name="menuslider">',
        '      <path>./assets/menu/slider-knob.svg</path>',
        '    </menuSlider>',
        '    <menuButton name="menubutton">',
        '      <path>./assets/menu/button.png</path>',
        '      <filledPath>./assets/menu/button-filled.png</filledPath>',
        '      <cornerSize>16 16</cornerSize>',
        '    </menuButton>',
        '    <menuGroup name="menugroup">',
        f'      <color>{group}</color>',
        f'      <backgroundColor>{band}</backgroundColor>',
        '    </menuGroup>',
    ]


variants = {
    f'{B}/theme/theme.xml': block('7D7D82', '5CA0AF', 'C9C9CF', '0000000A'),
    f'{B}/overlay-dark/theme.xml': block('9A9AA2', '4C97A8', '5A5A62', 'FFFFFF0D'),
}
for path, blk in variants.items():
    raw = open(path, 'rb').read().decode('utf-8')
    if '<menuSwitch' in raw:
        print('ya tiene menuSwitch:', path)
        continue
    eol = '\r\n' if '\r\n' in raw else '\n'
    lines = raw.split(eol)
    idx = max(i for i, l in enumerate(lines) if '</menuIcons>' in l)
    lines[idx + 1:idx + 1] = blk
    open(path, 'wb').write(eol.join(lines).encode('utf-8'))
    print('ok', path)
