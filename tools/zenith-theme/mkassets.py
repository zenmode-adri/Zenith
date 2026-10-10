"""Genera los PNG del tema Zenith Switch (claro). Salida: <out>/assets/"""
import os
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1]
A = os.path.join(OUT, 'assets')
os.makedirs(A, exist_ok=True)
S = 4  # supersampling
CY = (0, 195, 227)


def canvas(n):
    return Image.new('RGBA', (n * S, n * S), (0, 0, 0, 0))


def done(im, n, name):
    im.resize((n, n), Image.LANCZOS).save(os.path.join(A, name))


def layer(mask, color, opacity=1.0):
    """RGBA con color solido y alfa = mascara (sin halos oscuros al difuminar)."""
    r = Image.new('RGBA', mask.size, color + (0,))
    r.putalpha(mask.point(lambda v: int(v * opacity)))
    return r


def rrmask(n, box, radius, width=None):
    m = Image.new('L', (n * S, n * S), 0)
    d = ImageDraw.Draw(m)
    b = [v * S for v in box]
    if width is None:
        d.rounded_rectangle(b, radius * S, fill=255)
    else:
        d.rounded_rectangle(b, radius * S, outline=255, width=width * S)
    return m


# --- tile.png: 320x320, tarjeta blanca 256 con sombra suave ---
N = 384
M = 64  # margen del lienzo: la sombra/halo no se corta en el borde
im = canvas(N)
sh = rrmask(N, (M, M + 8, M + 256, M + 264), 17).filter(ImageFilter.GaussianBlur(11 * S))
im.alpha_composite(layer(sh, (30, 40, 80), 0.27))
im.alpha_composite(layer(rrmask(N, (M, M, M + 256, M + 256), 17), (255, 255, 255)))
done(im, N, 'tile.png')

# --- frame.png: relleno blanco hasta el aro + aro cian + halo suave cian ---
im = canvas(N)
OUTER = (M - 10, M - 10, M + 256 + 10, M + 256 + 10)
halo = rrmask(N, OUTER, 20, width=7).filter(ImageFilter.GaussianBlur(7 * S))
im.alpha_composite(layer(halo, CY, 0.75))
im.alpha_composite(layer(rrmask(N, OUTER, 20), (255, 255, 255)))
im.alpha_composite(layer(rrmask(N, OUTER, 20, width=7), CY))
done(im, N, 'frame.png')

# --- gridframe.png: ninepatch 48x48 (esquinas 16) para la casilla seleccionada del grid ---
n = 48
im = canvas(n)
d = ImageDraw.Draw(im)
d.rounded_rectangle((1 * S, 1 * S, 47 * S, 47 * S), 9 * S, outline=CY + (255,), width=4 * S)
done(im, n, 'gridframe.png')

# --- px.png: pixel blanco (fondos de color) ---
Image.new('RGBA', (4, 4), (255, 255, 255, 255)).save(os.path.join(A, 'px.png'))

# --- avatar.png: circulo cian con Z ---
n = 128
im = canvas(n)
d = ImageDraw.Draw(im)
d.ellipse((4 * S, 4 * S, 124 * S, 124 * S), fill=CY + (255,))
f = ImageFont.truetype('C:/Windows/Fonts/segoeuib.ttf', 66 * S)
d.text((64 * S, 64 * S), 'Z', font=f, fill=(255, 255, 255, 255), anchor='mm')
done(im, n, 'avatar.png')

# --- iconos de ayuda A/B/X/Y: aro + letra, blanco (se tiñe con iconColor) ---
for k in 'ABXY':
    n = 96
    im = canvas(n)
    d = ImageDraw.Draw(im)
    d.ellipse((6 * S, 6 * S, 90 * S, 90 * S), outline=(255, 255, 255, 255), width=6 * S)
    f = ImageFont.truetype('C:/Windows/Fonts/segoeuib.ttf', 46 * S)
    d.text((48 * S, 50 * S), k, font=f, fill=(255, 255, 255, 255), anchor='mm')
    done(im, n, f'help-{k}.png')
print('ok', os.listdir(A))
