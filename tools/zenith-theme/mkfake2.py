"""Juegos de prueba con caratulas VERTICALES (megadrive, 3:4) y APAISADAS (snes, 7:5) para probar cuadriculas de tiles.
Salida: C:/tmp/zt/fakemd y C:/tmp/zt/fakesnes (ROMs vacias ZZTEST + gamelist.xml + images)."""
import os
import shutil
from PIL import Image, ImageDraw

names = ['Sonic the Hedgehog', 'Streets of Rage 2', 'Phantasy Star IV', 'Gunstar Heroes',
         'Ecco the Dolphin', 'Shinobi III', 'Comix Zone', 'Golden Axe']
snes = ['Super Mario World', 'Zelda: A Link to the Past', 'Super Metroid', 'Donkey Kong Country',
        'Chrono Trigger', 'F-Zero', 'Kirby Super Star', 'Street Fighter II Turbo']


def build(out, ext, titles, size, tint):
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out + '/images')
    xml = ['<?xml version="1.0"?>', '<gameList>']
    for i, t in enumerate(titles, start=1):
        fn = f'ZZTEST {i:02d}.{ext}'
        open(f'{out}/{fn}', 'wb').close()
        src = Image.open(f'C:/tmp/zt/covers/{i:02d}.png').convert('RGB')
        W, H = size
        im = Image.new('RGB', size, tint[i % len(tint)])
        d = ImageDraw.Draw(im)
        d.rectangle((6, 6, W - 7, H - 7), outline=(255, 255, 255), width=3)
        # la imagen cuadrada ocupa el ancho; el resto es relleno que da la forma de la caja
        s = W - 24
        im.paste(src.resize((s, s), Image.LANCZOS), (12, 18 if H > W else (H - s) // 2))
        d.rectangle((12, H - 34, W - 13, H - 14), fill=(20, 20, 20))
        img = f'./images/{i:02d}.png'
        im.save(f'{out}/images/{i:02d}.png')
        xml.append(f'  <game><path>./{fn}</path><name>{t}</name><desc>Texto de prueba para ver la cuadricula con caratulas de distinto formato.</desc>'
                   f'<image>{img}</image><thumbnail>{img}</thumbnail><releasedate>199{i}0101T000000</releasedate>'
                   f'<developer>Prueba</developer><publisher>Prueba</publisher><genre>Accion</genre><players>1</players></game>')
    xml.append('</gameList>')
    open(f'{out}/gamelist.xml', 'w', encoding='utf-8').write('\n'.join(xml))
    print(out, size, len(titles))


build('C:/tmp/zt/fakemd', 'md', names, (210, 280), [(30, 60, 140), (140, 40, 40), (30, 110, 70), (110, 70, 20)])
build('C:/tmp/zt/fakesnes', 'sfc', snes, (280, 200), [(70, 40, 120), (30, 100, 130), (130, 100, 30), (100, 30, 60)])
