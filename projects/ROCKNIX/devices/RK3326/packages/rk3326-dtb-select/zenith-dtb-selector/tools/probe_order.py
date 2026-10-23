#!/usr/bin/env python3
"""Genera web/probe-order.json: candidatos del modo de prueba de DTB, uno por receta de pantalla distinta.
Solo familias cuyo DTB no se puede deducir del ADC de arranque (clones). Orden: primero las recetas que comparten mas
consolas; de cada grupo se propone como representante el DTB de corriente de carga mas baja (a igualdad, el primero
del catalogo). Necesita `dtc` y los .dtb compilados.
Uso: python3 probe_order.py <carpeta con rk3326-*.dtb> <catalog.json> <salida probe-order.json>"""
import re, subprocess, glob, os, json, sys, hashlib, collections

dtb_dir, cat_path, out_path = sys.argv[1:4]
FAMILIES = ("r36s_clone", "r36s_sauce", "gameconsole", "k36")
cat = json.load(open(cat_path, encoding="utf-8"))
order_in_cat = {}
fam_of = {}
names_of = collections.defaultdict(list)
for i, d in enumerate(cat["devices"]):
    order_in_cat.setdefault(d["dtb"], i)
    fam_of.setdefault(d["dtb"], d["family"])
    names_of[d["dtb"]].append({"en": d["en"], "es": d["es"], "zh": d["zh"]})

def dts(p):
    return subprocess.run(["dtc", "-I", "dtb", "-O", "dts", "-q", p], capture_output=True, text=True).stdout

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

def num(blk, name):
    m = re.search(r"^\s*" + re.escape(name) + r" = <(0x[0-9a-f]+)>;", blk, re.M)
    return int(m.group(1), 16) if m else None

rows = []
for p in sorted(glob.glob(os.path.join(dtb_dir, "rk3326-*.dtb"))):
    n = os.path.basename(p)
    if fam_of.get(n) not in FAMILIES:
        continue
    t = dts(p)
    panel = block(t, r"^\s*(?:\w[\w-]*:\s*)?panel@0\s*\{")
    desc = re.sub(r"\s+", " ", " ".join(re.findall(r'"([GMI][^"]*)"', panel)))
    comp = (re.findall(r'compatible = "([^"]*)"', panel) or [""])[0]
    # sin panel generico (otro driver): la receta es el compatible; cada DTB cuenta como propio si no hay descripcion
    sig = hashlib.sha1((desc or ("compat:" + comp + ":" + n)).encode()).hexdigest()[:8]
    icc = num(block(t, r"battery\w*\s*\{"), "constant-charge-current-max-microamp") or 99999999
    rows.append(dict(dtb=n, sig=sig, icc=icc, fam=fam_of[n], idx=order_in_cat[n]))

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
