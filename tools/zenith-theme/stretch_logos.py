"""Estira en vertical los logos de sistemas de Zenith (claro y oscuro) a partir de las copias originales.
Regla: aspecto < 2:1 -> x1,00 (iconos y logos casi cuadrados, no se tocan); 2-5:1 -> x1,12; > 5:1 -> x1,25.
Uso: python stretch_logos.py   (lee C:/tmp/zt/logos-orig/{light,dark}, escribe en el fork)"""
import glob
import os
import shutil

from PIL import Image

FORK = '//wsl.localhost/Ubuntu-24.04/home/epsil/r36s-fork/projects/ROCKNIX/packages/ui/themes/es-theme-zenith/'
SETS = {'light': FORK + 'theme/assets/logos/', 'dark': FORK + 'overlay-dark/assets/logos/'}
BK = 'C:/tmp/zt/logos-orig/'


def kf(a):
    return 1.0 if a < 2 else (1.25 if a > 5 else 1.12)


for name, dst in SETS.items():
    bk = BK + name + '/'
    if not os.path.isdir(bk):          # primera vez: guarda los originales
        os.makedirs(bk)
        for p in glob.glob(dst + '*.webp'):
            shutil.copyfile(p, bk + os.path.basename(p))
    n = ch = 0
    for p in sorted(glob.glob(bk + '*.webp')):
        im = Image.open(p).convert('RGBA')
        k = kf(im.width / im.height)
        out = dst + os.path.basename(p)
        if k == 1.0:
            shutil.copyfile(p, out)
        else:
            im.resize((im.width, round(im.height * k)), Image.LANCZOS).save(out, 'WEBP', lossless=True, quality=100, method=6)
            ch += 1
        n += 1
    print(name, 'logos:', n, 'estirados:', ch)
