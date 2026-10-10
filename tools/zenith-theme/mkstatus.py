"""Iconos de estado (bateria y wifi) de trazo fino, a tamano nativo (la caja del stackpanel mide 24 px de alto)."""
import math
import os
import sys
from PIL import Image, ImageDraw

A = os.path.join(sys.argv[1], 'assets', 'icons')
S = 16
H = 24
W = (255, 255, 255)


def new(w):
    return Image.new('L', (w * S, H * S), 0)


def out(mask, w, name):
    m = mask.resize((w, H), Image.LANCZOS)
    im = Image.new('RGBA', (w, H), W + (0,))
    im.putalpha(m)
    im.save(os.path.join(A, name))


def battery(level, charging=False, w=34):
    m = new(w)
    d = ImageDraw.Draw(m)
    x0, y0, x1, y1 = 3, 6, 29, 18          # cuerpo 26x12 centrado en 34x24
    lw = 1.6
    d.rounded_rectangle((x0 * S, y0 * S, x1 * S, y1 * S), 2.6 * S, outline=255, width=int(lw * S))
    d.rounded_rectangle((29.8 * S, 9.4 * S, 31.6 * S, 14.6 * S), 0.8 * S, fill=255)   # polo
    if level > 0:
        ix0, iy0, iy1 = x0 + 3.2, y0 + 3.2, y1 - 3.2
        full = (x1 - 3.2) - ix0
        d.rounded_rectangle((ix0 * S, iy0 * S, (ix0 + full * level) * S, iy1 * S), 0.8 * S, fill=255)
    if charging:
        # bateria con el hueco central limpio y un rayo solido
        d.rectangle(((x0 + 2.4) * S, (y0 + 2.4) * S, (x1 - 2.4) * S, (y1 - 2.4) * S), fill=0)
        cx, cy = 16, 12
        bolt = [(cx + 2.0, cy - 4.4), (cx - 3.0, cy + 0.6), (cx - 0.3, cy + 0.6), (cx - 2.0, cy + 4.4), (cx + 3.0, cy - 0.6), (cx + 0.3, cy - 0.6)]
        d.polygon([(x * S, y * S) for x, y in bolt], fill=255)
    return m, w


def wifi(off=False, w=30):
    m = new(w)
    d = ImageDraw.Draw(m)
    cx, cy = 15, 20.2
    lw = int(1.9 * S)
    for r in (15.2, 10.2, 5.2):
        box = ((cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S)
        d.arc(box, 225, 315, fill=255, width=lw)
    d.ellipse(((cx - 1.7) * S, (cy - 1.7) * S, (cx + 1.7) * S, (cy + 1.7) * S), fill=255)
    if off:
        d.line([(5 * S, 21 * S), (25 * S, 3.6 * S)], fill=0, width=int(4.2 * S))
        d.line([(5 * S, 21 * S), (25 * S, 3.6 * S)], fill=255, width=int(1.7 * S))
    return m, w


for name, lvl, ch in (('100', 1.0, False), ('75', 0.75, False), ('50', 0.5, False), ('25', 0.25, False), ('0', 0.0, False), ('charging', 0.6, True)):
    m, w = battery(lvl, ch)
    out(m, w, f'battery-icon-{name}.png')
for name, off in (('on', False), ('off', True)):
    m, w = wifi(off)
    out(m, w, f'network-icon-{name}.png')
print('ok')
