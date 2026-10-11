"""Hojas de contacto de logos: cada celda = par (original a ancho 90 % / estirado B+) dentro de tiles blancas.
Uso: python contact_logos.py <grupo> <pagina> [--grid]   grupos: wide (aspecto>5), mid (2-5), sq (<2)
Caja del logo en la tile: 90 % de ancho x 72 % de alto (la caja actual es cuadrada de 72 %: los casi cuadrados no encogen)."""
import glob
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

D = '//wsl.localhost/Ubuntu-24.04/home/epsil/r36s-fork/projects/ROCKNIX/packages/ui/themes/es-theme-zenith/theme/assets/logos/'
GROUP, PAGE = sys.argv[1], int(sys.argv[2])
S = 2
TILE = 118
COLS = 6
PER = 30                      # 6 x 5 pares por pagina
BG = (235, 235, 237)
F = ImageFont.truetype('C:/Windows/Fonts/consola.ttf', 10 * S)


def kf(a):
    return 1.0 if a < 2 else (1.25 if a > 5 else 1.12)


logos = []
for p in sorted(glob.glob(D + '*.webp')):
    im = Image.open(p).convert('RGBA')
    a = im.width / im.height
    g = 'wide' if a > 5 else ('mid' if a >= 2 else 'sq')
    if g == GROUP:
        logos.append((os.path.basename(p)[:-5], im, a))
logos = logos[PAGE * PER:(PAGE + 1) * PER]
rows = math.ceil(len(logos) / COLS)
CW = 2 * TILE + 6
CH = TILE + 16
sheet = Image.new('RGB', (COLS * CW * S + 6 * S, rows * CH * S + 6 * S), BG)
d = ImageDraw.Draw(sheet)


def render(im, a, stretch):
    t = TILE * S
    tile = Image.new('RGB', (t, t), (255, 255, 255))
    k = kf(a) if stretch else 1.0
    lw = 0.90 * t
    lh = lw / a * k
    if lh > 0.72 * t:
        lh = 0.72 * t
        lw = lh * a / k
    lg = im.resize((max(1, int(lw)), max(1, int(lh))), Image.LANCZOS)
    tile.paste(lg, (int((t - lg.width) / 2), int((t - lg.height) / 2)), lg)
    return tile


for i, (n, im, a) in enumerate(logos):
    cx = (i % COLS) * CW * S + 6 * S
    cy = (i // COLS) * CH * S + 6 * S
    sheet.paste(render(im, a, False), (cx, cy))
    sheet.paste(render(im, a, True), (cx + (TILE + 2) * S, cy))
    d.text((cx, cy + TILE * S + 2), '%s %.1f:1 x%.2f' % (n, a, kf(a)), font=F, fill=(60, 60, 66))
if '--grid' in sys.argv:
    dd = ImageDraw.Draw(sheet, 'RGBA')
    for x in range(0, sheet.width, 10 * S):
        dd.line((x, 0, x, sheet.height), fill=(255, 0, 80, 28))
    for y in range(0, sheet.height, 10 * S):
        dd.line((0, y, sheet.width, y), fill=(0, 110, 255, 28))
out = 'C:/tmp/zt/contact-%s-%d.png' % (GROUP, PAGE)
sheet.resize((sheet.width // S, sheet.height // S), Image.LANCZOS).save(out)
print(out, len(logos), sheet.width // S, sheet.height // S)
