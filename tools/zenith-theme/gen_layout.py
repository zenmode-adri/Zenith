"""Genera las variables de geometria del tema Zenith para las resoluciones habituales de las consolas compatibles:
  1:1  720x720 (R36 Ultra)      4:3  640x480 (R36S y clones, RG351V...)
  3:2  480x320 (RG351P/OGA...)  16:9 854x480 (RGB10 Max 2...)
Salida: layout.xml (bloques <variables if=...>) y las pastillas pill-<cubo>.png / pillw-<cubo>.png con medidas exactas.
Uso: python gen_layout.py <carpeta del tema>
"""
import os
import sys
from PIL import Image, ImageDraw

OUT = sys.argv[1]
A = os.path.join(OUT, 'assets')

BUCKETS = {
    '1x1': dict(key='1x1', volW=210, volH=36, volY=38, fVol=16, pitch=44, W=720, H=720, cond="${screen.ratio} == '1/1'",
                M=22, D=32, sep=10, fClock=19, fBatt=16, fHelp=16.6, helpCY=684, T=150, carY=0.47, Y0=94, Y0L=94,
                gapX=24, gapY=3, cols=4, rows=3, gp=6, fGrid=13, fTitle=21.6, fSys=15.8, fList=21.6, 
                selH=36, listW=0.52, fName=18.7, fMeta=14.4, fDesc=14.4, coverCap=245, nameGap=14, nameH=25, gapRow=14,
                metaPitch=29, descGap=14, desc=1, gapCol=36, labelW=115, fMenuTitle=24.5, fMenu=21.6,
                fMenuSmall=17.3, fMenuFoot=15.8, fLogo=21.6, hm=16),
    '4x3': dict(key='4x3', volW=222, volH=38, volY=32, fVol=14, pitch=38, W=640, H=480, cond="${screen.ratio} == '4/3'",
                M=18, D=28, sep=8, fClock=16, fBatt=13, fHelp=12.2, helpCY=455, T=125, carY=0.47, Y0=70, Y0L=70,
                gapX=18, gapY=2, cols=4, rows=3, gp=5, fGrid=11.5, fTitle=18, fSys=13.5, fList=17, 
                selH=30, listW=0.52, fName=15.5, fMeta=12.5, fDesc=12.5, coverCap=170, nameGap=10, nameH=21, gapRow=10,
                metaPitch=21, descGap=10, desc=1, gapCol=24, labelW=80, fMenuTitle=20, fMenu=18,
                fMenuSmall=14.5, fMenuFoot=13, fLogo=18, hm=13),
    '3x2': dict(key='3x2', volW=180, volH=32, volY=56, fVol=12, pitch=32, W=480, H=320, cond="${screen.ratio} == '3/2'",
                M=14, D=24, sep=6, fClock=13, fBatt=11, fHelp=10.2, helpCY=300, T=96, carY=0.46, Y0=52, Y0L=52,
                gapX=12, gapY=2, cols=4, rows=2, gp=4, fGrid=10.5, fTitle=15, fSys=11.5, fList=14.5, 
                selH=26, listW=0.58, fName=13, fMeta=11, fDesc=11, coverCap=115, nameGap=8, nameH=18, gapRow=8,
                metaPitch=17, descGap=0, desc=0, gapCol=12, labelW=70, fMenuTitle=17, fMenu=15,
                fMenuSmall=12, fMenuFoot=11, fLogo=14, hm=10),
    '16x9': dict(key='16x9', volW=222, volH=38, volY=34, fVol=14, pitch=38, W=854, H=480, cond="${screen.ratio} == '16/9'",
                 M=20, D=28, sep=10, fClock=16, fBatt=13, fHelp=13, helpCY=455, T=125, carY=0.47, Y0=70, Y0L=70,
                 gapX=24, gapY=2, cols=5, rows=3, gp=5, fGrid=11.5, fTitle=18, fSys=13.5, fList=18.5, 
                 selH=30, listW=0.45, fName=15.5, fMeta=12.5, fDesc=12.5, coverCap=170, nameGap=10, nameH=21, gapRow=10,
                 metaPitch=21, descGap=10, desc=1, gapCol=60, labelW=100, fMenuTitle=20, fMenu=18,
                 fMenuSmall=14.5, fMenuFoot=13, fLogo=18, hm=14),
}
# Relaciones intermedias que ES distingue (nombre de ES, bloque base, ancho a H del bloque base, cambios)
EXTRA = [
    ('16/10', '16x9', 768, dict(listW=0.48, fHelp=10.6, gapCol=40)),
    ('19/12', '16x9', 760, dict(listW=0.48, fHelp=10.6, gapCol=40)),
    ('30/17', '16x9', 847, {}),
    ('21/9', '16x9', 1120, dict(listW=0.37, cols=6, gapCol=60)),
    ('2/1', '16x9', 960, dict(listW=0.40, cols=5, gapCol=60)),
    ('5/4', '4x3', 600, {}),
    ('19/14', '4x3', 651, {}),
]
for _name, _base, _w, _over in EXTRA:
    _b = dict(BUCKETS[_base])
    _b.update(_over)
    _b['W'] = _w
    _b['cond'] = "${screen.ratio} == '%s'" % _name
    BUCKETS['r' + _name.replace('/', 'x')] = _b
DEFAULT = '4x3'   # para relaciones no previstas (16:10, 5:3...)


def f(v, n=5):
    return f'{v:.{n}f}'.rstrip('0').rstrip('.') if '.' in f'{v:.{n}f}' else f'{v:.{n}f}'


def variables(b):
    W, H, M, D = b['W'], b['H'], b['M'], b['D']
    v = {}
    v['mx'] = M / W
    v['avX'] = M / W
    v['avY'] = (M + D / 2) / H
    v['avW'] = D / W
    v['avH'] = D / H
    v['stackH'] = D * 0.75 / H
    v['stackY'] = (M + D * 0.125) / H
    v['stackX'] = -M / W
    v['stackSep'] = b['sep'] / W
    v['fClock'] = b['fClock'] / H
    v['fBatt'] = b['fBatt'] / H
    v['fHelp'] = b['fHelp'] / H
    v['helpY'] = b['helpCY'] / H
    T = b['T']
    v['cellW'] = T / W
    v['cellH'] = T / H
    cpitch = T * 1.147
    v['carW'] = 5 * cpitch / W
    v['carY'] = b['carY']
    v['sysInfoY'] = (b['carY'] * H + T * 1.23 / 2 + 56 * T / 150) / H
    v['fSys'] = b['fSys'] / H
    v['fLogo'] = b['fLogo'] / H
    helpTop = b['helpCY'] - 24 * (H / 720) - 4
    v['gridX'] = M / W
    v['gridY'] = b['Y0'] / H
    v['gridW'] = (W - 2 * M) / W
    v['gridH'] = (helpTop - b['Y0']) / H
    v['gridCols'] = b['cols']
    v['gridRows'] = b['rows']
    v['gridMgX'] = b['gapX'] / W
    v['gridMgY'] = b['gapY'] / H
    v['gridPad'] = b['gp']
    v['fGrid'] = b['fGrid'] / H
    v['nameX'] = (M + D + 12 * (H / 720 if H < 700 else 1)) / W
    v['fTitle'] = b['fTitle'] / H
    listW_px = round(b['listW'] * W)
    v['listX'] = M / W
    v['listY'] = b['Y0L'] / H
    v['listW'] = listW_px / W
    pitch = b['pitch']
    rows_fit = max(1, int((helpTop - b['Y0L']) // pitch))
    v['listH'] = (rows_fit * pitch) / H
    v['listLines'] = rows_fit
    v['listWide'] = (W - 2 * M) / W
    v['fList'] = b['fList'] / H
    k = {'1x1': 23.2, '4x3': 24.9, '3x2': 21.0, '16x9': 24.9}[b['key']]
    pitch = b['pitch']
    v['spacing'] = round(pitch / k, 3)
    v['selH'] = b['selH'] / H
    v['listHM'] = b['hm'] / W
    colX = M + listW_px + b['gapCol']
    colW = W - M - colX
    C = min(colW, b['coverCap'])
    v['colX'] = colX / W
    v['colW'] = colW / W
    v['coverW'] = C / W
    v['coverH'] = C / H
    nameY = b['Y0L'] + C + b['nameGap']
    v['nameY'] = nameY / H
    v['nameH'] = b['nameH'] / H
    v['fName'] = b['fName'] / H
    meta1 = nameY + b['nameH'] + b['gapRow']
    v['meta1'] = meta1 / H
    v['meta2'] = (meta1 + b['metaPitch']) / H
    v['meta3'] = (meta1 + 2 * b['metaPitch']) / H
    v['meta4'] = (meta1 + 3 * b['metaPitch']) / H
    v['metaH'] = (b['metaPitch'] - 3) / H
    v['starH'] = (b['fMeta'] * 1.4) / H
    v['starY'] = (meta1 + 3 * b['metaPitch'] + b['fMeta'] * 0.2) / H
    v['valX'] = (colX + b['labelW']) / W
    v['valW'] = (colW - b['labelW']) / W
    v['lblW'] = b['labelW'] / W
    v['fMeta'] = b['fMeta'] / H
    descY = meta1 + 4 * b['metaPitch'] + b['gapRow']
    v['descY'] = descY / H
    # paso de linea real de ES (medido en la consola; ES redondea la fuente): 22 px con fuente 14.4 (720x720), 21 px con 12.5 (pantallas de 480 de alto)
    line = 22.0 if b['fDesc'] > 13 else 21.0
    n = max(1, int((helpTop - descY) // line))
    v["descH"] = (n * line) / H
    v['fDesc'] = b['fDesc'] / H
    v['descVisible'] = 'true' if b['desc'] else 'false'
    v['volW'] = b['volW'] / W
    v['volH'] = b['volH'] / H
    v['volY'] = b['volY'] / H
    v['fVol'] = b['fVol'] / H
    v['fMenuTitle'] = b['fMenuTitle'] / H
    v['fMenu'] = b['fMenu'] / H
    v['fMenuSmall'] = b['fMenuSmall'] / H
    v['fMenuFoot'] = b['fMenuFoot'] / H
    return v


def fmt(x):
    if isinstance(x, str):
        return x
    if isinstance(x, int):
        return str(x)
    return f'{x:.5f}'.rstrip('0').rstrip('.')


def pill(name, w, h):
    S = 4
    im = Image.new('RGBA', (w * S, h * S), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle((0, 0, w * S - 1, h * S - 1), (h // 2) * S, fill=(214, 242, 248, 255))
    im.resize((w, h), Image.LANCZOS).save(os.path.join(A, name))


lines = ['<?xml version="1.0" encoding="UTF-8"?>',
         '<!-- GENERADO por gen_layout.py: geometria por relacion de aspecto (no editar a mano) -->',
         '<theme>', '  <formatVersion>7</formatVersion>']


def block(key, b, cond=None):
    v = variables(b)
    v['pill'] = f'./assets/pill-{key}.png'
    v['pillw'] = f'./assets/pillw-{key}.png'
    open_tag = f'  <variables if="{cond}">' if cond else '  <variables>'
    lines.append(f'  <!-- {key}: {b["W"]}x{b["H"]} -->')
    lines.append(open_tag)
    for k, val in v.items():
        lines.append(f'    <{k}>{fmt(val)}</{k}>')
    lines.append('  </variables>')


block(DEFAULT, BUCKETS[DEFAULT])            # valores por defecto (relaciones no previstas)
for key, b in BUCKETS.items():
    if key != DEFAULT:
        block(key, b, b['cond'])
    W, H = b['W'], b['H']
    pill(f'pill-{key}.png', round(b['listW'] * W), b['selH'])
    pill(f'pillw-{key}.png', W - 2 * b['M'], b['selH'])
lines.append('</theme>')
open(os.path.join(OUT, 'layout.xml'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
print('layout.xml generado;', len(lines), 'lineas')
