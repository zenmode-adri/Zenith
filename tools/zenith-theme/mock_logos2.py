"""Maqueta B / B+ de logos de sistemas (Zenith claro 1:1). B+ = estiramiento vertical suave:
  k = 1.12 para todos; los mas apaisados (aspecto > 5:1) llegan a 1.25.   Alto maximo 60 % de la tile, ancho 90 %.
Uso: python mock_logos2.py <salida.png> [--grid]"""
import sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

OUT = sys.argv[1]
GRID = '--grid' in sys.argv
S = 2
W = 720
ROW_H = 232
LOGOS = 'C:/tmp/zt/logos-mock/'
RUBIK = 'C:/Users/epsil/Documents/r36-kernel-mainline/SESION-2026-10-04-OS-DEFINITIVO/temas-zenith/zenith-switch-claro/assets/fonts/Rubik-Regular.ttf'
BG = (235, 235, 237)
CY = (0, 195, 227)
T, SEL, PITCH, R = 150, 196, 172, 14


def kfactor(aspect):
    return 1.25 if aspect > 5.0 else 1.12


def font(px):
    return ImageFont.truetype(RUBIK, int(px * S))


def tile(canvas, cx, cy, size, selected, logo, frac, stretch):
    x0, y0, x1, y1 = (cx - size / 2) * S, (cy - size / 2) * S, (cx + size / 2) * S, (cy + size / 2) * S
    sh = Image.new('RGBA', canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle((x0, y0 + 6 * S, x1, y1 + 6 * S), R * S, fill=(0, 0, 0, 55))
    canvas.alpha_composite(sh.filter(ImageFilter.GaussianBlur(7 * S)))
    d = ImageDraw.Draw(canvas)
    d.rounded_rectangle((x0, y0, x1, y1), R * S, fill=(255, 255, 255, 255))
    if selected:
        d.rounded_rectangle((x0, y0, x1, y1), R * S + 2, outline=CY + (255,), width=4 * S)
    aspect = logo.width / logo.height
    k = kfactor(aspect) if stretch else 1.0
    lw = frac * size
    lh = lw / aspect * k
    if lh > 0.60 * size:
        lh = 0.60 * size
        lw = lh * aspect / k
    lg = logo.resize((max(1, int(lw * S)), max(1, int(lh * S))), Image.LANCZOS)
    canvas.alpha_composite(lg, (int(cx * S - lg.width / 2), int(cy * S - lg.height / 2)))
    return lw, lh


def row(canvas, y0, title, names, stretch):
    d = ImageDraw.Draw(canvas)
    d.text((18 * S, (y0 + 16) * S), title, font=font(15), fill=(106, 106, 112), anchor='lm')
    cy = y0 + 128
    info = None
    for i, n in enumerate(names):
        cx = 360 + (i - 2) * PITCH
        logo = Image.open(LOGOS + n + '.png').convert('RGBA')
        if i != 2:
            tile(canvas, cx, cy, T, False, logo, 0.90, stretch)
    logo = Image.open(LOGOS + names[2] + '.png').convert('RGBA')
    info = tile(canvas, 360, cy, SEL, True, logo, 0.90, stretch)
    return info


canvas = Image.new('RGBA', (W * S, (ROW_H * 3 + 10) * S), BG + (255,))
a = row(canvas, 0, 'B   90 % del ancho, sin deformar', ['n64', 'snes', 'gba', 'switch', 'megadrive'], False)
b = row(canvas, ROW_H + 5, 'B+  igual con estiramiento vertical suave (x1,12 y hasta x1,25 en los muy apaisados)', ['n64', 'snes', 'gba', 'switch', 'megadrive'], True)
c = row(canvas, 2 * ROW_H + 10, 'B+  otros logos: Dreamcast, PlayStation, NES, Game Boy, PC Engine', ['dreamcast', 'psx', 'nes', 'gb', 'pcengine'], True)
print('GBA seleccionado: B %.0fx%.0f px  B+ %.0fx%.0f px' % (a[0], a[1], b[0], b[1]))
out = canvas.convert('RGB')
if GRID:
    d = ImageDraw.Draw(out, 'RGBA')
    f = ImageFont.truetype('C:/Windows/Fonts/consola.ttf', 10)
    for x in range(0, W + 1, 10):
        big = x % 50 == 0
        d.line((x * S, 0, x * S, out.height), fill=(255, 0, 80, 120 if big else 35), width=1)
        if big:
            d.text((x * S + 3, 2), str(x), font=f, fill=(255, 0, 80, 220))
    for y in range(0, ROW_H * 3 + 11, 10):
        big = y % 50 == 0
        d.line((0, y * S, out.width, y * S), fill=(0, 110, 255, 120 if big else 35), width=1)
        if big:
            d.text((3, y * S + 2), str(y), font=f, fill=(0, 110, 255, 220))
out.resize((W, ROW_H * 3 + 10), Image.LANCZOS).save(OUT)
print(OUT)
