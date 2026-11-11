"""Logos de trazo de las utilidades Capturas (imageviewer) y Reproductor (mplayer) del tema Zenith.

Mismo formato que music/tools (mkassets2.py + zenith-77): lienzo 360x300 dibujado a 4x, trazo 9 px, solo el icono (la
palabra la pone el tema como texto). Claro en theme/, oscuro en overlay-dark/.
Uso: python3 mk_util_icons.py <directorio es-theme-zenith>
"""
import os
import sys
from PIL import Image, ImageDraw

ROOT = sys.argv[1]
S = 4
LW, LH = 360, 300
SW = 9 * S
COLORS = {'theme': (0, 134, 159, 255), 'overlay-dark': (79, 216, 241, 255)}


def p(*xy):
    return [(x * S, y * S) for x, y in zip(xy[::2], xy[1::2])]


def ic_screenshots(d, c):
    # foto: marco redondeado, montaña y sol (caja del glifo ~x 98-262, y 30-150, como music/tools)
    d.rounded_rectangle((100 * S, 34 * S, 260 * S, 150 * S), 16 * S, outline=c, width=SW)
    d.ellipse((205 * S, 56 * S, 233 * S, 84 * S), outline=c, width=SW)
    # la montaña empieza y acaba en los lados rectos del marco (no en las esquinas redondas)
    d.line(p(104, 128, 148, 88, 186, 124, 210, 102, 256, 130), fill=c, width=SW, joint='curve')


def ic_mplayer(d, c):
    # reproductor: círculo con el triángulo de «play»
    d.ellipse((118 * S, 30 * S, 242 * S, 154 * S), outline=c, width=SW)
    d.line(p(166, 66, 166, 118, 208, 92, 166, 66), fill=c, width=SW, joint='curve')


ICONS = {'imageviewer': ic_screenshots, 'mplayer': ic_mplayer}

for sub, c in COLORS.items():
    out = os.path.join(ROOT, sub, 'assets', 'logos')
    os.makedirs(out, exist_ok=True)
    for key, fn in ICONS.items():
        im = Image.new('RGBA', (LW * S, LH * S), (0, 0, 0, 0))
        fn(ImageDraw.Draw(im), c)
        im.resize((LW, LH), Image.LANCZOS).save(os.path.join(out, key + '.webp'), quality=95, method=6)
print('ok')
