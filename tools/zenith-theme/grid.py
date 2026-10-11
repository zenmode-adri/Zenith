"""Cuadricula de medicion sobre una captura.
Uso: python grid.py <entrada.png> <salida.png> [paso=10] [zoom=1] [x0 y0 x1 y1]
Dibuja lineas cada <paso> px (cada 5 pasos mas fuertes, con la coordenada en px de la captura original) y devuelve
la imagen ampliada <zoom> veces. Con x0 y0 x1 y1 recorta antes (en px originales, la cuadricula conserva las coordenadas).
"""
import sys
from PIL import Image, ImageDraw, ImageFont

src, dst = sys.argv[1], sys.argv[2]
step = int(sys.argv[3]) if len(sys.argv) > 3 else 10
zoom = int(sys.argv[4]) if len(sys.argv) > 4 else 1
im = Image.open(src).convert('RGB')
if len(sys.argv) > 8:
    x0, y0, x1, y1 = map(int, sys.argv[5:9])
else:
    x0, y0, x1, y1 = 0, 0, im.width, im.height
im = im.crop((x0, y0, x1, y1)).resize(((x1 - x0) * zoom, (y1 - y0) * zoom), Image.NEAREST if zoom > 1 else Image.LANCZOS)
d = ImageDraw.Draw(im, 'RGBA')
f = ImageFont.truetype('C:/Windows/Fonts/consola.ttf', 10 if zoom < 3 else 12)
gx = (x0 // step + (1 if x0 % step else 0)) * step
for x in range(gx, x1 + 1, step):
    big = (x // step) % 5 == 0
    X = (x - x0) * zoom
    d.line((X, 0, X, im.height), fill=(255, 0, 80, 150 if big else 55), width=1)
    if big:
        d.text((X + 2, 1), str(x), font=f, fill=(255, 0, 80, 230))
gy = (y0 // step + (1 if y0 % step else 0)) * step
for y in range(gy, y1 + 1, step):
    big = (y // step) % 5 == 0
    Y = (y - y0) * zoom
    d.line((0, Y, im.width, Y), fill=(0, 110, 255, 150 if big else 55), width=1)
    if big:
        d.text((1, Y + 1), str(y), font=f, fill=(0, 110, 255, 230))
im.save(dst)
print(dst, im.size)
