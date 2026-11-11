#!/usr/bin/env python3
"""Montaje de una variante: 4 resoluciones (filas) x pantallas de screens.txt (4 columnas por pagina) con rejilla de 40 px, e informe de
contraste WCAG en la primera pagina. Paginas: <salida>.png, <salida sin .png>2.png...
Uso: montage.py <paleta.json> <carpeta_capturas> <salida.png>"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFont

pal = json.load(open(sys.argv[1], encoding="utf-8"))
src, out = sys.argv[2], sys.argv[3]
SIZES = ["480x320", "640x480", "720x405", "720x720"]
HERE = os.path.dirname(os.path.abspath(__file__))
SCREENS = [l.split("\t")[0] for l in open(os.path.join(HERE, "screens.txt"), encoding="utf-8").read().splitlines() if l.strip()]
PER_PAGE = 4
H = 260  # alto de cada celda


def lum(h):
    c = [int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    c = [v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4 for v in c]
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def ratio(a, b):
    la, lb = lum(a), lum(b)
    if la < lb:
        la, lb = lb, la
    return (la + 0.05) / (lb + 0.05)


# (descripcion, primer plano, fondo, minimo): lo que se lee de verdad en Zenith
CHECKS = [("texto sobre fondo (txt/bg)", "txt", "bg", 7.0), ("texto tenue (dim/bg)", "dim", "bg", 4.5),
          ("fila elegida (accentDark/accentSoft)", "accentDark", "accentSoft", 4.5),
          ("titulo de tarjeta (tileTxt/surface)", "tileTxt", "surface", 4.5), ("acento sobre fondo (accent/bg)", "accent", "bg", 3.0),
          ("acento sobre tarjeta (accent/surface)", "accent", "surface", 3.0), ("ayuda (hint/bg)", "hint", "bg", 4.5)]
rows = []
for d, f, b, mn in CHECKS:
    r = ratio(pal[f], pal[b])
    rows.append((d, r, mn, r >= mn))

font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 11)
fontb = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 13)


def cell(sz, sc):
    w, h = map(int, sz.split("x"))
    im = Image.open("%s/%s_%s.png" % (src, sz, sc)).convert("RGB")
    k = H / im.height
    im = im.resize((round(im.width * k), H), Image.LANCZOS)
    d = ImageDraw.Draw(im, "RGBA")
    for x in range(0, w + 1, 40):  # rejilla cada 40 px de la captura original
        X = round(x * k)
        d.line([(X, 0), (X, H)], fill=(255, 0, 90, 60 if x % 200 else 130))
        if x % 200 == 0:
            d.text((X + 2, 1), str(x), fill=(255, 60, 120, 255), font=font)
    for y in range(0, h + 1, 40):
        Y = round(y * k)
        d.line([(0, Y), (im.width, Y)], fill=(0, 110, 255, 60 if y % 200 else 130))
        if y % 200 == 0 and y:
            d.text((1, Y + 1), str(y), fill=(0, 110, 255, 255), font=font)
    return im


pages = [SCREENS[i:i + PER_PAGE] for i in range(0, len(SCREENS), PER_PAGE)]
for pi, scr in enumerate(pages):
    cells = [[cell(sz, sc) for sc in scr] for sz in SIZES]
    colw = [max(r[i].width for r in cells) for i in range(len(scr))]
    W = sum(colw) + (len(scr) + 1) * 8
    rep_h = (20 + 16 * len(rows)) if pi == 0 else 0
    canvas = Image.new("RGB", (W, 24 + len(cells) * (H + 20) + rep_h + 8), (24, 24, 28))
    d = ImageDraw.Draw(canvas)
    d.text((8, 4), "%s (%s)  accent #%s  bg #%s  corner %s   [%s]" % (pal["name"], pal["mode"], pal["accent"], pal["bg"], pal.get("corner", 1), ", ".join(scr)),
           fill=(240, 240, 240), font=fontb)
    y = 24
    for sz, row in zip(SIZES, cells):
        d.text((8, y), sz, fill=(200, 200, 120), font=font)
        x = 8
        for i, im in enumerate(row):
            canvas.paste(im, (x, y + 16))
            x += colw[i] + 8
        y += H + 20
    if pi == 0:
        d.text((8, y), "CONTRASTE WCAG (verde = cumple el minimo)", fill=(240, 240, 240), font=font)
        for i, (desc, r, mn, ok) in enumerate(rows):
            d.text((8, y + 16 + 16 * i), "%-42s %5.2f  (min %.1f)  %s" % (desc, r, mn, "OK" if ok else "BAJO"),
                   fill=(120, 230, 140) if ok else (255, 110, 110), font=font)
    path = out if pi == 0 else out[:-4] + str(pi + 1) + ".png"
    canvas.save(path)
    print("montaje", path)
for desc, r, mn, ok in rows:
    print("%-42s %5.2f  min %.1f  %s" % (desc, r, mn, "OK" if ok else "BAJO"))
