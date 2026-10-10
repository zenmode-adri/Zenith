"""Campo de texto del teclado en pantalla (nine-patch 48x48, esquina de 16 px): reposo y activo, variantes clara y oscura.
Salida: C:/tmp/zt/te-assets/{light,dark}/textinput.png y textinput-active.png"""
import os
from PIL import Image, ImageDraw

OUT = 'C:/tmp/zt/te-assets'
S = 8
N = 48
R = 12        # radio de la esquina (debe caber en los 16 px de la esquina del nine-patch)


def make(path, fill, border, bw):
    im = Image.new('RGBA', (N * S, N * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, N * S - 1, N * S - 1], radius=R * S, fill=border)
    d.rounded_rectangle([bw * S, bw * S, N * S - 1 - bw * S, N * S - 1 - bw * S], radius=(R - bw) * S, fill=fill)
    im.resize((N, N), Image.LANCZOS).save(path)


variants = {
    'light': dict(fill=(255, 255, 255, 255), rest=(201, 201, 207, 255), active=(0, 195, 227, 255)),
    'dark': dict(fill=(42, 42, 47, 255), rest=(90, 90, 98, 255), active=(0, 195, 227, 255)),
}
for name, v in variants.items():
    os.makedirs(f'{OUT}/{name}', exist_ok=True)
    make(f'{OUT}/{name}/textinput.png', v['fill'], v['rest'], 2)
    make(f'{OUT}/{name}/textinput-active.png', v['fill'], v['active'], 2)
print('ok', os.listdir(OUT))
