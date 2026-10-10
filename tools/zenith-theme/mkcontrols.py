"""Controles del menu de ES para Zenith: interruptor, boton del deslizador y boton (nine-patch).
Todo en BLANCO: ES lo tine con el color del tema (colorOn/colorOff del interruptor, color del texto/selector del boton).
Salida: <tema>/assets/menu/{switch-on.svg,switch-off.svg,slider-knob.svg,button.png,button-filled.png}"""
import os, sys
from PIL import Image, ImageDraw

OUT = os.path.join(sys.argv[1], 'assets', 'menu')
os.makedirs(OUT, exist_ok=True)

PILL = 'M12 0h20a12 12 0 0 1 0 24H12a12 12 0 0 1 0-24z'


def hole(cx):
    return f'M{cx + 9} 12a9 9 0 1 0 -18 0a9 9 0 1 0 18 0z'


def svg(w, h, body):
    return (f'<?xml version="1.0" encoding="utf-8"?>\n<svg viewBox="0 0 {w} {h}" xmlns="http://www.w3.org/2000/svg">{body}</svg>\n')


# interruptor: pastilla blanca con el pomo como hueco (a la derecha = activado)
open(os.path.join(OUT, 'switch-on.svg'), 'w').write(svg(44, 24, f'<path fill="#ffffff" fill-rule="evenodd" d="{PILL} {hole(32)}"/>'))
open(os.path.join(OUT, 'switch-off.svg'), 'w').write(svg(44, 24, f'<path fill="#ffffff" fill-rule="evenodd" d="{PILL} {hole(12)}"/>'))
# pomo del deslizador: anillo
open(os.path.join(OUT, 'slider-knob.svg'), 'w').write(svg(24, 24, '<circle cx="12" cy="12" r="8" fill="none" stroke="#ffffff" stroke-width="4"/>'))

# boton: nine-patch 32x32 con esquina de 16 px (pastilla exacta con 32 px de alto)
S = 8
N = 32


def button(path, filled):
    im = Image.new('RGBA', (N * S, N * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, N * S - 1, N * S - 1], radius=16 * S, fill=(255, 255, 255, 255))
    if not filled:
        w = int(2.0 * S)
        d.rounded_rectangle([w, w, N * S - 1 - w, N * S - 1 - w], radius=16 * S - w, fill=(255, 255, 255, 0))
    im.resize((N, N), Image.LANCZOS).save(path)


button(os.path.join(OUT, 'button.png'), False)
button(os.path.join(OUT, 'button-filled.png'), True)
print('ok', sorted(os.listdir(OUT))[-6:])
