"""Superpone medidas (margenes, cajas medidas sobre la captura real y huecos en px) a las capturas del tema."""
import sys
from PIL import Image, ImageDraw, ImageFont

BG = (235, 235, 237)
RED = (235, 40, 90)
BLUE = (0, 110, 230)
GREEN = (0, 150, 80)
F = ImageFont.truetype('C:/Windows/Fonts/consola.ttf', 12)
FB = ImageFont.truetype('C:/Windows/Fonts/consolab.ttf', 12)


def load(name, w, h):
    return Image.open(f'C:/tmp/zt/shots/{name}.png').convert('RGB').crop((0, 0, w, h))


def bbox(im, region, tol=40, bg=BG):
    x0, y0, x1, y1 = region
    xs, ys = [], []
    px = im.load()
    for y in range(max(0, y0), min(im.height, y1)):
        for x in range(max(0, x0), min(im.width, x1)):
            p = px[x, y]
            if abs(p[0] - bg[0]) + abs(p[1] - bg[1]) + abs(p[2] - bg[2]) > tol:
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs), max(ys))


class Ov:
    def __init__(self, im):
        self.im = im.copy()
        self.W, self.H = im.size
        self.d = ImageDraw.Draw(self.im, 'RGBA')

    def grid(self, step=8):
        for y in range(0, self.H, step):
            self.d.line((0, y, self.W, y), fill=(0, 120, 255, 14 if y % 64 else 40))
        for x in range(0, self.W, step):
            self.d.line((x, 0, x, self.H), fill=(0, 120, 255, 14 if x % 64 else 40))

    def box(self, b, color=RED, label=None):
        if b is None:
            return
        x0, y0, x1, y1 = b
        self.d.rectangle((x0, y0, x1, y1), outline=color + (210,))
        if label:
            self.d.text((x0 + 2, y0 - 13 if y0 > 14 else y1 + 2), label, font=F, fill=color)

    def vline(self, x, color=GREEN, label=None):
        self.d.line((x, 0, x, self.H), fill=color + (170,))
        if label:
            self.d.text((x + 3, self.H - 14), label, font=F, fill=color)

    def hline(self, y, color=GREEN, label=None):
        self.d.line((0, y, self.W, y), fill=color + (170,))
        if label:
            self.d.text((3, y + 2), label, font=F, fill=color)

    def hgap(self, y, x0, x1, color=BLUE):
        """cota horizontal entre x0 y x1 con el numero de px"""
        if x1 - x0 < 2:
            return
        self.d.line((x0, y, x1, y), fill=color + (255,))
        self.d.line((x0, y - 3, x0, y + 3), fill=color + (255,))
        self.d.line((x1, y - 3, x1, y + 3), fill=color + (255,))
        t = str(x1 - x0)
        self.d.text(((x0 + x1) / 2 - 3 * len(t), y - 14), t, font=FB, fill=color)

    def vgap(self, x, y0, y1, color=BLUE):
        if y1 - y0 < 2:
            return
        self.d.line((x, y0, x, y1), fill=color + (255,))
        self.d.line((x - 3, y0, x + 3, y0), fill=color + (255,))
        self.d.line((x - 3, y1, x + 3, y1), fill=color + (255,))
        self.d.text((x + 5, (y0 + y1) / 2 - 6), str(y1 - y0), font=FB, fill=color)


def header(ov, im, M):
    W, H = ov.W, ov.H
    left = bbox(im, (0, 0, W // 2, int(H * 0.12)))
    right = bbox(im, (W // 2, 0, W, int(H * 0.12)))
    ov.box(left, RED, 'avatar/titulo')
    ov.box(right, RED, 'estado')
    cy = None
    if left:
        ov.hgap(max(6, left[1] - 8), 0, left[0])
        ov.vgap(left[0] - 8 if left[0] > 14 else left[0] + 2, 0, left[1])
    if right:
        ov.hgap(right[3] + 8, right[2] + 1, W)
        ov.vgap(right[2] + 6 if right[2] + 6 < W - 1 else right[2] - 2, 0, right[1])
    return left, right


def footer(ov, im, y0):
    W, H = ov.W, ov.H
    hb = bbox(im, (0, y0, W, H))
    ov.box(hb, RED, 'ayuda')
    if hb:
        ov.hgap(hb[3] + 8 if hb[3] + 8 < H - 2 else hb[3] - 2, 0, hb[0])
        ov.hgap(hb[3] + 8 if hb[3] + 8 < H - 2 else hb[3] - 2, hb[2] + 1, W)
        ov.vgap(hb[0] + 10, hb[3] + 1, H)
    return hb


def frame_common(name, W, H, M, kind):
    im = load(name, W, H)
    ov = Ov(im)
    ov.grid()
    for x in (M, W - M):
        ov.vline(x, GREEN, f'M{M}')
    for y in (M, H - M):
        ov.hline(y, GREEN, f'M{M}')
    ov.d.line((W // 2, 0, W // 2, H), fill=(0, 120, 255, 60))
    ov.d.line((0, H // 2, W, H // 2), fill=(0, 120, 255, 60))
    return im, ov


def system_view(name, W, H, M, T, helpTop, out):
    im, ov = frame_common(name, W, H, M, 'sys')
    hdr = header(ov, im, M)
    mid = bbox(im, (0, int(H * 0.16), W, int(H * 0.60)), tol=14)
    # tesela seleccionada: marco cian
    cy = None
    px = im.load()
    xs, ys = [], []
    for y in range(int(H * 0.16), int(H * 0.62)):
        for x in range(W):
            p = px[x, y]
            if p[0] < 40 and 150 < p[1] < 215 and p[2] > 200:
                xs.append(x); ys.append(y)
    sel = (min(xs), min(ys), max(xs), max(ys)) if xs else None
    ov.box(sel, RED, 'seleccionada (con marco)')
    if sel:
        ov.hgap((sel[1] + sel[3]) // 2, 0, sel[0]) if sel[0] < W // 3 else None
        label = bbox(im, (W // 4, sel[3] + 10, 3 * W // 4, int(H * 0.8)), tol=40)
        ov.box(label, RED, 'recuento')
        if label:
            ov.vgap(W // 2 + 50, sel[3] + 1, label[1])
        hb = footer(ov, im, int(H * 0.86))
        if label and hb:
            ov.vgap(W // 2 + 90, label[3] + 1, hb[1])
        ov.vgap(W // 2 - 60, hdr[0][3] + 1 if hdr[0] else 0, sel[1])
    ov.im.save(out)


def grid_view(name, W, H, M, Y0, helpTop, cols, rows, out):
    im, ov = frame_common(name, W, H, M, 'grid')
    hdr = header(ov, im, M)
    ov.hline(Y0, BLUE, f'Y0={Y0}')
    area = bbox(im, (M // 2, Y0 - 10, W - M // 2, helpTop), tol=30)
    ov.box(area, RED, 'cuadricula (contenido)')
    hb = footer(ov, im, helpTop)
    if area and hb:
        ov.vgap(W - M - 20, area[3] + 1, hb[1])
    if area and hdr[0]:
        ov.vgap(M + 30, hdr[0][3] + 1, area[1])
    ov.im.save(out)


def detail_view(name, W, H, M, Y0L, listW, out, basic=False):
    im, ov = frame_common(name, W, H, M, 'det')
    hdr = header(ov, im, M)
    helpTop = int(H * 0.90)
    lst = bbox(im, (0, Y0L - 10, int(M + listW + 4) if not basic else W, helpTop), tol=14)
    ov.box(lst, RED, 'lista')
    if lst and hdr[0]:
        ov.vgap(M + 10, hdr[0][3] + 1, lst[1])
    if not basic:
        colx = bbox(im, (int(M + listW + 6), Y0L - 10, W, helpTop), tol=30)
        ov.box(colx, RED, 'columna derecha')
        if lst and colx:
            ov.hgap(lst[1] + 20, lst[2] + 1, colx[0])
        # bloques de la columna derecha (cover, nombre, metadatos, descripcion)
        if colx:
            x0 = colx[0]
            prev = None
            y = colx[1]
            blocks = []
            ys = [yy for yy in range(colx[1], helpTop) if bbox(im, (x0, yy, W, yy + 1), tol=30)]
            if ys:
                start = ys[0]
                last = ys[0]
                for yy in ys[1:]:
                    if yy > last + 4:
                        blocks.append((start, last)); start = yy
                    last = yy
                blocks.append((start, last))
            for i, (a, b) in enumerate(blocks):
                ov.hline(a, RED, None); ov.hline(b, RED, None)
                if i > 0:
                    ov.vgap(W - M - 14, blocks[i - 1][1] + 1, a)
    hb = footer(ov, im, helpTop)
    if lst and hb:
        ov.vgap(M + 60, lst[3] + 1, hb[1])
    ov.im.save(out)


if __name__ == '__main__':
    out = 'C:/tmp/zt/'
    # 720x720: M=22, T=150, Y0=90/101
    system_view('m11_sys', 720, 720, 22, 150, 650, out + 'med-sys-720.png')
    grid_view('m11_grid', 720, 720, 22, 94, 650, 4, 3, out + 'med-grid-720.png')
    detail_view('rt11_detailed', 720, 720, 22, 94, 374, out + 'med-det-720.png')
    detail_view('m11_basic', 720, 720, 22, 94, 676, out + 'med-basic-720.png', basic=True)
    # 640x480: M=18
    system_view('m43_sys', 640, 480, 18, 125, 435, out + 'med-sys-640.png')
    grid_view('m43_grid', 640, 480, 18, 70, 435, 4, 3, out + 'med-grid-640.png')
    detail_view('rt43_detailed', 640, 480, 18, 70, 333, out + 'med-det-640.png')
    detail_view('m43_basic', 640, 480, 18, 70, 604, out + 'med-basic-640.png', basic=True)
    print('ok')
