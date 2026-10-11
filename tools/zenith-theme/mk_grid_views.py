"""Vistas de cuadricula (view "grid") para Techdweeb y Art Book Next, disenadas sobre malla.

MALLA (px a 480 de alto, que es la unidad base de las dos hojas de aspecto de estos temas; todo se pasa a fracciones):
  margen M = 20 a izquierda y derecha (el estado del tema ya queda a 20 px del borde derecho)
  cabecera (la tinta del logo acaba en 73 en Techdweeb y en 87 en Art Book) | hueco ~10-15 | cuadricula Y0..GB (3 filas, hueco 8)
  | hueco 10 | banda del nombre 24 px | pie (Techdweeb: margen de 20; Art Book: banda negra de ayuda desde 425)
  ancho de la cuadricula = area util exacta (W480 - 2M): borde izquierdo alineado con el logo, derecho con el estado
  columnas C = las minimas con las que un tile cuadrado cabe en 3 filas (tiles lo mas grandes posible)
  el bloque se centra en vertical dentro de la banda sobrante
Salida: <out>/zenith-grid.xml y <out>/zenith-gridframe.png (marco con esquinas fijas, nunca se estira una pildora).
Uso: python mk_grid_views.py <techdweeb|artbook> <carpeta-salida>
"""
import math
import os
import sys

from PIL import Image, ImageDraw

THEME, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)
H = 480.0
M = 20.0
GAP = 8.0
ROWS = 3

# ancho de pantalla a 480 de alto y ancho maximo del logo (px a 480) que usa cada tema en sus hojas de aspecto
RATIOS = [
    # ratio, W480, logo_w (techdweeb), logo_w (artbook)
    ('1/1', 480.0, 194.0, 150.0),
    ('4/3', 640.0, 224.0, 230.0),
    ('3/2', 720.0, 224.0, 230.0),
    ('16/10', 768.0, 224.0, 210.0),
    ('16/9', 853.0, 224.0, 230.0),
]

CFG = {
    'techdweeb': dict(Y0=88.0, GB=426.0, NB0=436.0, NB1=460.0, logoY=48.0, logoH=60.0, name_px=14.0,
                      frame=(255, 115, 0), frame_px=4, pad=6,
                      font='${metadataFont}', name_color='${gamelistTextlistPrimaryColor}',
                      tile_bg='171717FF', tile_img='B8B8B8FF', sel_bg='0A0A0AFF',
                      logo_color='${systemLogoColor}', logo_path='./_inc/systems/logos/${system.theme}.png',
                      bg_view=False),
    'artbook': dict(Y0=100.0, GB=385.0, NB0=395.0, NB1=419.0, logoY=62.5, logoH=50.0, name_px=16.0,
                    frame=(255, 255, 255), frame_px=3, pad=5,
                    font='${fontBold}', name_color='${gamelistListTextlistSelectedColor}',
                    tile_bg='111111FF', tile_img='9A9A9AFF', sel_bg='000000FF',
                    logo_color='${gamelistListLogoColor}', logo_path='./_inc/systems/logos/${system.theme}.svg',
                    bg_view=True),
}[THEME]

# marco del tile seleccionado: PNG 32x32, borde de frame_px, esquinas cuadradas, centro transparente
FP = 32
fr = Image.new('RGBA', (FP, FP), (0, 0, 0, 0))
d = ImageDraw.Draw(fr)
d.rectangle((0, 0, FP - 1, FP - 1), fill=CFG['frame'] + (255,))
d.rectangle((CFG['frame_px'], CFG['frame_px'], FP - 1 - CFG['frame_px'], FP - 1 - CFG['frame_px']), fill=(0, 0, 0, 0))
fr.save(os.path.join(OUT, 'zenith-gridframe.png'))

Hg = CFG['GB'] - CFG['Y0']
t_max = (Hg - GAP * (ROWS - 1)) / ROWS
lines = ['<theme>', '  <formatVersion>7</formatVersion>',
         '  <!-- Zenith: grid view designed on a grid (px at 480 high): margin 20, header 0-90, grid %d-%d with %d rows and gap 8, name band %d-%d.'
         % (CFG['Y0'], CFG['GB'], ROWS, CFG['NB0'], CFG['NB1']),
         '       Grid width = usable width (aligned with the logo and the status indicators); columns = the fewest that keep square tiles within 3 rows. -->']
report = []
for ratio, W, lw_tw, lw_ab in RATIOS:
    Wg = W - 2 * M
    C = math.ceil((Wg + GAP) / (t_max + GAP))
    t = (Wg - GAP * (C - 1)) / C
    blockH = ROWS * t + GAP * (ROWS - 1)
    y0 = CFG['Y0'] + (Hg - blockH) / 2
    lw = lw_tw if THEME == 'techdweeb' else lw_ab
    name_cy = (CFG['NB0'] + CFG['NB1']) / 2
    lines.append('  <variables if="${screen.ratio} == \'%s\'">' % ratio)
    for k, v in (('zgX', M / W), ('zgY', y0 / H), ('zgW', Wg / W), ('zgH', blockH / H), ('zgGX', GAP / W), ('zgGY', GAP / H),
                 ('zgLogoW', lw / W), ('zgNameCY', name_cy / H), ('zgNameW', Wg / W)):
        lines.append('    <%s>%.5f</%s>' % (k, v, k))
    lines.append('    <zgCols>%d</zgCols>' % C)
    lines.append('  </variables>')
    report.append((ratio, W, C, round(t, 1), round(blockH, 1), round(y0, 1)))

bgxml = ''
if CFG['bg_view']:
    bgxml = ('    <image name="background" extra="true">\n'
             '      <color>${gamelistListBackgroundColor}</color>\n'
             '    </image>\n')
pad = CFG['pad']
lines += [
    '  <view name="grid">',
    bgxml.rstrip('\n') if bgxml else '    <!-- background: shared gamelist-background of the theme -->',
    '    <image name="logo">',
    '      <origin>0 0.5</origin>',
    '      <pos>${zgX} %.6f</pos>' % (CFG['logoY'] / H),
    '      <maxSize>${zgLogoW} %.6f</maxSize>' % (CFG['logoH'] / H),
    '      <path>%s</path>' % CFG['logo_path'],
    '      <color>%s</color>' % CFG['logo_color'],
    '      <zIndex>5</zIndex>',
    '    </image>',
    '    <text name="logoText">',
    '      <visible>false</visible>',
    '    </text>',
    '    <imagegrid name="gamegrid">',
    '      <pos>${zgX} ${zgY}</pos>',
    '      <size>${zgW} ${zgH}</size>',
    '      <cellProportion>1</cellProportion>',
    '      <margin>${zgGX} ${zgGY}</margin>',
    '      <autoLayout>${zgCols} %d</autoLayout>' % ROWS,
    '      <autoLayoutSelectedZoom>1</autoLayoutSelectedZoom>',
    '      <imageSource>thumbnail</imageSource>',
    '      <animateSelection>false</animateSelection>',
    '      <centerSelection>false</centerSelection>',
    '      <scrollSound>./assets/sounds/scroll.wav</scrollSound>' if False else '',
    '      <zIndex>20</zIndex>',
    '    </imagegrid>',
    '    <gridtile name="default">',
    '      <backgroundColor>%s</backgroundColor>' % CFG['tile_bg'],
    '      <selectionMode>image</selectionMode>',
    '      <imageSizeMode>maxSize</imageSizeMode>',
    '      <padding>%d %d</padding>' % (pad, pad),
    '      <imageColor>%s</imageColor>' % CFG['tile_img'],
    '    </gridtile>',
    '    <gridtile name="selected">',
    '      <backgroundImage>./zenith-gridframe.png</backgroundImage>',
    '      <backgroundCornerSize>8 8</backgroundCornerSize>',
    '      <backgroundColor>FFFFFFFF</backgroundColor>',
    '      <selectionMode>image</selectionMode>',
    '      <imageSizeMode>maxSize</imageSizeMode>',
    '      <padding>%d %d</padding>' % (pad, pad),
    '      <imageColor>FFFFFFFF</imageColor>',
    '    </gridtile>',
    '    <text name="gridtile">',
    '      <visible>false</visible>',
    '    </text>',
    '    <text name="md_name">',
    '      <origin>0 0.5</origin>',
    '      <pos>${zgX} ${zgNameCY}</pos>',
    '      <size>${zgNameW} %.6f</size>' % ((CFG['NB1'] - CFG['NB0']) / H),
    '      <fontPath>%s</fontPath>' % CFG['font'],
    '      <fontSize>%.6f</fontSize>' % (CFG['name_px'] / H),
    '      <color>%s</color>' % CFG['name_color'],
    '      <alignment>left</alignment>',
    '      <lineSpacing>1</lineSpacing>',
    '      <zIndex>30</zIndex>',
    '    </text>',
    '  </view>',
    '</theme>',
]
open(os.path.join(OUT, 'zenith-grid.xml'), 'w', encoding='utf-8', newline='\n').write('\n'.join(l for l in lines if l != '') + '\n')
print('ratio  W480  cols  tile  blockH  y0')
for r in report:
    print(*r)
