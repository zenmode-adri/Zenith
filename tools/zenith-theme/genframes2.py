"""Fotogramas de Zenith con malla de maquetacion: bloques apilados con huecos fijos, grupo centrado en el area util.
Salida: PNG + BGRA por resolucion, y (con --debug) una version con la malla dibujada (margenes, bloques y huecos en px).
Uso: python genframes2.py <carpeta-salida> [--debug]
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1]
DEBUG = '--debug' in sys.argv
os.makedirs(OUT, exist_ok=True)
RUBIK = 'C:/Users/epsil/Documents/r36-kernel-mainline/SESION-2026-10-04-OS-DEFINITIVO/temas-zenith/zenith-switch-claro/assets/fonts/Rubik-Regular.ttf'
CJK = 'C:/Windows/Fonts/msyh.ttc'
S = 3
# Paleta oscura (la misma que el tema Zenith oscuro: bg 26262A, txt F0F0F2, dim 8C8C94, pista 55555C)
BG = (38, 38, 42)
TXT = (240, 240, 242)
DIM = (140, 140, 148)
CY = (0, 195, 227)
TRACK = (85, 85, 92)
RED = (230, 40, 90)


def F(px, path=RUBIK):
    return ImageFont.truetype(path, int(px * S))


class Frame:
    def __init__(self, W, H):
        self.W, self.H = W, H
        self.im = Image.new('RGB', (W * S, H * S), BG)
        self.d = ImageDraw.Draw(self.im)
        self.blocks = []      # (nombre, y0, y1)
        self.sf = 1.0 if H >= 700 else 0.82     # escala de huecos en pantallas bajas
        self.M = round(H * 0.06)                # margen superior/inferior (6 % de la altura)

    def gap(self, v):
        return round(v * self.sf)

    def text(self, x, y, s, font, fill, anchor='mm'):
        self.d.text((x * S, y * S), s, font=font, fill=fill, anchor=anchor)

    def disc(self, cx, cy, r, fill):
        self.d.ellipse(((cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S), fill=fill)

    def pill(self, x0, y0, x1, y1, fill):
        self.d.rounded_rectangle((x0 * S, y0 * S, x1 * S, y1 * S), (y1 - y0) / 2 * S, fill=fill)

    def mark(self, name, y0, y1):
        self.blocks.append((name, y0, y1))

    def save(self, name):
        im = self.im.resize((self.W, self.H), Image.LANCZOS)
        base = os.path.join(OUT, f'{name}-{self.W}x{self.H}')
        im.save(base + '.png')
        r, g, b = im.split()
        open(base + '.bgra', 'wb').write(Image.merge('RGBA', (b, g, r, Image.new('L', im.size, 255))).tobytes())
        if DEBUG:
            self.debug(im).save(base + '-malla.png')
        return im

    def debug(self, im):
        W, H = self.W, self.H
        o = im.copy().convert('RGB')
        d = ImageDraw.Draw(o, 'RGBA')
        for y in range(0, H, 8):
            d.line((0, y, W, y), fill=(0, 120, 255, 20 if y % 64 else 55))
        for y in (self.M, H - self.M):
            d.line((0, y, W, y), fill=(0, 160, 90, 190), width=1)
        d.line((W // 2, 0, W // 2, H), fill=(0, 120, 255, 70))
        d.line((0, H // 2, W, H // 2), fill=(0, 120, 255, 70))
        f9 = ImageFont.truetype('C:/Windows/Fonts/consola.ttf', 11)
        prev = None
        for name, y0, y1 in sorted(self.blocks, key=lambda b: b[1]):
            d.line((0, y0, W, y0), fill=RED + (150,))
            d.line((0, y1, W, y1), fill=RED + (150,))
            d.text((6, y0 + 1), f'{name} {y1 - y0}px', font=f9, fill=RED)
            if prev is not None:
                d.text((W - 8, (prev + y0) / 2), f'hueco {y0 - prev}', font=f9, fill=(0, 100, 200), anchor='rm')
            prev = y1
        d.text((6, self.M + 2), f'margen {self.M}', font=f9, fill=(0, 140, 80))
        d.text((6, H - self.M + 2), f'margen {self.M}', font=f9, fill=(0, 140, 80))
        return o


# ---------------- bloques ----------------
def lockup_metrics(f, compact):
    fs = 38 if not compact else 26
    font = F(fs)
    x0, y0, x1, y1 = f.d.textbbox((0, 0), 'E', font=font)
    cap = (y1 - y0) / S
    D = cap * 1.8
    return fs, font, (y0 + y1) / 2, D


def b_lockup(f, top, compact=False, sig=True):
    """Disco cian con la Z como primera letra de ZENITH (todo alineado a la altura de versal). Devuelve la altura."""
    fs, font, capc, D = lockup_metrics(f, compact)
    W = f.W
    r = D / 2
    cy = top + r
    sp = fs * 0.36
    zb = f.d.textbbox((0, 0), 'Z', font=font)
    zw = (zb[2] - zb[0]) / S
    gap = max(1.0, sp + 2 - (r - zw / 2))
    letters = 'ENITH'
    lw = [f.d.textlength(c, font=font) / S for c in letters]
    total = D + gap + sum(lw) + sp * (len(letters) - 1)
    x = W / 2 - total / 2
    f.disc(x + r, cy, r, CY)
    f.d.text(((x + r) * S - (zb[0] + zb[2]) / 2, cy * S - (zb[1] + zb[3]) / 2), 'Z', font=font, fill=(255, 255, 255))
    x += D + gap
    for c, w in zip(letters, lw):
        f.d.text((x * S, cy * S - capc), c, font=font, fill=TXT)
        x += w + sp
    f.mark('logo', round(top), round(top + D))
    return D


def b_sig(f, top):
    f.text(f.W / 2, top + 9, 'by zenmode-adri', F(15), DIM)
    f.mark('firma', round(top), round(top + 18))
    return 18


def b_bar(f, top, pct):
    bx = f.W * 0.236
    bw = f.W - 2 * bx
    f.pill(bx, top, bx + bw, top + 6, TRACK)
    if pct > 0:
        f.pill(bx, top, bx + max(6, bw * pct), top + 6, CY)
    f.mark('barra', round(top), round(top + 6))
    return 6


def b_steps(f, top, step):
    bx = f.W * 0.236
    bw = f.W - 2 * bx
    for i, lab in enumerate(['Prepare', 'Install', 'Settings']):
        x = bx + bw / 3 * (i + 0.5)
        done = step >= 3 or i < step
        cur = i == step
        f.disc(x, top + 9, 5, CY if (done or cur) else TRACK)
        if cur:
            f.d.ellipse(((x - 9) * S, (top + 9 - 9) * S, (x + 9) * S, (top + 9 + 9) * S), outline=CY, width=2 * S)
        f.text(x, top + 9 + 26, lab, F(14), TXT if (done or cur) else DIM)
    f.mark('pasos', round(top), round(top + 42))
    return 42


def b_text(f, top, s, px, fill, h, name, font_path=RUBIK):
    f.text(f.W / 2, top + h / 2, s, F(px, font_path), fill)
    f.mark(name, round(top), round(top + h))
    return h


def footer(f):
    """Pie anclado al margen inferior: dos lineas de creditos."""
    H, M = f.H, f.M
    top = H - M - 38
    f.text(f.W / 2, top + 9, 'Thanks to everyone who builds and shares open source for these handhelds', F(13), DIM)
    f.text(f.W / 2, top + 29, 'Built on ROCKNIX  -  AURKNIX by lcdyk  -  AveyondFly', F(13), DIM)
    f.mark('pie', top, top + 38)
    return top


def center_group(f, total, foot_top, min_gap=36, lift=0.0):
    """Y inicial del grupo: centrado entre el margen superior y (pie - hueco minimo)."""
    area0 = f.M
    area1 = foot_top - f.gap(min_gap)
    return round(area0 + (area1 - area0 - total) / 2 - lift * f.H)


# ---------------- pantallas ----------------
def screen_boot(W, H):
    f = Frame(W, H)
    D = lockup_metrics(f, False)[3]
    total = D + f.gap(16) + 18
    top = round(H * 0.46 - total / 2)
    y = top
    y += b_lockup(f, y) + f.gap(16)
    b_sig(f, y)
    f.save('zenith-boot')


def screen_progress(name, W, H, step, pct, status):
    f = Frame(W, H)
    foot = footer(f)
    D = lockup_metrics(f, False)[3]
    g = f.gap
    gaps = [16, 64, 22, 36, 16]
    blocks_h = [D, 18, 6, 42, 22] + ([18] if pct < 1.0 else [])
    n = len(blocks_h)
    total = sum(blocks_h) + sum(g(v) for v in gaps[:n - 1])
    y = center_group(f, total, foot)
    y += b_lockup(f, y) + g(gaps[0])
    y += b_sig(f, y) + g(gaps[1])
    y += b_bar(f, y, pct) + g(gaps[2])
    y += b_steps(f, y, step) + g(gaps[3])
    y += b_text(f, y, status, 17, TXT, 22, 'estado')
    if pct < 1.0:
        y += g(gaps[4])
        b_text(f, y, 'Please do not turn off your console', 14, DIM, 18, 'aviso')
    f.save(name)


def screen_lang(W, H):
    f = Frame(W, H)
    foot = footer(f)
    g = f.gap
    D = lockup_metrics(f, True)[3]
    row = 52 if H >= 700 else 44      # paso entre opciones
    opts_h = row * 2 + 38
    gaps = [44, 8, 34, 34]
    total = D + g(gaps[0]) + 28 + g(gaps[1]) + 22 + g(gaps[2]) + opts_h + g(gaps[3]) + 18
    y = center_group(f, total, foot)
    y += b_lockup(f, y, compact=True) + g(gaps[0])
    y += b_text(f, y, 'Choose your language', 22, TXT, 28, 'titulo') + g(gaps[1])
    y += b_text(f, y, '\u9009\u62e9\u8bed\u8a00  -  Elige tu idioma', 18, DIM, 22, 'subtitulo', CJK) + g(gaps[2])
    opts = [('A', 'English', RUBIK), ('B', '\u4e2d\u6587', CJK), ('X', 'Espa\u00f1ol', RUBIK)]
    r = 19
    maxw = max(f.d.textlength(o[1], font=F(26, o[2])) / S for o in opts)
    left = (W - (2 * r + 18 + maxw)) / 2
    top = y
    for i, (k, label, fp) in enumerate(opts):
        cy = top + 19 + i * row
        f.disc(left + r, cy, r, CY)
        f.text(left + r, cy + 1, k, F(19), (255, 255, 255))
        f.text(left + 2 * r + 18, cy, label, F(24 if fp == CJK else 26, fp), TXT, anchor='lm')
    f.mark('opciones', round(top), round(top + opts_h))
    y += opts_h + g(gaps[3])
    b_text(f, y, 'Press the button / \u6309\u952e / Pulsa el bot\u00f3n', 14, DIM, 18, 'aviso', CJK)
    f.save('zenith-lang')


for W, H in ((640, 480), (720, 720)):
    screen_boot(W, H)
    screen_progress('zenith-resize', W, H, 0, 0.08, 'Preparing the memory card...')
    screen_progress('zenith-0-prepare', W, H, 0, 0.25, 'Preparing your console...')
    screen_progress('zenith-1-install', W, H, 1, 0.55, 'Installing games support...')
    screen_progress('zenith-2-apply', W, H, 2, 0.85, 'Applying your settings...')
    screen_progress('zenith-3-done', W, H, 3, 1.0, 'All set - starting')
    screen_lang(W, H)
Image.open(os.path.join(OUT, 'zenith-boot-720x720.png')).save(os.path.join(OUT, 'zenith-splash.png'))
print('ok')
