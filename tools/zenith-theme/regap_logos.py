"""Separa mas el icono de su etiqueta en los logos propios de Zenith (icono + etiqueta van en una sola imagen 360x300).
Hueco icono -> etiqueta: antes ~34 (8-9 px en la tesela), ahora GAP (~15 px en la tesela). El conjunto se centra en vertical."""
import os, sys
import numpy as np
from PIL import Image

SRC = 'C:/tmp/zt/logos-src'
OUT = 'C:/tmp/zt/logos-out'
GAP = 76
NAMES = ['music', 'tools', 'auto-favorites', 'auto-allgames', 'auto-lastplayed', 'auto-neverplayed', 'ebook', 'custom-collections']


def segments(a):
    rows = (a > 40).sum(axis=1)
    ys = [y for y in range(len(rows)) if rows[y] > 0]
    segs = []
    s = p = ys[0]
    for y in ys[1:]:
        if y != p + 1:
            segs.append((s, p))
            s = y
        p = y
    segs.append((s, p))
    return segs


for variant in ('light', 'dark'):
    os.makedirs(f'{OUT}/{variant}', exist_ok=True)
    for n in NAMES:
        src = f'{SRC}/{variant}/{n}.webp'
        if not os.path.exists(src):
            continue
        im = Image.open(src).convert('RGBA')
        a = np.array(im)[:, :, 3]
        segs = segments(a)
        # etiqueta = ultimo segmento; icono = todo lo anterior (puede tener huecos internos, como "All games")
        label = segs[-1]
        icon = (segs[0][0], segs[-2][1])
        gap_old = label[0] - icon[1] - 1
        ih, lh = icon[1] - icon[0] + 1, label[1] - label[0] + 1
        total = ih + GAP + lh
        top = (im.height - total) // 2
        out = Image.new('RGBA', im.size, (0, 0, 0, 0))
        out.paste(im.crop((0, icon[0], im.width, icon[1] + 1)), (0, top))
        out.paste(im.crop((0, label[0], im.width, label[1] + 1)), (0, top + ih + GAP))
        out.save(f'{OUT}/{variant}/{n}.webp', lossless=True, quality=100, method=6)
        print(variant, n, 'icono', icon, 'etiqueta', label, 'hueco', gap_old, '->', GAP, 'inicio', top)
