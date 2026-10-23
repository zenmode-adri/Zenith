#!/usr/bin/env python3
"""Genera web/probe-order.json: candidatos del modo de prueba de DTB, uno por "conjunto de pantalla" distinto.
Solo familias cuyo DTB no se puede deducir del ADC de arranque (clones).

Dos DTB cuentan como el MISMO candidato solo si es igual TODO lo que decide que el panel se encienda:
  - el nodo del panel (compatible, receta G/M/I completa, reset-gpios con su BANCO resuelto, alimentaciones, pinctrl),
  - la retroiluminacion (nodo backlight: pwms, enable-gpios, niveles, alimentacion) y las alimentaciones del panel
    (nodo del regulador, con su gpio/tension),
  - el nodo del host DSI (sin el panel hijo) y el PHY.
Los phandle se resuelven a la RUTA del nodo (no se descartan): el mismo numero de pin en otro banco es otro pin.
Orden: primero los conjuntos que comparten mas consolas; representante = menor corriente de carga (a igualdad, el
primero del catalogo). Necesita `dtc` y los .dtb compilados.
Uso: python3 probe_order.py <carpeta con rk3326-*.dtb> <catalog.json> <salida probe-order.json> [--explain]"""
import re, subprocess, glob, os, json, sys, hashlib, collections

dtb_dir, cat_path, out_path = sys.argv[1:4]
EXPLAIN = "--explain" in sys.argv
FAMILIES = ("r36s_clone", "r36s_sauce", "gameconsole", "k36")
cat = json.load(open(cat_path, encoding="utf-8"))
order_in_cat, fam_of, names_of = {}, {}, collections.defaultdict(list)
for i, d in enumerate(cat["devices"]):
    order_in_cat.setdefault(d["dtb"], i)
    fam_of.setdefault(d["dtb"], d["family"])
    names_of[d["dtb"]].append({"en": d["en"], "es": d["es"], "zh": d["zh"]})

def dts(p):
    return subprocess.run(["dtc", "-I", "dtb", "-O", "dts", "-q", p], capture_output=True, text=True).stdout

def phandles(t):
    stack, out = [], {}
    for line in t.split("\n"):
        s = line.strip()
        m = re.match(r"^([\w@,.+-]+(?::\s*[\w@,.+-]+)?|/)\s*\{$", s)
        if m:
            stack.append(m.group(1).split(":")[-1].strip()); continue
        if s == "};" and stack:
            stack.pop(); continue
        m = re.match(r"^phandle = <(0x[0-9a-f]+)>;", s)
        if m:
            out[int(m.group(1), 16)] = "/" + "/".join(x for x in stack if x != "/")
    return out

def block(t, pat):
    m = re.search(pat, t, re.M)
    if not m:
        return ""
    i = t.index("{", m.start()); depth = 0; j = i
    while j < len(t):
        if t[j] == "{": depth += 1
        elif t[j] == "}":
            depth -= 1
            if depth == 0: break
        j += 1
    return t[m.start():j + 1]

REF = ("reset-gpios", "enable-gpios", "gpios", "pwms", "backlight", "iovcc-supply", "vcc-supply", "vdd-supply",
       "power-supply", "pinctrl-0", "phys", "remote-endpoint", "power-domains", "rockchip,grf", "clocks", "resets")
def resolve(text, ph):
    def rep(m):
        name, first, rest = m.group(1), int(m.group(2), 16), m.group(3)
        return f"{name} = <{ph.get(first, 'phandle?')}{rest}>;"
    return re.sub(r"^\s*(" + "|".join(re.escape(r) for r in REF) + r") = <(0x[0-9a-f]+)([^>]*)>;", rep, text, flags=re.M)

def norm(s):
    s = re.sub(r"(linux,)?phandle = <0x[0-9a-f]+>;", "", s)
    s = re.sub(r"remote-endpoint = <[^>]*>;", "", s)
    return re.sub(r"\s+", " ", s).strip()

def strip_child(blk, child_pat):
    """quita el hijo (panel) de un bloque para comparar solo el host."""
    c = block(blk, child_pat)
    return blk.replace(c, "") if c else blk

def node_text(t, ph, path):
    """texto resuelto del nodo cuya ruta termina en `path` (p. ej. /backlight)."""
    name = path.rsplit("/", 1)[-1]
    return norm(resolve(block(t, r"^\s*(?:\w[\w-]*:\s*)?" + re.escape(name) + r"\s*\{"), ph))

def signature(t):
    ph = phandles(t)
    panel_raw = block(t, r"^\s*(?:\w[\w-]*:\s*)?panel@0\s*\{")
    panel = norm(resolve(panel_raw, ph))
    parts = {"panel": panel}
    # alimentaciones y retroiluminacion a las que apunta el panel
    for prop in ("iovcc-supply", "vcc-supply", "vdd-supply", "backlight"):
        m = re.search(r"^\s*" + prop + r" = <(0x[0-9a-f]+)>;", panel_raw, re.M)
        if m:
            path = ph.get(int(m.group(1), 16))
            parts[prop] = node_text(t, ph, path) if path else "?"
    dsi = block(t, r"^\s*(?:\w[\w-]*:\s*)?dsi@ff450000\s*\{")
    parts["dsi_host"] = norm(resolve(strip_child(dsi, r"^\s*(?:\w[\w-]*:\s*)?panel@0\s*\{"), ph))
    parts["dphy"] = norm(resolve(block(t, r"^\s*(?:\w[\w-]*:\s*)?dsi-dphy@ff2e0000\s*\{|^\s*(?:\w[\w-]*:\s*)?phy@ff2e0000\s*\{"), ph))
    return parts

def icc_of(t):
    b = block(t, r"battery\w*\s*\{")
    m = re.search(r"constant-charge-current-max-microamp = <(0x[0-9a-f]+)>", b)
    return int(m.group(1), 16) if m else 99999999

rows = []
for p in sorted(glob.glob(os.path.join(dtb_dir, "rk3326-*.dtb"))):
    n = os.path.basename(p)
    if fam_of.get(n) not in FAMILIES:
        continue
    t = dts(p)
    parts = signature(t)
    if not parts["panel"]:
        parts["panel"] = "sin-nodo-panel-generico:" + n    # otro driver de panel: se trata como propio
    sig = hashlib.sha1(json.dumps(parts, sort_keys=True).encode()).hexdigest()[:8]
    rows.append(dict(dtb=n, sig=sig, parts=parts, icc=icc_of(t), idx=order_in_cat[n]))

groups = collections.defaultdict(list)
for r in rows:
    groups[r["sig"]].append(r)
cands = []
for sig, g in groups.items():
    g.sort(key=lambda r: (r["icc"], r["idx"]))
    rep = g[0]
    cands.append({"dtb": rep["dtb"], "group": [x["dtb"] for x in sorted(g, key=lambda r: r["idx"])],
                  "icc": rep["icc"] if rep["icc"] != 99999999 else None, "_first": min(x["idx"] for x in g)})
cands.sort(key=lambda c: (-len(c["group"]), c["_first"]))
for c in cands:
    del c["_first"]
    c["names"] = names_of[c["dtb"]][:1]
json.dump({"families": list(FAMILIES), "candidates": cands}, open(out_path, "w", encoding="utf-8", newline="\n"), ensure_ascii=False, indent=1)
print(len(rows), "DTB ->", len(cands), "candidatos; grupos de varios:", [len(c["group"]) for c in cands if len(c["group"]) > 1])
if EXPLAIN:
    # que partes distinguen a cada par de DTB que antes se agrupaban solo por la receta
    by_panel = collections.defaultdict(list)
    for r in rows:
        by_panel[hashlib.sha1(re.sub(r"reset-gpios = <[^>]*>;", "", r["parts"]["panel"]).encode()).hexdigest()[:6]].append(r)
    for k, g in by_panel.items():
        if len({x["sig"] for x in g}) > 1:
            print("receta comun", k, "-> se parte en", len({x["sig"] for x in g}), "por:")
            ref = g[0]
            for x in g[1:]:
                diff = [p for p in ref["parts"] if ref["parts"][p] != x["parts"].get(p)]
                print("   ", ref["dtb"][8:-4], "vs", x["dtb"][8:-4], "difieren en:", diff)
