"""Iconos de la barra de ayuda a tamano nativo (ES los pinta a ~15 px de alto): discos rellenos con el simbolo recortado."""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

A = os.path.join(sys.argv[1], 'assets')
S = 16
H = 15
FONT = 'C:/Windows/Fonts/segoeuib.ttf'


def base(w):
    return Image.new('L', (w * S, H * S), 0)


def disc(d, w=H):
    d.ellipse((1.0 * S, 1.0 * S, (w - 1.0) * S, (H - 1.0) * S), fill=255)


def pill(d, w):
    d.rounded_rectangle((1.0 * S, 2.6 * S, (w - 1.0) * S, (H - 2.6) * S), 3.2 * S, fill=255)


def letter(d, w, ch, size, fill=0):
    """Centra la letra por su caja real (no por la linea base), en el centro del icono."""
    f = ImageFont.truetype(FONT, int(size * S))
    x0, y0, x1, y1 = d.textbbox((0, 0), ch, font=f)
    cx, cy = w * S / 2, H * S / 2
    d.text((cx - (x0 + x1) / 2, cy - (y0 + y1) / 2), ch, font=f, fill=fill)


def save(mask, w, name):
    # alfa = mascara (blanco; se tine con iconColor)
    m = mask.resize((w, H), Image.LANCZOS)
    im = Image.new('RGBA', (w, H), (255, 255, 255, 0))
    im.putalpha(m)
    im.save(os.path.join(A, f'help-{name}.png'))


for ch in 'ABXY':
    m = base(H)
    d = ImageDraw.Draw(m)
    disc(d)
    letter(d, H, ch, 9.5)
    save(m, H, ch)

for ch in 'LR':
    w = 21
    m = base(w)
    d = ImageDraw.Draw(m)
    pill(d, w)
    letter(d, w, ch, 8.6)
    save(m, w, ch)

# START y SELECT como en la consola: pastilla con la palabra recortada
def label_pill(text, name):
    f = ImageFont.truetype(FONT, int(7.4 * S))
    tw = base(40)
    d0 = ImageDraw.Draw(tw)
    w = int(d0.textlength(text, font=f) / S + 9)
    m = base(w)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle((1.0 * S, 2.8 * S, (w - 1.0) * S, (H - 2.8) * S), 3.0 * S, fill=255)
    x0, y0, x1, y1 = d.textbbox((0, 0), text, font=f)
    d.text((w * S / 2 - (x0 + x1) / 2, H * S / 2 - (y0 + y1) / 2), text, font=f, fill=0)
    save(m, w, name)


label_pill('START', 'start')
label_pill('SELECT', 'select')


def arrows(up, down, left, right):
    m = base(H)
    d = ImageDraw.Draw(m)
    disc(d)
    c = 7.5 * S
    t = 2.6 * S
    if up:
        d.polygon([(c, 3.2 * S), (c - t, 6.4 * S), (c + t, 6.4 * S)], fill=0)
    if down:
        d.polygon([(c, 11.8 * S), (c - t, 8.6 * S), (c + t, 8.6 * S)], fill=0)
    if left:
        d.polygon([(3.2 * S, c), (6.4 * S, c - t), (6.4 * S, c + t)], fill=0)
    if right:
        d.polygon([(11.8 * S, c), (8.6 * S, c - t), (8.6 * S, c + t)], fill=0)
    return m


save(arrows(1, 1, 0, 0), H, 'updown')
save(arrows(0, 0, 1, 1), H, 'leftright')
save(arrows(1, 1, 1, 1), H, 'dpad')
print('ok')
