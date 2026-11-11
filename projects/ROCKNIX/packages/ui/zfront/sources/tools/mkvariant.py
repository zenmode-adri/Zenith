#!/usr/bin/env python3
"""Genera una variante de color del tema Zenith (carpeta fusionada, valida para zfront y para EmulationStation).

Uso: mkvariant.py <paleta.json> <carpeta_base> <carpeta_salida>
  <carpeta_base>  theme-light o theme-dark (salida de make-themes.sh); se elige segun "mode" de la paleta.
  Se copia la base completa y se cambian: <variables> y los colores literales de theme.xml, los PNG de color fijo
  (pill*, frame, tile, textinput*) y el acento cian de los demas PNG (botones de ayuda, marcos...).
No toca layout.xml, fuentes, sonidos ni logos de consolas (son marcas): la geometria y esos logos son los del Zenith
original. Si recolorea los iconos de trazo de las tarjetas (los que tienen etiqueta en assets/logos/icon-labels).

Paleta (JSON): name, mode ("light"|"dark"), y estos colores RRGGBB (sin #):
  bg txt dim dimDark tileTxt            texto y fondo
  accent accentDark accentSoft         "cyan", "cyanDark", "cyanSoft" de Zenith (acento, acento para texto, pastilla)
  surface                              tarjetas del carrusel y cuadros (blanco / 38383E en Zenith)
  panel separator track                fondo de paneles de menu, separadores, pista del deslizador
  switchOn switchOff                   interruptor
  hint hintBg                          texto y fondo (RRGGBBAA) de la barra de ayuda inferior
  scroll                               barra de desplazamiento (RRGGBBAA)
  inputBorder                          borde del cuadro de texto sin foco; inputBg = surface
Opcional:
  corner                               0..1, forma de las esquinas (defecto 1 = como Zenith; 0 = rectas; 0.4 = poco redondeado).
                                       Rehace con radio_nuevo = radio_original x corner (supersampling 4x/8x + LANCZOS) SOLO estas
                                       piezas, conservando tamano, margenes, grosor del aro, brillo, sombra y colores; logos e iconos no.
                                       Medidas de la base (verificadas con --check-corner):
                                         tile.png        384x384  tarjeta 256x256 en (64,64)-(319,319), radio 17; sombra gaussiana sigma 11
                                                         desplazada 8 px abajo (claro: (30,40,80) al 27 %; oscuro: negro al 50 %), alcance ~26 px
                                         frame.png       384x384  cuerpo 276x276 en (54,54)-(329,329), radio 20, aro de 7 px (acento) y brillo
                                                         exterior: el aro difuminado (sigma 7) al 75 %; alfa 2..54 fuera del aro, hasta ~16 px
                                         gridframe.png   48x48    aro de 4 px, caja (1,1)-(47,47), radio 9
                                         pill*/pillw*    WxH      estadio de alto h (26/30/36 px): radio h//2 (13/15/18); solo se rehace el alfa
                                         menu/textinput(-active).png 48x48  caja entera, radio 8, borde de 2 px
                                       Con corner ausente o 1 NO se regenera nada (salida identica byte a byte).
                                       `mkvariant.py --check-corner <theme-light|theme-dark>` compara cada modelo con los PNG de la base.
  selStyle                             "fill" (defecto, la pastilla de Zenith) | "outline" | "underline" | "bar": como se marca la fila elegida
                                       (solo contorno / linea debajo / barra fina a la izquierda, en el color de acento). Se escribe en theme.xml.
  headerAlign                          "left" (defecto) | "center": titulo de las pantallas de menu y lista. Se escribe en theme.xml.
  fontFallback                         ruta(s) relativas a la carpeta del tema, separadas por comas, de fuentes de respaldo para glifos que
                                       falten (CJK, emoji). Se escribe en theme.xml; la fuente debe estar ya en assets/ del tema.
"""
import colorsys, json, os, re, shutil, sys
from PIL import Image

# literales de theme.xml de la base, por modo: (etiqueta, valor) -> clave de la paleta
LIT = {
    "light": {"color": {"F7F7F9": "panel", "FFFFFF": "surface", "5F5F66": "hint"},
              "scrollbarColor": {"2D2D3260": "scroll"}, "separatorColor": {"E1E1E6": "separator"},
              "trackColor": {"D6D6DC": "track"}, "textColor": {"2D2D32": "txt"},
              "colorOn": {"3A8CA0": "switchOn"}, "colorOff": {"A9A9B2": "switchOff"},
              "backgroundColor": {"0000000A": "hintBg"}},
    "dark": {"color": {"2F2F34": "panel", "38383E": "surface", "A8A8B0": "hint"},
             "scrollbarColor": {"F0F0F260": "scroll"}, "separatorColor": {"3E3E44": "separator"},
             "trackColor": {"55555C": "track"}, "textColor": {"F0F0F2": "txt"},
             "colorOn": {"4C97A8": "switchOn"}, "colorOff": {"6C6C76": "switchOff"},
             "backgroundColor": {"FFFFFF0D": "hintBg"}},
}
# colores de los PNG de la base
BASE = {"light": {"soft": (196, 234, 243), "surface": (255, 255, 255), "border": (200, 200, 206), "inbg": (255, 255, 255)},
        "dark": {"soft": (34, 82, 95), "surface": (56, 56, 62), "border": (91, 91, 99), "inbg": (42, 42, 47)}}
CYAN = (0, 195, 227)
REQ = "bg txt dim dimDark tileTxt accent accentDark accentSoft surface panel separator track switchOn switchOff hint hintBg scroll inputBorder".split()


def rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def hexs(c):
    return "%02X%02X%02X" % tuple(c)


def retint_accent(im, accent):
    """Cambia el matiz cian de Zenith por el acento (H del acento; S y V escalados)."""
    import numpy as np
    ch, cs, cv = colorsys.rgb_to_hsv(*(v / 255 for v in CYAN))
    ah, as_, av = colorsys.rgb_to_hsv(*(v / 255 for v in accent))
    a = np.asarray(im, dtype=np.float32) / 255.0
    r, g, b, al = a[..., 0], a[..., 1], a[..., 2], a[..., 3]
    mx, mn = a[..., :3].max(-1), a[..., :3].min(-1)
    d = np.maximum(mx - mn, 1e-6)
    s_ = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0)
    h = np.where(mx == r, ((g - b) / d) % 6, np.where(mx == g, (b - r) / d + 2, (r - g) / d + 4)) / 6.0
    sel = (al > 0) & (s_ >= 0.35) & (np.abs(h - ch) <= 0.07)  # solo lo que ya es cian saturado
    if not sel.any():
        return False
    s2 = np.minimum(1.0, s_ * as_ / cs)
    v2 = np.minimum(1.0, mx * av / cv)
    h6 = ah * 6.0
    i = int(np.floor(h6)) % 6
    f = h6 - np.floor(h6)
    p_, q_, t_ = v2 * (1 - s2), v2 * (1 - s2 * f), v2 * (1 - s2 * (1 - f))
    tab = [(v2, t_, p_), (q_, v2, p_), (p_, v2, t_), (p_, q_, v2), (t_, p_, v2), (v2, p_, q_)][i]
    out = a.copy()
    for c in range(3):
        out[..., c] = np.where(sel, tab[c], a[..., c])
    im.paste(Image.fromarray(np.round(out * 255).astype(np.uint8), "RGBA"))
    return True


def swap(im, src, dst, tol=3):
    """Sustituye un color plano (con tolerancia) conservando el alfa."""
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a and abs(r - src[0]) <= tol and abs(g - src[1]) <= tol and abs(b - src[2]) <= tol:
                px[x, y] = (dst[0], dst[1], dst[2], a)


# ---------------------------------------------------------------------------------------------------------------------------
# corner: rehacer las esquinas de las piezas redondeadas. Los modelos reproducen los generadores originales de Zenith (con
# --check-corner: tile, frame, gridframe y alfa de pill* exactos; textinput = modelo aproximado, alfa +-28). Para tile, frame,
# gridframe y pill* se suma al PNG real la DIFERENCIA premultiplicada  modelo(corner) - modelo(1)  solo en los pixeles donde cambia:
# el resto queda byte a byte como en la base. textinput se rehace entero con el modelo (ver reshape_corner).
# ---------------------------------------------------------------------------------------------------------------------------
_CN, _CM = 384, 64  # lienzo de tile/frame y margen
BASE_SHADOW = {"light": ((30, 40, 80), 0.27), "dark": ((0, 0, 0), 0.50)}  # color y opacidad de la sombra de tile.png


def _canvas(w, h, s):
    return Image.new("RGBA", (w * s, h * s), (0, 0, 0, 0))


def _layer(mask, color, opacity=1.0):
    """RGBA de color solido y alfa = mascara * opacidad (sin halos oscuros al difuminar)."""
    r = Image.new("RGBA", mask.size, tuple(color) + (0,))
    r.putalpha(mask.point(lambda v: int(v * opacity)))
    return r


def _rrmask(w, h, s, box, radius, width=None):
    from PIL import ImageDraw
    m = Image.new("L", (w * s, h * s), 0)
    d = ImageDraw.Draw(m)
    b = [v * s for v in box]
    if width is None:
        d.rounded_rectangle(b, radius * s, fill=255)
    else:
        d.rounded_rectangle(b, radius * s, outline=255, width=width * s)
    return m


def _down(im, w, h):
    return im.resize((w, h), Image.LANCZOS)


def model_tile(mode, r):
    from PIL import ImageFilter
    S, N, M = 4, _CN, _CM
    sc, so = BASE_SHADOW[mode]
    im = _canvas(N, N, S)
    sh = _rrmask(N, N, S, (M, M + 8, M + 256, M + 264), r).filter(ImageFilter.GaussianBlur(11 * S))
    im.alpha_composite(_layer(sh, sc, so))
    im.alpha_composite(_layer(_rrmask(N, N, S, (M, M, M + 256, M + 256), r), BASE[mode]["surface"]))
    return _down(im, N, N)


def model_frame(mode, r):
    from PIL import ImageFilter
    S, N, M = 4, _CN, _CM
    O = (M - 10, M - 10, M + 256 + 10, M + 256 + 10)
    im = _canvas(N, N, S)
    halo = _rrmask(N, N, S, O, r, width=7).filter(ImageFilter.GaussianBlur(7 * S))
    im.alpha_composite(_layer(halo, CYAN, 0.75))
    im.alpha_composite(_layer(_rrmask(N, N, S, O, r), BASE[mode]["surface"]))
    im.alpha_composite(_layer(_rrmask(N, N, S, O, r, width=7), CYAN))
    return _down(im, N, N)


def model_gridframe(mode, r):
    from PIL import ImageDraw
    S, n = 4, 48
    im = _canvas(n, n, S)
    ImageDraw.Draw(im).rounded_rectangle((S, S, 47 * S, 47 * S), r * S, outline=CYAN + (255,), width=4 * S)
    return _down(im, n, n)


def model_pill(w, h, r):
    from PIL import ImageDraw
    S = 4
    im = Image.new("RGBA", (w * S, h * S), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle((0, 0, w * S - 1, h * S - 1), r * S, fill=(214, 242, 248, 255))
    return _down(im, w, h)


def model_textinput(im, r):
    """Cuadro de texto 48x48: borde de 2 px + relleno. Los colores se leen del PNG real (el activo no es exactamente el cian de
    Zenith); se reduce con BOX (LANCZOS deja un halo de 1 px entre el aro y el relleno que el recoloreado no ve)."""
    from PIL import ImageDraw
    S, n, bw = 8, 48, 2
    border, fill = im.getpixel((n // 2, 0)), im.getpixel((n // 2, n // 2))
    big = Image.new("RGBA", (n * S, n * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    d.rounded_rectangle([0, 0, n * S - 1, n * S - 1], radius=r * S, fill=border[:3] + (255,))
    d.rounded_rectangle([bw * S, bw * S, n * S - 1 - bw * S, n * S - 1 - bw * S], radius=max(0.0, r - bw) * S, fill=fill[:3] + (255,))
    return big.resize((n, n), Image.BOX)


def corner_model(rel, mode, im):
    """(radio original, funcion modelo(r)) de una pieza redondeada (rel = ruta relativa a assets/, con /), o None."""
    w, h = im.size
    if rel == "tile.png":
        return 17, lambda r: model_tile(mode, r)
    if rel == "frame.png":
        return 20, lambda r: model_frame(mode, r)
    if rel == "gridframe.png":
        return 9, lambda r: model_gridframe(mode, r)
    if rel.startswith(("pill-", "pillw-")) and "/" not in rel:
        return h // 2, lambda r: model_pill(w, h, r)
    if rel in ("menu/textinput.png", "menu/textinput-active.png"):
        return 8, lambda r: model_textinput(im, r)
    return None


def reshape_corner(im, rel, mode, corner):
    """Devuelve im con las esquinas escaladas por corner (0..1), o im tal cual si rel no es una pieza redondeada."""
    import numpy as np
    cm = corner_model(rel, mode, im)
    if cm is None:
        return im
    r0, mk = cm
    if rel.startswith("menu/"):
        # textinput: el modelo no es identico al PNG original (alfa +-28 en unos pixeles de cada esquina): la diferencia dejaria
        # motas en las esquinas, asi que con corner != 1 la pieza se rehace entera (48x48: solo borde y relleno planos)
        return mk(r0 * corner)
    p1 = _pre(np.asarray(mk(r0), dtype=np.float64))
    pc = _pre(np.asarray(mk(r0 * corner), dtype=np.float64))
    o = np.asarray(im, dtype=np.float64)
    d = pc - p1
    chg = (np.abs(d) > 1e-9).any(-1)
    pn = _pre(o) + d
    al = np.clip(pn[..., 3], 0, 255)
    safe = np.where(al > 0, al, 1)[..., None]
    col = np.clip(np.clip(pn[..., :3], 0, None) * 255.0 / safe, 0, 255)
    out = o.copy()
    new = np.concatenate([col, al[..., None]], axis=-1)
    out[chg] = new[chg]
    out[chg & (al <= 0)] = 0
    return Image.fromarray(np.round(out).astype(np.uint8), "RGBA").copy()


def _pre(a):  # premultiplicado, escala 0..255
    import numpy as np
    return np.concatenate([a[..., :3] * a[..., 3:4] / 255.0, a[..., 3:4]], axis=-1)


def check_corner(base):
    """Compara cada modelo (corner 1) con los PNG de la base. Uso: mkvariant.py --check-corner <theme-light|theme-dark>."""
    import numpy as np
    mode = "dark" if base.rstrip("/\\").endswith("dark") else "light"
    A = os.path.join(base, "assets")
    names = ["tile.png", "frame.png", "gridframe.png", "menu/textinput.png", "menu/textinput-active.png"]
    names += sorted(f for f in os.listdir(A) if f.startswith(("pill-", "pillw-")) and f.endswith(".png"))
    for rel in names:
        im = Image.open(os.path.join(A, rel)).convert("RGBA")
        r0, mk = corner_model(rel, mode, im)
        a, b = np.asarray(im).astype(int), np.asarray(mk(r0)).astype(int)
        # los pill llevan el color propio (se recolorea siempre): solo cuenta el alfa
        print("%-26s r=%2d  maxdiff RGBA %3d  maxdiff alfa %3d" % (rel, r0, np.abs(a - b).max(), np.abs(a[..., 3] - b[..., 3]).max()))


def main():
    if sys.argv[1] == "--check-corner":
        return check_corner(sys.argv[2])
    pal_path, base, out = sys.argv[1:4]
    pal = json.load(open(pal_path, encoding="utf-8"))
    miss = [k for k in REQ if k not in pal]
    if miss or pal.get("mode") not in ("light", "dark"):
        sys.exit("paleta incompleta: faltan %s / mode light|dark" % (miss or "mode"))
    mode = pal["mode"]
    corner = float(pal.get("corner", 1))
    if not 0 <= corner <= 1:
        sys.exit("corner debe estar entre 0 y 1 (1 = esquinas de Zenith)")
    if os.path.exists(out):
        shutil.rmtree(out)
    shutil.copytree(base, out)
    # --- theme.xml
    p = os.path.join(out, "theme.xml")
    x = open(p, encoding="utf-8", newline="").read()
    for tag, key in (("bg", "bg"), ("txt", "txt"), ("dim", "dim"), ("dimDark", "dimDark"), ("tileTxt", "tileTxt"),
                     ("cyan", "accent"), ("cyanDark", "accentDark"), ("cyanSoft", "accentSoft")):
        x, n = re.subn(r"(<%s>)[0-9A-Fa-f]{6}(</%s>)" % (tag, tag), r"\g<1>%s\g<2>" % pal[key].upper(), x, count=1)
        assert n == 1, tag
    for tag, m in LIT[mode].items():
        for old, key in m.items():
            x, n = re.subn(r"(<%s>)%s(</%s>)" % (tag, old, tag), r"\g<1>%s\g<2>" % pal[key].upper(), x)
    if corner != 1:  # el motor lee <corner> (ui_style: radio = alto/2 x corner); ausente = 1 = como Zenith
        x, n = re.subn(r"(<bg>[0-9A-Fa-f]{6}</bg>)", r"\g<1>\n    <corner>%g</corner>" % corner, x, count=1)
        assert n == 1, "corner"
    for key, ok in (("selStyle", ("fill", "outline", "underline", "bar")), ("headerAlign", ("left", "center")), ("fontFallback", None)):
        if key not in pal:
            continue
        if ok and pal[key] not in ok:
            sys.exit("%s debe ser uno de %s" % (key, "|".join(ok)))
        x, n = re.subn(r"(<bg>[0-9A-Fa-f]{6}</bg>)", lambda m: m.group(1) + "\n    <%s>%s</%s>" % (key, pal[key], key), x, count=1)
        assert n == 1, key
    x = re.sub(r"Zenith \(tema (claro|oscuro)\)", "Zenith (variante %s)" % pal.get("name", "?"), x, count=1)
    open(p, "w", encoding="utf-8", newline="").write(x)
    # --- PNG
    b = BASE[mode]
    acc = rgb(pal["accent"])
    # iconos de trazo de las tarjetas (Music, Tools, Screenshots...) = los que el tema etiqueta con
    # assets/logos/icon-labels/<etiqueta>/<sistema>.txt; los logos de consolas son marcas y no se recolorean
    card_icons = {os.path.splitext(f)[0] for _, _, fs in os.walk(os.path.join(out, "assets", "logos", "icon-labels")) for f in fs if f.endswith(".txt")}
    for root, _, files in os.walk(os.path.join(out, "assets")):
        if os.sep + "fonts" in root:
            continue
        for f in files:
            if f.endswith(".webp") and os.sep + "logos" in root:
                if os.path.splitext(f)[0] in card_icons:  # cian cocido en el icono
                    fp = os.path.join(root, f)
                    im = Image.open(fp).convert("RGBA")
                    if retint_accent(im, acc):
                        im.save(fp, lossless=True)
                continue
            if not f.endswith(".png") or f.startswith("zenith-splash"):
                continue
            fp = os.path.join(root, f)
            im = Image.open(fp).convert("RGBA")
            if corner != 1:
                im = reshape_corner(im, os.path.relpath(fp, os.path.join(out, "assets")).replace(os.sep, "/"), mode, corner)
            if f.startswith("pill"):
                px = im.load()
                soft = rgb(pal["accentSoft"])
                for yy in range(im.height):
                    for xx in range(im.width):
                        a = px[xx, yy][3]
                        if a:
                            px[xx, yy] = soft + (a,)
            else:
                if f in ("frame.png", "tile.png"):
                    swap(im, b["surface"], rgb(pal["surface"]), 4)
                elif f.startswith("textinput"):
                    swap(im, b["inbg"], rgb(pal["surface"]), 4)
                    if f == "textinput.png":
                        swap(im, b["border"], rgb(pal["inputBorder"]), 4)
                retint_accent(im, acc)
            im.save(fp)
    json.dump(pal, open(os.path.join(out, "variant.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    print("ok", out)


if __name__ == "__main__":
    main()
