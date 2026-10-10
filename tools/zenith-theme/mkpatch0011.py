#!/usr/bin/env python3
"""Genera 0011-es-switch-colors.patch sobre el arbol de ES ya parcheado (0001-0010)."""
import glob, os, shutil, subprocess, sys

SRC = glob.glob('/home/epsil/r36s-fork/build.ROCKNIX-RK3326.aarch64/build/emulationstation-cada*/')[0].rstrip('/')
WORK = '/home/epsil/es0011'
shutil.rmtree(WORK, ignore_errors=True)
files = ['es-core/src/ThemeData.h', 'es-core/src/ThemeData.cpp', 'es-core/src/components/SwitchComponent.cpp']
for d in ('a', 'b'):
    for f in files:
        os.makedirs(os.path.dirname(f'{WORK}/{d}/{f}'), exist_ok=True)
        shutil.copy(f'{SRC}/{f}', f'{WORK}/{d}/{f}')


# el arbol de build ya lleva 0011 (ultima compilacion): se revierte en las copias para partir de la base sin 0011
OLD = '/mnt/c/tmp/zt/es/0011-es-switch-colors.patch'
if os.path.exists(OLD):
    for d in ('a', 'b'):
        subprocess.run(['patch', '-R', '-p1', '-s', '-i', OLD], cwd=f'{WORK}/{d}', check=False)


def edit(path, old, new):
    p = f'{WORK}/b/{path}'
    s = open(p, encoding='utf-8', newline='').read()
    if s.count(old) != 1:
        sys.exit(f'{s.count(old)} COINCIDENCIAS en {path}: {old[:70]!r}')
    open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))


# --- ThemeData.h: colores del interruptor
edit('es-core/src/ThemeData.h',
     '\tstd::string textinput_ninepatch_active;\n};',
     '\tstd::string textinput_ninepatch_active;\n\n'
     '\t// optional tint of the menu switch per state (<menuSwitch><colorOn/><colorOff>); 0 = use the menu text colour\n'
     '\tbool hasSwitchColors = false;\n\tunsigned int switchOnColor = 0xFFFFFFFF;\n\tunsigned int switchOffColor = 0xFFFFFFFF;\n};')

# --- ThemeData.cpp: propiedades y lectura
edit('es-core/src/ThemeData.cpp',
     '\t{ "menuSwitch",{\n\t\t{ "pathOn", PATH },\n\t\t{ "pathOff", PATH } } },',
     '\t{ "menuSwitch",{\n\t\t{ "pathOn", PATH },\n\t\t{ "pathOff", PATH },\n\t\t{ "colorOn", COLOR },\n\t\t{ "colorOff", COLOR } } },')
edit('es-core/src/ThemeData.cpp',
     '\t\tif (elem->has("pathOff") && ResourceManager::getInstance()->fileExists(elem->get<std::string>("pathOff")))\n'
     '\t\t\tIcons.off = elem->get<std::string>("pathOff");\n',
     '\t\tif (elem->has("pathOff") && ResourceManager::getInstance()->fileExists(elem->get<std::string>("pathOff")))\n'
     '\t\t\tIcons.off = elem->get<std::string>("pathOff");\n'
     '\t\tif (elem->has("colorOn") && elem->has("colorOff"))\n\t\t{\n'
     '\t\t\tIcons.switchOnColor = elem->get<unsigned int>("colorOn");\n'
     '\t\t\tIcons.switchOffColor = elem->get<unsigned int>("colorOff");\n'
     '\t\t\tIcons.hasSwitchColors = true;\n\t\t}\n')

# --- SwitchComponent.cpp: tinte por estado
edit('es-core/src/components/SwitchComponent.cpp',
     '\tmImage.setColorShift(menuTheme->Text.color);\n\tmImage.setResize(0, height);',
     '\tmImage.setColorShift(menuTheme->Icons.hasSwitchColors ? menuTheme->Icons.switchOffColor : menuTheme->Text.color);\n\tmImage.setResize(0, height);')
edit('es-core/src/components/SwitchComponent.cpp',
     'void SwitchComponent::setColor(unsigned int color) \n{\n\tmImage.setColorShift(color);\n}',
     'void SwitchComponent::setColor(unsigned int color) \n{\n\t// themes that colour the switch per state keep their colours when the row is selected\n'
     '\tif (ThemeData::getMenuTheme()->Icons.hasSwitchColors)\n\t\treturn;\n\n\tmImage.setColorShift(color);\n}')
edit('es-core/src/components/SwitchComponent.cpp',
     '\tmImage.setImage((mHasAuto && mAutoState) ? theme->Icons.onoffauto : (mState ? theme->Icons.on : theme->Icons.off));\n',
     '\tmImage.setImage((mHasAuto && mAutoState) ? theme->Icons.onoffauto : (mState ? theme->Icons.on : theme->Icons.off));\n'
     '\tif (theme->Icons.hasSwitchColors)\n\t\tmImage.setColorShift(mState ? theme->Icons.switchOnColor : theme->Icons.switchOffColor);\n')

# imagen del interruptor con carga inmediata: con una imagen del tema (no precargada) el ancho medido en el
# constructor era 0 y la fila dejaba el interruptor fuera del panel
edit('es-core/src/components/SwitchComponent.cpp',
     'GuiComponent(window), mImage(window), mState(state)',
     'GuiComponent(window), mImage(window, true), mState(state)')

r = subprocess.run(['diff', '-ruN', 'a', 'b'], cwd=WORK, capture_output=True, text=True)
patch = r.stdout.replace('\r\n', '\n')
header = ('Zenith 0011: menu switch colours. <menuSwitch> accepts <colorOn> and <colorOff>: the switch image is tinted per\n'
          'state (and keeps its colours when the row is selected), so a theme can draw a coloured on/off switch.\n\n')
open('/mnt/c/tmp/zt/es/0011-es-switch-colors.patch', 'w', encoding='utf-8', newline='\n').write(header + patch)
print('patch bytes', len(patch))
