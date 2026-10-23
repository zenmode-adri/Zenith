#!/usr/bin/env python3
"""Genera catalog.json (zh/en/es) a partir del selector heredado (dtb_selector_src.py).
Los nombres chinos son las claves originales; el ingles sale de DEVICE_TRANSLATIONS; el español se deriva por reglas."""
import json, re, importlib.util, sys

spec = importlib.util.spec_from_file_location("dsel", "../../dtb_selector.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

# familias: id -> (zh, en, es). El orden es el del menu.
FAMILIES = [
    ("udt",       "UDT",        "UDT",            "UDT"),
    ("r36s_clone", "R36S克隆",  "R36S Clone",     "R36S clon"),
    ("r36s_sauce", "R36S酱油",  "R36S Sauce",     "R36S Sauce"),
    ("gameconsole", "Game Console", "Game Console", "Game Console"),
    ("xifan",     "稀范科技",   "Xifan",          "Xifan"),
    ("anbernic",  "安伯尼克",   "Anbernic",       "Anbernic"),
    ("powkiddy",  "泡机堂",     "PowKiddy",       "PowKiddy"),
    ("ymc",       "亿米创",     "YMC",            "YMC"),
    ("diium",     "迪优米",     "Diium",          "Diium"),
    ("magicx",    "Magicx",     "Magicx",         "Magicx"),
    ("gamemt",    "漫特科技",   "GAMEMT",         "GAMEMT"),
    ("odroid",    "Odroid",     "Odroid",         "Odroid"),
    ("aislpc",    "AISLPC",     "AISLPC",         "AISLPC"),
    ("k36",       "K36系列",    "K36 series",     "Serie K36"),
    ("batlexp",   "Batlexp",    "Batlexp",        "Batlexp"),
    ("other",     "其他",       "Others",         "Otros"),
]
ZH_TO_FAM = {
    "稀范科技": "xifan", "安伯尼克": "anbernic", "亿米创": "ymc", "迪优米": "diium", "Magicx": "magicx",
    "泡机堂": "powkiddy", "漫特科技": "gamemt", "Odroid": "odroid", "Game Console": "gameconsole",
    "R36S克隆": "r36s_clone", "R36S酱油": "r36s_sauce", "K36系列": "k36", "AISLPC": "aislpc",
    "Batlexp": "batlexp", "UDT": "udt", "其他": "other",
}
cat_of = {}
for c, items in m.CATEGORIES.items():
    for k in items:
        cat_of[k] = ZH_TO_FAM[c]
# modelos que el selector viejo dejaba sin categoria
cat_of["BatleXP G350"] = "batlexp"
cat_of["ODROID-GO Advance"] = "odroid"
cat_of["ODROID-GO Advance 1.1"] = "odroid"


def clean_en(s):
    s = s.replace("RGB2OS", "RGB20S")
    s = re.sub(r"Type(\d)", r"Type \1", s)
    s = re.sub(r"Panel(\d)", r"Panel \1", s)
    s = s.replace("with Amp", "with amplifier").replace("without Amp", "without amplifier")
    s = s.replace("P2 Screen", "P2 screen").replace("P1 Screen", "P1 screen").replace("P3 Screen", "P3 screen").replace("P4 Screen", "P4 screen")
    return s


def to_es(s):
    s = clean_en(s)
    s = s.replace("Clone", "clon")
    s = re.sub(r"Type (\d)", r"tipo \1", s)
    s = re.sub(r"Panel (\d)", r"pantalla \1", s)
    s = s.replace("without amplifier", "sin amplificador").replace("with amplifier", "con amplificador")
    s = s.replace("(Inverted R-Stick)", "(stick derecho invertido)")
    s = re.sub(r"\b(P\d) screen", r"pantalla \1", s)
    s = s.replace("Original", "original")
    return s


devices = []
seen_dtb_name = set()
for k, e in m.ALL_DEVICES.items():
    if k == "BatleXP G350":          # duplicado de "Batlexp G350" (mismo DTB); se queda el categorizado
        continue
    en = clean_en(m.DEVICE_TRANSLATIONS.get(k, k))
    es = to_es(m.DEVICE_TRANSLATIONS.get(k, k))
    zh = k
    uart = int(e.tty) % 100 if isinstance(e.tty, int) else 2
    fam = cat_of.get(k, "other")
    devices.append({
        "id": re.sub(r"[^a-z0-9]+", "-", en.lower()).strip("-"),
        "family": fam, "zh": zh, "en": en, "es": es,
        "dtb": e.dtb, "uart": uart, "overlay": e.overlay,
    })

# DTB que la imagen Zenith incluye y el selector heredado no listaba
EXTRA = [
    ("gameconsole", "Game Console R36xx ProMax", "Game Console R36xx ProMax", "Game Console R36xx ProMax", "rk3326-gameconsole-r36xxpromax.dtb", 2),
    ("gameconsole", "Game Console EE Clone", "Game Console EE Clone", "Game Console EE Clone", "rk3326-gameconsole-eeclone.dtb", 2),
]
for fam, zh, en, es, dtb, uart in EXTRA:
    devices.append({"id": "", "family": fam, "zh": zh, "en": en, "es": es, "dtb": dtb, "uart": uart, "overlay": None})
for d in devices:
    if not d["id"]:
        d["id"] = re.sub(r"[^a-z0-9]+", "-", d["en"].lower()).strip("-")

# ids unicos
cnt = {}
for d in devices:
    cnt[d["id"]] = cnt.get(d["id"], 0) + 1
    if cnt[d["id"]] > 1:
        d["id"] += "-%d" % cnt[d["id"]]

# El R36 Ultra de Zenith usa ttyS2 en su extlinux de fabrica (el catalogo viejo decia ttyS1)
for d in devices:
    if d["family"] == "udt":
        d["uart"] = 2

order = {f[0]: i for i, f in enumerate(FAMILIES)}
devices.sort(key=lambda d: order[d["family"]])   # sort estable: dentro de cada familia queda el orden original
out = {
    "families": [{"id": a, "zh": b, "en": c, "es": d} for a, b, c, d in FAMILIES],
    "devices": devices,
}
json.dump(out, open("../web/catalog.json", "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print(len(devices), "dispositivos,", len(set(d["dtb"] for d in devices)), "DTB distintos")
for d in devices[:14]:
    print(d["family"], "|", d["zh"], "|", d["en"], "|", d["es"], "|", d["dtb"], d["uart"])
