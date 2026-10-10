"""Segunda tanda de assets del tema Zenith Switch: iconos de ayuda, pastilla del selector y logos de utilidades."""
import math
import os
import sys
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1]
A = os.path.join(OUT, 'assets')
LG = os.path.join(A, 'logos')
S = 4
CY = (0, 195, 227)
CYD = (0, 134, 159)
TXT = (45, 45, 50)
FONT = os.path.join(A, 'fonts', 'Rubik-Regular.ttf')


def canvas(w, h):
    return Image.new('RGBA', (w * S, h * S), (0, 0, 0, 0))


def save(im, w, h, path, **kw):
    im.resize((w, h), Image.LANCZOS).save(path, **kw)


W = (255, 255, 255, 255)

# ---------- iconos de ayuda (blanco, se tiñen con iconColor) ----------
def help_icon(name, draw_fn, w=96, h=96):
    im = canvas(w, h)
    draw_fn(ImageDraw.Draw(im))
    save(im, w, h, os.path.join(A, f'help-{name}.png'))


def ring(d, w=96, lw=6):
    d.ellipse((6 * S, 6 * S, (w - 6) * S, (w - 6) * S), outline=W, width=lw * S)


def glyph_minus(d):
    ring(d)
    d.rounded_rectangle((28 * S, 45 * S, 68 * S, 51 * S), 3 * S, fill=W)


def glyph_plus(d):
    ring(d)
    d.rounded_rectangle((28 * S, 45 * S, 68 * S, 51 * S), 3 * S, fill=W)
    d.rounded_rectangle((45 * S, 28 * S, 51 * S, 68 * S), 3 * S, fill=W)


def pill_letter(letter):
    def f(d):
        d.rounded_rectangle((6 * S, 20 * S, 90 * S, 76 * S), 22 * S, outline=W, width=6 * S)
        d.text((48 * S, 49 * S), letter, font=ImageFont.truetype('C:/Windows/Fonts/segoeuib.ttf', 40 * S), fill=W, anchor='mm')
    return f


def dpad(d, up=True, down=True, left=True, right=True):
    d.ellipse((6 * S, 6 * S, 90 * S, 90 * S), outline=W, width=6 * S)
    arm = 7 * S
    if up or down:
        d.rounded_rectangle((48 * S - arm / 2, 26 * S if up else 40 * S, 48 * S + arm / 2, 70 * S if down else 56 * S), 3 * S, fill=W)
    if left or right:
        d.rounded_rectangle((26 * S if left else 40 * S, 48 * S - arm / 2, 70 * S if right else 56 * S, 48 * S + arm / 2), 3 * S, fill=W)


help_icon('select', glyph_minus)
help_icon('start', glyph_plus)
help_icon('L', pill_letter('L'))
help_icon('R', pill_letter('R'))
help_icon('updown', lambda d: dpad(d, left=False, right=False))
help_icon('leftright', lambda d: dpad(d, up=False, down=False))
help_icon('dpad', lambda d: dpad(d))

# ---------- pastilla del selector (lista): 374x42, esquinas redondas ----------
w, h = 374, 42
im = canvas(w, h)
ImageDraw.Draw(im).rounded_rectangle((0, 0, w * S - 1, h * S - 1), (h // 2) * S, fill=(214, 242, 248, 255))
save(im, w, h, os.path.join(A, 'pill.png'))

# ---------- logos de utilidades: icono de trazo + etiqueta ----------
LW, LH = 360, 300
SW = 9 * S


def stroke_ic():
    im = canvas(LW, LH)
    return im, ImageDraw.Draw(im)


def gear(d, cx, cy, r):
    pts = []
    n = 8
    for i in range(n * 2):
        a = math.pi * 2 * i / (n * 2) - math.pi / (n * 2)
        rr = r if i % 2 == 0 else r * 0.78
        # dientes trapezoidales: dos puntos por diente
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    teeth = []
    for i in range(n):
        a0 = math.pi * 2 * i / n
        for da, rr in ((-0.20, r * 0.8), (-0.12, r), (0.12, r), (0.20, r * 0.8)):
            teeth.append((cx + rr * math.cos(a0 + da), cy + rr * math.sin(a0 + da)))
    d.line([(x * S, y * S) for x, y in teeth + [teeth[0]]], fill=CYD + (255,), width=SW, joint='curve')
    d.ellipse(((cx - r * 0.34) * S, (cy - r * 0.34) * S, (cx + r * 0.34) * S, (cy + r * 0.34) * S), outline=CYD + (255,), width=SW)


def ic_tools(d):
    gear(d, 180, 105, 62)


def ic_music(d):
    c = CYD + (255,)
    d.ellipse((92 * S, 135 * S, 142 * S, 175 * S), outline=c, width=SW)
    d.ellipse((190 * S, 115 * S, 240 * S, 155 * S), outline=c, width=SW)
    d.line([(140 * S, 152 * S), (140 * S, 55 * S), (238 * S, 35 * S), (238 * S, 135 * S)], fill=c, width=SW, joint='curve')
    d.line([(140 * S, 85 * S), (238 * S, 65 * S)], fill=c, width=SW)


def ic_ebook(d):
    c = CYD + (255,)
    d.line([(180 * S, 55 * S), (180 * S, 165 * S)], fill=c, width=SW)
    d.line([(180 * S, 55 * S), (120 * S, 40 * S), (62 * S, 52 * S), (62 * S, 158 * S), (120 * S, 146 * S), (180 * S, 165 * S)], fill=c, width=SW, joint='curve')
    d.line([(180 * S, 55 * S), (240 * S, 40 * S), (298 * S, 52 * S), (298 * S, 158 * S), (240 * S, 146 * S), (180 * S, 165 * S)], fill=c, width=SW, joint='curve')


def ic_star(d):
    c = CYD + (255,)
    cx, cy, R, r = 180, 108, 70, 30
    pts = []
    for i in range(10):
        a = -math.pi / 2 + math.pi * i / 5
        rr = R if i % 2 == 0 else r
        pts.append(((cx + rr * math.cos(a)) * S, (cy + rr * math.sin(a)) * S))
    d.line(pts + [pts[0]], fill=c, width=SW, joint='curve')


def ic_grid(d):
    c = CYD + (255,)
    for i in range(2):
        for j in range(2):
            x = 118 + i * 70
            y = 40 + j * 70
            d.rounded_rectangle((x * S, y * S, (x + 56) * S, (y + 56) * S), 12 * S, outline=c, width=SW)


def ic_clock(d):
    c = CYD + (255,)
    d.ellipse((120 * S, 38 * S, 240 * S, 158 * S), outline=c, width=SW)
    d.line([(180 * S, 98 * S), (180 * S, 64 * S)], fill=c, width=SW)
    d.line([(180 * S, 98 * S), (208 * S, 114 * S)], fill=c, width=SW)


def ic_sparkle(d):
    c = CYD + (255,)
    cx, cy = 180, 100
    pts = []
    for i in range(8):
        a = -math.pi / 2 + math.pi * i / 4
        rr = 66 if i % 2 == 0 else 20
        pts.append(((cx + rr * math.cos(a)) * S, (cy + rr * math.sin(a)) * S))
    d.line(pts + [pts[0]], fill=c, width=SW, joint='curve')


def ic_stack(d):
    c = CYD + (255,)
    d.rounded_rectangle((108 * S, 36 * S, 252 * S, 108 * S), 12 * S, outline=c, width=SW)
    d.line([(124 * S, 128 * S), (236 * S, 128 * S)], fill=c, width=SW)
    d.line([(144 * S, 148 * S), (216 * S, 148 * S)], fill=c, width=SW)


SPECIAL = {
    'tools': ('Tools', ic_tools),
    'music': ('Music', ic_music),
    'ebook': ('Books', ic_ebook),
    'auto-favorites': ('Favorites', ic_star),
    'auto-allgames': ('All games', ic_grid),
    'auto-lastplayed': ('Last played', ic_clock),
    'auto-neverplayed': ('Never played', ic_sparkle),
    'custom-collections': ('Collections', ic_stack),
}
font = ImageFont.truetype(FONT, 60 * S)
for key, (label, fn) in SPECIAL.items():
    im, d = stroke_ic()
    fn(d)
    tw = d.textlength(label, font=font) / S
    f = font if tw <= LW - 20 else ImageFont.truetype(FONT, int(60 * (LW - 20) / tw * S))
    d.text((LW * S / 2, 232 * S), label, font=f, fill=TXT + (255,), anchor='mm')
    save(im, LW, LH, os.path.join(LG, key + '.webp'), quality=95, method=6)
print('ok')
