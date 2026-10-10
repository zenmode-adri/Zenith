#!/usr/bin/env python3
"""Genera 0009-es-volume-osd.patch sobre el arbol de ES ya parcheado (0001-0008)."""
import glob
import os
import shutil
import subprocess
import sys

SRC = glob.glob('/home/epsil/r36s-fork/build.ROCKNIX-RK3326.aarch64/build/emulationstation-cada*/')[0].rstrip('/')
WORK = '/home/epsil/es0009'
shutil.rmtree(WORK, ignore_errors=True)
for d in ('a', 'b'):
    os.makedirs(f'{WORK}/{d}/es-core/src/components')
files = ['es-core/src/ThemeData.h', 'es-core/src/ThemeData.cpp',
         'es-core/src/components/VolumeInfoComponent.h', 'es-core/src/components/VolumeInfoComponent.cpp']
for f in files:
    shutil.copy(f'{SRC}/{f}', f'{WORK}/a/{f}')
    shutil.copy(f'{SRC}/{f}', f'{WORK}/b/{f}')

# el arbol de build ya lleva 0009: se revierte en las copias para tener la base sin 0009
OLD = '/mnt/c/tmp/zt/es/0009-es-volume-osd.patch'
for d in ('a', 'b'):
    subprocess.run(['patch', '-R', '-p1', '-s', '-i', OLD], cwd=f'{WORK}/{d}', check=False)

def edit(path, old, new, count=1):
    p = f'{WORK}/b/{path}'
    s = open(p, encoding='utf-8').read()
    if old not in s:
        sys.exit(f'NO ENCONTRADO en {path}: {old[:60]!r}')
    s = s.replace(old, new, count)
    open(p, 'w', encoding='utf-8').write(s)

# --- ThemeData.h: struct + miembro del menu ---
edit('es-core/src/ThemeData.h',
     'struct ButtonElement\n',
     '''struct VolumeElement
{
	Vector2f		pos;
	Vector2f		origin;
	Vector2f		size;		// 0 0 = automatic size
	float			radius;		// fraction of the height (0.5 = pill)
	unsigned int	color;		// background
	unsigned int	barColor;
	unsigned int	trackColor;
	unsigned int	textColor;
	std::shared_ptr<Font> font;
};

struct ButtonElement
''')
edit('es-core/src/ThemeData.h',
     '\t\tButtonElement Button { ":/button.png", ":/button_filled.png", Vector2f(16,16) };\n',
     '\t\tButtonElement Button { ":/button.png", ":/button_filled.png", Vector2f(16,16) };\n'
     '\t\tVolumeElement Volume { Vector2f(0.5f, 0.054f), Vector2f(0.5f, 0.0f), Vector2f(0, 0), 0.5f, 0x111111FF, 0xFFFFFFFF, 0xFFFFFF33, 0xFFFFFFFF, nullptr };\n')

# --- ThemeData.cpp: propiedades del elemento y lectura ---
edit('es-core/src/ThemeData.cpp',
     '\t{ "menuButton",{\n',
     '''\t{ "menuVolume",{
		{ "pos", NORMALIZED_PAIR },
		{ "origin", NORMALIZED_PAIR },
		{ "size", NORMALIZED_PAIR },
		{ "cornerRadius", FLOAT },
		{ "color", COLOR },
		{ "barColor", COLOR },
		{ "trackColor", COLOR },
		{ "textColor", COLOR },
		{ "fontPath", PATH },
		{ "fontSize", FLOAT } } },
	{ "menuButton",{
''')
edit('es-core/src/ThemeData.cpp',
     '\t// switch auto\n',
     '''\t// Volume popup: colours derived from the menu theme, optionally overridden by <menuVolume name="menuvolume">
	Volume.color = (((Background.centerColor & 0xFF) ? Background.centerColor : Background.color) & 0xFFFFFF00) | 0xFF;
	Volume.textColor = (Title.color & 0xFFFFFF00) | 0xFF;
	Volume.barColor = (Text.selectedColor & 0xFFFFFF00) | 0xFF;
	Volume.trackColor = (Title.color & 0xFFFFFF00) | 0x33;
	Volume.font = TextSmall.font;

	elem = theme->getElement("menu", "menuvolume", "menuVolume");
	if (elem)
	{
		if (elem->has("pos"))
			Volume.pos = elem->get<Vector2f>("pos");
		if (elem->has("origin"))
			Volume.origin = elem->get<Vector2f>("origin");
		if (elem->has("size"))
			Volume.size = elem->get<Vector2f>("size");
		if (elem->has("cornerRadius"))
			Volume.radius = elem->get<float>("cornerRadius");
		if (elem->has("color"))
			Volume.color = elem->get<unsigned int>("color");
		if (elem->has("barColor"))
			Volume.barColor = elem->get<unsigned int>("barColor");
		if (elem->has("trackColor"))
			Volume.trackColor = elem->get<unsigned int>("trackColor");
		if (elem->has("textColor"))
			Volume.textColor = elem->get<unsigned int>("textColor");
		if (elem->has("fontPath") || elem->has("fontSize"))
			Volume.font = Font::getFromTheme(elem, ThemeFlags::ALL, TextSmall.font, true);
	}

	// switch auto
''')

# --- componente nuevo (reemplazo completo) ---
for f in ('es-core/src/components/VolumeInfoComponent.h', 'es-core/src/components/VolumeInfoComponent.cpp'):
    shutil.copy('/mnt/c/tmp/zt/es/' + os.path.basename(f), f'{WORK}/b/{f}')

r = subprocess.run(['diff', '-ruN', 'a', 'b'], cwd=WORK, capture_output=True, text=True)
patch = r.stdout.replace('\r\n', '\n')
header = ('Zenith 0009: volume popup redesign. Default: a pill at the top center (icon, bar, percentage) coloured from the\n'
          'menu theme; themes can restyle it with <menuVolume name="menuvolume"> in the menu view (pos, origin, size,\n'
          'cornerRadius, color, barColor, trackColor, textColor, fontPath, fontSize).\n\n')
open('/mnt/c/tmp/zt/es/0009-es-volume-osd.patch', 'w', encoding='utf-8', newline='\n').write(header + patch)
print('patch bytes', len(patch))
