#!/usr/bin/env python3
"""Genera la tarjeta del modo de prueba de DTB: zenith-probe-<WxH>.bgra (crudo BGRA, fila = ancho*4) y el PNG 640x480
(que zenith-splash convierte con ImageMagick para otras resoluciones). Mismo estilo que las tarjetas de primer arranque.
Uso: python make_probe_frames.py <carpeta de salida>"""
import sys, os
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
BG = (0x26, 0x26, 0x2a)
WHITE = (255, 255, 255)
GREY = (150, 150, 158)
CYAN = (0, 188, 212)
AMBER = (255, 193, 7)

LATIN = [r"C:\Windows\Fonts\segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]
CJK = [r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\simhei.ttf", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"]

def font(cands, size):
    for p in cands:
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    raise SystemExit("falta una fuente: " + str(cands))

def draw_card(W, H):
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    s = min(W / 640, H / 480)             # escala respecto al diseño base
    def f(sz): return font(LATIN, int(sz * s))
    def fz(sz): return font(CJK, int(sz * s))
    cx = W // 2
    top = int((H - 480 * s) / 2)          # centrado vertical en pantallas mas altas

    # logo centrado: circulo + "ZENITH"
    word = "E  N  I  T  H"; wf = f(25)
    r = int(17 * s); gap = int(14 * s)
    tw = d.textlength(word, font=wf)
    total = 2 * r + gap + tw
    x_start = cx - total / 2
    ly = top + int(58 * s)
    d.ellipse([x_start, ly - r, x_start + 2 * r, ly + r], fill=CYAN)
    d.text((x_start + r, ly), "Z", font=f(24), fill=WHITE, anchor="mm")
    d.text((x_start + 2 * r + gap, ly), word, font=wf, fill=WHITE, anchor="lm")

    # pregunta (3 idiomas)
    d.text((cx, top + int(122 * s)), "Can you see this screen clearly?", font=f(24), fill=WHITE, anchor="mm")
    d.text((cx, top + int(156 * s)), "你能清楚看到画面吗？  -  ¿Ves bien esta pantalla?", font=fz(19), fill=GREY, anchor="mm")

    # respuesta SI
    y1 = top + int(226 * s); rr = int(23 * s); x0 = int(56 * s + (W - 640 * s) / 2) + rr
    tx = x0 + int(44 * s)
    d.ellipse([x0 - rr, y1 - rr, x0 + rr, y1 + rr], fill=CYAN)
    d.text((x0, y1), "A", font=f(24), fill=WHITE, anchor="mm")
    d.text((tx, y1 - int(13 * s)), "Yes: press any button (or VOL / POWER)", font=f(22), fill=WHITE, anchor="lm")
    d.text((tx, y1 + int(15 * s)), "是：按任意键  -  Sí: pulsa un botón", font=fz(18), fill=GREY, anchor="lm")

    # respuesta NO
    y2 = top + int(306 * s)
    d.ellipse([x0 - rr, y2 - rr, x0 + rr, y2 + rr], outline=GREY, width=max(2, int(2 * s)))
    d.text((x0, y2), "…", font=f(24), fill=GREY, anchor="mm")
    d.text((tx, y2 - int(13 * s)), "No: wait, the next one is tried", font=f(22), fill=WHITE, anchor="lm")
    d.text((tx, y2 + int(15 * s)), "否：等待，自动试下一个  -  No: espera, se prueba otra", font=fz(18), fill=GREY, anchor="lm")

    # aviso del cargador (dos lineas)
    d.text((cx, top + int(378 * s)), "Unplug the charger while testing", font=f(16), fill=AMBER, anchor="mm")
    d.text((cx, top + int(400 * s)), "测试期间请拔下充电器  -  Desconecta el cargador durante la prueba", font=fz(14), fill=AMBER, anchor="mm")
    d.text((cx, top + int(440 * s)), "Zenith DTB probe  -  each try takes about 12 s", font=f(14), fill=GREY, anchor="mm")
    return im

def to_bgra(im):
    r, g, b = im.split()
    return Image.merge("RGBA", (b, g, r, Image.new("L", im.size, 255))).tobytes()

os.makedirs(OUT, exist_ok=True)
for W, H in ((640, 480), (720, 720), (1024, 768), (480, 854), (720, 1280)):
    im = draw_card(W, H)
    open(os.path.join(OUT, f"zenith-probe-{W}x{H}.bgra"), "wb").write(to_bgra(im))
    if (W, H) == (640, 480):
        im.save(os.path.join(OUT, "zenith-probe-640x480.png"))
    im.save(os.path.join(os.path.dirname(os.path.abspath(__file__)), f"preview-probe-{W}x{H}.png"))
print("ok")
