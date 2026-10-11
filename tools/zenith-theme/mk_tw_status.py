"""Fondo translucido para el grupo de estado (reloj, wifi, bateria) de Techdweeb.
El ancho del grupo cambia con la proporcion (separadores y fuente de cada hoja de aspecto), asi que hay una pildora por
proporcion con la relacion de aspecto exacta (extremos circulares, sin deformar) y un borde derecho medido por proporcion.
Medidas tomadas sobre capturas reales con cuadricula (px a 480 de alto): grupo 167 / 206 / 200 / 218 / 206.
Salida: zenith-statusbg-<cubo>.png y zenith-status.xml."""
import os
import sys
from PIL import Image, ImageDraw

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
H_PX = 60
PILL_H480 = 0.066 * 480      # alto de la pildora a 480 de alto

# (proporcion, cubo, ancho de pantalla a 480 de alto, ancho del grupo a 480, borde derecho de la pildora como fraccion de W)
RATIOS = [
    ('1/1', '1x1', 480, 167.0, 0.9701),
    ('4/3', '4x3', 640, 206.0, 0.9758),
    ('3/2', '3x2', 720, 200.0, 0.9733),
    ('16/10', '16x10', 768, 218.0, 0.9814),
    ('16/9', '16x9', 853, 206.0, 0.9804),
]
PAD = 7.0     # relleno a cada lado del grupo (a 480 de alto)

lines = ['<theme>', '  <formatVersion>7</formatVersion>',
         '  <!-- Zenith: translucent dark pill behind the clock/wifi/battery group so it stays readable over light covers.',
         '       One pill image per aspect ratio (exact aspect, so the ends stay round); right edge and width measured per ratio. -->']
for r, tag, w480, g480, right in RATIOS:
    pill_w480 = g480 + 2 * PAD
    wpng = round(H_PX * pill_w480 / PILL_H480)
    S = 4
    im = Image.new('RGBA', (wpng * S, H_PX * S), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle((0, 0, wpng * S - 1, H_PX * S - 1), radius=H_PX * S // 2, fill=(0, 0, 0, 150))
    im.resize((wpng, H_PX), Image.LANCZOS).save(os.path.join(OUT, 'zenith-statusbg-%s.png' % tag))
    lines.append('  <variables if="${screen.ratio} == \'%s\'">' % r)
    lines.append('    <zsbX>%.5f</zsbX>' % right)
    lines.append('    <zsbW>%.5f</zsbW>' % (pill_w480 / w480))
    lines.append('    <zsbImg>./zenith-statusbg-%s.png</zsbImg>' % tag)
    lines.append('  </variables>')
lines += ['  <view name="system,basic,detailed,gamecarousel">',
          '    <image name="zenith-status-bg" extra="true">',
          '      <path>${zsbImg}</path>',
          '      <origin>1 0.5</origin>',
          '      <pos>${zsbX} 0.0690</pos>',
          '      <size>${zsbW} 0.066</size>',
          '      <zIndex>5</zIndex>',
          '    </image>',
          '  </view>',
          '</theme>']
open(os.path.join(OUT, 'zenith-status.xml'), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
print('ok', sorted(os.listdir(OUT)))
