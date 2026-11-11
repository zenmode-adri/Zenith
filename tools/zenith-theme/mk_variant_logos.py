"""Sistemas variantes del tema Zenith (hacks, minis, DSiWare...): el logo es el de la consola base, sin nada pintado, y la
palabra la escribe el tema como texto (ES: zs-tag-* en theme.xml; zfront: la lee de este marcador), en Rubik y con el
acento para texto, así cambia con las variantes de color y se puede traducir.

Escribe, para cada variante:
  assets/logos/<variante>.webp                 copia del logo base (y en overlay-dark si la base tiene versión oscura)
  assets/logos/tags/<variante>.txt             la palabra (zfront la lee; ES mira si existe para subir el logo)
  assets/logos/tags/<palabra>/<variante>.txt   marcador para el <text> de esa palabra en theme.xml (exists())
Uso: python3 mk_variant_logos.py <directorio es-theme-zenith>
"""
import io
import os
import shutil
import sys

ROOT = sys.argv[1]
VARIANTS = {  # sistema: (logo base, palabra)
    'gbh': ('gb', 'Hacks'),
    'gbch': ('gbc', 'Hacks'),
    'gbah': ('gba', 'Hacks'),
    'nesh': ('nes', 'Hacks'),
    'snesh': ('snes', 'Hacks'),
    'snesnah': ('snesna', 'Hacks'),
    'genh': ('genesis', 'Hacks'),
    'ggh': ('gamegear', 'Hacks'),
    'megadriveh': ('megadrive', 'Hacks'),
    'gbav': ('gba', 'Video'),
    'pspminis': ('psp', 'Minis'),
    'ndsiware': ('nds', 'DSiWare'),
    'hbmame': ('mame', 'Homebrew'),
    'varcade': ('arcade', 'Vertical'),
}


def logos(sub):
    return os.path.join(ROOT, sub, 'assets', 'logos')


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with io.open(path, 'w', newline='') as f:
        f.write(text)


tags = os.path.join(logos('theme'), 'tags')
shutil.rmtree(tags, ignore_errors=True)
for key, (base, word) in VARIANTS.items():
    for sub in ('theme', 'overlay-dark'):
        src, dst = os.path.join(logos(sub), base + '.webp'), os.path.join(logos(sub), key + '.webp')
        if os.path.exists(src):
            shutil.copyfile(src, dst)
        elif os.path.exists(dst):
            os.remove(dst)  # sin versión oscura de la base: el tema oscuro usa la clara
    write(os.path.join(tags, key + '.txt'), word)
    write(os.path.join(tags, word.lower(), key + '.txt'), word)
print('ok', len(VARIANTS))
