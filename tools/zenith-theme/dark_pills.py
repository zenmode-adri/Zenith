"""Variantes oscuras de las pildoras de seleccion: mismo canal alfa que las claras de gen-out, color (31, 74, 87)."""
import os
import numpy as np
from PIL import Image

SRC = 'C:/tmp/zt/gen-out/assets'
OUT = 'C:/tmp/zt/dark-pills-out'
os.makedirs(OUT, exist_ok=True)
n = 0
for f in sorted(os.listdir(SRC)):
    if not f.startswith('pill'):
        continue
    a = np.array(Image.open(f'{SRC}/{f}').convert('RGBA'))
    a[:, :, 0], a[:, :, 1], a[:, :, 2] = 31, 74, 87
    Image.fromarray(a).save(f'{OUT}/{f}')
    n += 1
print('pildoras oscuras', n)
