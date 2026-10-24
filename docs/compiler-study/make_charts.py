"""Charts for docs/COMPILER-STUDY.md, drawn in the Zenith theme (dark and light).

Numbers are copied by hand from the study tables (batches gb1, pg4, sn1, pg5, gp1, px1, P2, perf) measured on an
R36 Ultra on 23-24 Oct 2026. Run: python make_charts.py <output dir>   (needs matplotlib)."""
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
from matplotlib.patches import FancyBboxPatch

out = sys.argv[1] if len(sys.argv) > 1 else "."

# Colours sampled from the Zenith ES theme (dark: bg 26262a, card 38383e; light: bg ebebed, card ffffff; accent 00c3e3)
THEMES = {
    "dark": dict(bg="#26262a", card="#38383e", text="#eaeaec", muted="#97979e", grid="#4a4a52", accent="#00c3e3",
                 flag="#9b9ba6", lto="#f5a524", other="#6f6f7a", pink="#e879a8"),
    "light": dict(bg="#ebebed", card="#ffffff", text="#26262a", muted="#6b6b75", grid="#d6d6da", accent="#00a3c0",
                  flag="#7a7a86", lto="#d98a00", other="#b9b9c2", pink="#c4457f"),
}
_have = {f.name for f in font_manager.fontManager.ttflist}
FONT = next((f for f in ("Ubuntu", "Segoe UI", "Noto Sans", "DejaVu Sans") if f in _have), "DejaVu Sans")


def style(t):
    plt.rcParams.update({
        "font.family": FONT, "font.size": 10, "text.color": t["text"], "axes.labelcolor": t["muted"],
        "xtick.color": t["muted"], "ytick.color": t["text"], "axes.edgecolor": t["grid"],
        "figure.facecolor": t["bg"], "axes.facecolor": t["card"], "savefig.facecolor": t["bg"],
        "axes.spines.top": False, "axes.spines.right": False, "axes.spines.left": False, "axes.spines.bottom": False,
    })


def header(fig, t, title, sub):
    h = fig.get_figheight()
    fig.text(0.035, 1 - 0.22 / h, title, fontsize=15, fontweight="bold", color=t["text"], va="top")
    fig.text(0.035, 1 - 0.62 / h, sub, fontsize=9.5, color=t["muted"], va="top")


def footer(fig, t, extra=""):
    fig.text(0.035, 0.02, "Zenith  ·  R36 Ultra  ·  CPU time per frame, not battery" + extra, fontsize=8, color=t["muted"])


def card(ax, t):
    ax.set_facecolor("none")
    ax.add_patch(FancyBboxPatch((0, 0), 1, 1, boxstyle="round,pad=0,rounding_size=0.02", transform=ax.transAxes,
                                facecolor=t["card"], edgecolor="none", zorder=-5, clip_on=False))


def save(fig, name, mode, out):
    fig.savefig(f"{out}/{name}-{mode}.png", dpi=150)
    plt.close(fig)


def legend(fig, t, items, y, x=0.035, step=None):
    for name, c in items:
        fig.text(x, y, "●", color=c, fontsize=11, va="center")
        fig.text(x + 0.02, y, name, color=t["muted"], fontsize=8.5, va="center")
        x += 0.02 + 0.0068 * len(name) + 0.035


# ---- 0. summary tiles ------------------------------------------------------------------------------------------------
def tiles(mode):
    t = THEMES[mode]
    style(t)
    fig = plt.figure(figsize=(9, 3.6), dpi=150)
    header(fig, t, "What compiling the emulators differently buys on the R36 Ultra",
           "Change against the shipped build, measured on hardware. Nothing here is applied to the image.")
    data = [
        ("gambatte", "Game Boy / GBC", "-10 %", "CPU per frame with -O3 -flto; PGO adds ~1 point", t["accent"]),
        ("snes9x", "SNES", "-4 %", "CPU per frame with PGO; compiler flags: nothing", t["accent"]),
        ("PPSSPP", "PSP, Tenchu 4", "+3 % fps", "LTO, only in the game with no frame cap", t["lto"]),
        ("pcsx · gpsp", "PlayStation · GBA", "≈ 0", "their hot loops are hand-written NEON / dynarec", t["other"]),
    ]
    w, gap, x0, y0, h = 0.2125, 0.0167, 0.035, 0.12, 0.60
    for i, (name, sys_, big, note, col) in enumerate(data):
        ax = fig.add_axes([x0 + i * (w + gap), y0, w, h])
        ax.set_xlim(0, 1)
        ax.set_ylim(0, 1)
        ax.axis("off")
        card(ax, t)
        ax.add_patch(FancyBboxPatch((0.08, 0.88), 0.14, 0.035, boxstyle="round,pad=0,rounding_size=0.015",
                                    transform=ax.transAxes, facecolor=col, edgecolor="none"))
        ax.text(0.08, 0.78, name, fontsize=12.5, fontweight="bold", color=t["text"], va="center")
        ax.text(0.08, 0.68, sys_, fontsize=8.5, color=t["muted"], va="center")
        ax.text(0.08, 0.43, big, fontsize=24 if len(big) < 6 else 19, fontweight="bold", color=col, va="center")
        words = note.split()
        lines, cur = [], ""
        for wd in words:
            if len(cur) + len(wd) + 1 > 24:
                lines.append(cur)
                cur = wd
            else:
                cur = (cur + " " + wd).strip()
        lines.append(cur)
        for k, ln in enumerate(lines):
            ax.text(0.08, 0.22 - k * 0.075, ln, fontsize=7.6, color=t["muted"], va="center")
    footer(fig, t, "  ·  sound off, picture checked identical in every variant")
    save(fig, "study-0-summary", mode, out)


# ---- 1. forest plot ---------------------------------------------------------------------------------------------------
def forest(mode):
    t = THEMES[mode]
    style(t)
    rows = [
        ("Game Boy · gambatte", "-flto", -3.2, 2.5, "flag"),
        ("Game Boy · gambatte", "-O3 (shipped: -O2)", -7.0, 3.8, "flag"),
        ("Game Boy · gambatte", "-O3 -flto", -9.0, 3.5, "lto"),
        ("Game Boy · gambatte", "-Ofast", -1.2, 3.9, "flag"),
        ("Game Boy · gambatte", "PGO + -O3 -flto (unseen ROMs)", -10.4, 2.2, "pgo"),
        ("SNES · snes9x", "-fno-lto", -1.1, 0.7, "flag"),
        ("SNES · snes9x", "-O2", +1.1, 1.2, "flag"),
        ("SNES · snes9x", "-Ofast", -1.4, 0.8, "flag"),
        ("SNES · snes9x", "PGO (unseen ROMs)", -3.5, 1.1, "pgo"),
        ("GBA · gpsp (32-bit)", "-fno-lto", +0.1, 2.8, "flag"),
        ("GBA · gpsp (32-bit)", "-O2 (shipped: -O3)", +5.2, 1.9, "flag"),
        ("GBA · gpsp (32-bit)", "-Ofast", -1.8, 2.7, "flag"),
        ("PlayStation · pcsx_rearmed", "PGO (7 games)", -1.1, 1.1, "pgo"),
    ]
    col = {"flag": t["flag"], "lto": t["lto"], "pgo": t["accent"]}
    fig = plt.figure(figsize=(9, 7.4), dpi=150)
    header(fig, t, "Compiler flags barely matter; PGO helps the C++ interpreters",
           "CPU time per frame against the shipped build, with 95 % confidence interval. Left of zero is faster.")
    ax = fig.add_axes([0.40, 0.095, 0.565, 0.77])
    card(ax, t)
    y, ys, labels, groups = 0.0, [], [], {}
    prev = None
    for g, lab, d, ci, kind in rows:
        if g != prev:
            y += 1.1 if prev is not None else 0.4
            groups[g] = y
            y += 0.9
            prev = g
        ax.errorbar(d, -y, xerr=ci, fmt="o", color=col[kind], ecolor=col[kind], elinewidth=2.2, capsize=0,
                    markersize=7.5, zorder=3)
        strong = kind != "flag"
        ax.text(max(d + ci, d) + 0.7, -y, f"{d:+.1f} %", va="center", fontsize=8.5,
                color=t["text"] if strong else t["muted"], fontweight="bold" if strong else "normal")
        ys.append(-y)
        labels.append(lab)
        y += 1
    ax.axvspan(-3, 3, color=t["grid"], alpha=0.35, lw=0, zorder=0)
    ax.axvline(0, color=t["muted"], lw=1, zorder=1)
    for xv in (-15, -10, -5, 5, 10):
        ax.axvline(xv, color=t["grid"], lw=0.6, zorder=0)
    ax.set_xlim(-16, 10)
    ax.set_ylim(-y + 0.3, 0.3)
    ax.set_yticks(ys)
    ax.set_yticklabels(labels, fontsize=9)
    ax.tick_params(axis="y", length=0, pad=10)
    ax.tick_params(axis="x", length=0, pad=6)
    ax.set_xticks([-15, -10, -5, 0, 5, 10])
    ax.set_xticklabels(["-15 %", "-10 %", "-5 %", "0", "+5 %", "+10 %"])
    for g, gy in groups.items():
        ax.text(-0.015, -gy, g, transform=ax.get_yaxis_transform(), ha="right", va="center", fontsize=9.5,
                fontweight="bold", color=t["text"], clip_on=False)
    legend(fig, t, [("compiler flags", col["flag"]), ("-O3 + LTO", col["lto"]),
                    ("profile-guided optimisation (PGO)", col["pgo"])], 0.04)
    fig.text(0.72, 0.04, "■", color=t["grid"], fontsize=11, va="center")
    fig.text(0.745, 0.04, "typical noise (±3 %)", color=t["muted"], fontsize=8.5, va="center")
    save(fig, "study-1-flags-and-pgo", mode, out)


# ---- 2. where the time goes -------------------------------------------------------------------------------------------
def breakdown(mode):
    t = THEMES[mode]
    style(t)
    cores = ["gambatte\nGame Boy", "snes9x\nSNES", "pcsx_rearmed\nPlayStation"]
    emu = [28, 57, 49.5]
    libmali = [29, 14, 18]
    kernel = [19, 11, 14]
    other = [100 - a - b - c for a, b, c in zip(emu, libmali, kernel)]
    dark_txt = "#10262b"
    parts = (("Emulated core (+ JIT)", emu, t["accent"], dark_txt),
             ("libmali: graphics driver, CPU side", libmali, t["lto"], "#3a2800"),
             ("Kernel + mali_kbase", kernel, t["pink"], "#ffffff" if mode == "light" else dark_txt),
             ("libc and the rest", other, t["other"], t["text"] if mode == "dark" else "#26262a"))
    fig = plt.figure(figsize=(9, 4.3), dpi=150)
    header(fig, t, "A big part of the cost is not the emulator",
           "Share of the CPU time of RetroArch + core (perf, 997 Hz, one run per core).")
    ax = fig.add_axes([0.2, 0.22, 0.765, 0.56])
    card(ax, t)
    left = [0.0, 0.0, 0.0]
    for name, vals, c, tc in parts:
        ax.barh(range(3), vals, left=left, color=c, height=0.52, edgecolor=t["card"], linewidth=1.5)
        for i, v in enumerate(vals):
            if v >= 8:
                ax.text(left[i] + v / 2, i, f"{v:.0f} %", ha="center", va="center", fontsize=10, fontweight="bold",
                        color=tc)
        left = [l + v for l, v in zip(left, vals)]
    ax.set_xlim(0, 100)
    ax.set_ylim(2.6, -0.6)
    ax.set_yticks(range(3))
    ax.set_yticklabels(cores, fontsize=9.5)
    ax.tick_params(axis="both", length=0, pad=8)
    ax.set_xticks([])
    legend(fig, t, [(n, c) for n, _, c, _ in parts], 0.12)
    fig.text(0.035, 0.05, "In gambatte, resampling the audio (11.9 %) costs twice what emulating the Game Boy CPU does (5.8 %).",
             fontsize=8, color=t["muted"])
    save(fig, "study-2-where-the-time-goes", mode, out)


# ---- 3. PSP ------------------------------------------------------------------------------------------------------------
def psp(mode):
    t = THEMES[mode]
    style(t)
    games = ["Daxter\n30 fps cap", "Tenchu 4\nno cap, 26 fps", "Tekken DR\n60 fps cap"]
    lto, lto_ci = [-0.4, -4.2, -0.7], [2.7, 1.8, 0.4]
    pgo, pgo_ci = [-2.3, -0.1, -2.2], [3.8, 2.0, 0.4]
    lto_fps, pgo_fps = [-0.4, 3.2, -0.2], [0.1, 0.1, -0.1]
    fig = plt.figure(figsize=(9, 4.4), dpi=150)
    header(fig, t, "PSP: only the game without a frame cap gains, and only a little",
           "PPSSPP 1.12.3, three games. The PGO profile was trained without the game being measured.")
    a1 = fig.add_axes([0.075, 0.2, 0.40, 0.55])
    a2 = fig.add_axes([0.575, 0.2, 0.40, 0.55])
    w = 0.34
    xs = range(3)
    for ax, ttl in ((a1, "CPU per frame (lower is better)"), (a2, "New frames per second (higher is better)")):
        card(ax, t)
        ax.set_title(ttl, loc="left", fontsize=9.5, color=t["text"], fontweight="bold", pad=10)
        ax.set_xticks(list(xs))
        ax.set_xticklabels(games, fontsize=8.5)
        ax.tick_params(length=0, pad=6)
        ax.axhline(0, color=t["muted"], lw=1)
        ax.yaxis.grid(True, color=t["grid"], lw=0.6)
        ax.set_axisbelow(True)
        ax.set_xlim(-0.6, 2.6)
    ek = dict(ecolor=t["text"], lw=1.2, capsize=3, capthick=1.2)
    a1.bar([x - w / 2 for x in xs], lto, w, yerr=lto_ci, color=t["lto"], error_kw=ek)
    a1.bar([x + w / 2 for x in xs], pgo, w, yerr=pgo_ci, color=t["accent"], error_kw=ek)
    a1.set_ylim(-7, 3)
    a1.set_yticks([-6, -4, -2, 0, 2])
    a1.set_yticklabels(["-6 %", "-4 %", "-2 %", "0", "+2 %"])
    a2.bar([x - w / 2 for x in xs], lto_fps, w, color=t["lto"])
    a2.bar([x + w / 2 for x in xs], pgo_fps, w, color=t["accent"])
    a2.set_ylim(-1, 4)
    a2.set_yticks([0, 1, 2, 3])
    a2.set_yticklabels(["0", "+1 %", "+2 %", "+3 %"])
    a2.text(1 - w / 2, 3.3, "+3.2 %", ha="center", fontsize=10, fontweight="bold", color=t["lto"])
    legend(fig, t, [("LTO", t["lto"]), ("PGO", t["accent"])], 0.075)
    fig.text(0.2, 0.075, "bars: 95 % confidence interval  ·  God of War left out (throttled at 84 °C, inconclusive)",
             fontsize=8, color=t["muted"], va="center")
    save(fig, "study-3-psp-lto-pgo", mode, out)


for m in THEMES:
    tiles(m)
    forest(m)
    breakdown(m)
    psp(m)
print("ok", FONT)
