"""Sonidos de navegacion originales de Zenith (sintetizados, sin licencias de terceros).
Dos juegos: A = sutil/minimalista, B = mas 'consola'. WAV 48 kHz 16 bit mono, con fundidos de pocos ms (sin clics).
Uso: python mk_sounds.py <carpeta-salida>
"""
import math
import os
import struct
import sys
import wave

SR = 48000
OUT = sys.argv[1]


def env(n, a_ms, r_ms, curve=1.0):
    """Envolvente: ataque lineal corto + caida exponencial hasta 0 (acaba en 0 exacto)."""
    a = max(1, int(SR * a_ms / 1000))
    out = []
    for i in range(n):
        if i < a:
            e = i / a
        else:
            t = (i - a) / max(1, n - a)
            e = math.exp(-5.0 * curve * t) * (1 - t) ** 0.5
        out.append(e)
    return out


def tone(freq, ms, vol, a_ms=3, curve=1.0, harm=(1.0,), sweep=None):
    n = int(SR * ms / 1000)
    e = env(n, a_ms, ms, curve)
    s = []
    ph = 0.0
    for i in range(n):
        f = freq if sweep is None else freq * (sweep / freq) ** (i / n)
        ph += 2 * math.pi * f / SR
        v = sum(h * math.sin(ph * (k + 1)) for k, h in enumerate(harm)) / sum(harm)
        s.append(v * e[i] * vol)
    return s


def seq(parts, gap_ms=0):
    out = []
    for p in parts:
        out += p + [0.0] * int(SR * gap_ms / 1000)
    return out


def mix(a, b, delay_ms=0):
    d = int(SR * delay_ms / 1000)
    n = max(len(a), len(b) + d)
    out = [0.0] * n
    for i, v in enumerate(a):
        out[i] += v
    for i, v in enumerate(b):
        out[i + d] += v
    return out


def save(name, samples, peak):
    m = max(abs(v) for v in samples) or 1.0
    k = peak / m
    path = os.path.join(OUT, name + '.wav')
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b''.join(struct.pack('<h', int(max(-1, min(1, v * k)) * 32767)) for v in samples))
    return path


N = {'C5': 523.25, 'E5': 659.25, 'G5': 783.99, 'C6': 1046.5, 'A4': 440.0, 'G4': 392.0, 'D5': 587.33, 'B4': 493.88,
     'E6': 1318.5, 'F5': 698.46}

for setname in ('A', 'B'):
    d = os.path.join(OUT, 'set' + setname)
    os.makedirs(d, exist_ok=True)
    OUTD = OUT
    if setname == 'A':     # sutil: seno puro, notas altas y cortas, volumen bajo
        scroll = tone(2000, 22, 1.0, a_ms=2, curve=1.6)
        back = seq([tone(N['G5'], 55, 1.0, curve=1.2), tone(N['D5'], 80, 0.9, curve=1.0)], 0)
        menu = seq([tone(N['D5'], 55, 0.9, curve=1.2), tone(N['G5'], 90, 1.0, curve=1.0)], 0)
        launch = mix(tone(N['C5'], 320, 0.8, a_ms=8, curve=0.8, sweep=N['C6']), tone(N['G5'], 300, 0.45, a_ms=12, curve=0.9, sweep=N['E6']), 30)
        peaks = dict(scroll=0.22, back=0.32, menuOpen=0.32, launch=0.42)
    else:                  # consola: mas armonicos (timbre tipo chip), arpegios cortos
        h = (1.0, 0.0, 0.33, 0.0, 0.2)       # onda tipo cuadrada suave
        scroll = tone(1300, 30, 1.0, a_ms=2, curve=1.3, harm=h)
        back = seq([tone(N['E5'], 45, 1.0, curve=1.0, harm=h), tone(N['C5'], 45, 1.0, curve=1.0, harm=h), tone(N['G4'], 80, 1.0, curve=1.1, harm=h)], 0)
        menu = seq([tone(N['G5'], 50, 1.0, curve=1.0, harm=h), tone(N['C6'], 95, 1.0, curve=1.1, harm=h)], 0)
        launch = seq([tone(N['C5'], 70, 1.0, curve=0.9, harm=h), tone(N['E5'], 70, 1.0, curve=0.9, harm=h),
                      tone(N['G5'], 70, 1.0, curve=0.9, harm=h), tone(N['C6'], 220, 1.0, curve=1.0, harm=h)], 0)
        peaks = dict(scroll=0.22, back=0.30, menuOpen=0.30, launch=0.40)
    for nm, smp in (('scroll', scroll), ('back', back), ('menuOpen', menu), ('launch', launch)):
        p = save(nm, smp, peaks[nm])
        os.replace(p, os.path.join(d, nm + '.wav'))
    # demo encadenada para escuchar el juego entero de una vez
    demo = []
    for nm in ('scroll', 'scroll', 'scroll', 'menuOpen', 'back', 'launch'):
        with wave.open(os.path.join(d, nm + '.wav')) as w:
            raw = w.readframes(w.getnframes())
        demo += [x / 32767 for x in struct.unpack('<%dh' % (len(raw) // 2), raw)] + [0.0] * int(SR * (0.18 if nm == 'scroll' else 0.45))
    with wave.open(os.path.join(OUT, 'demo-%s.wav' % setname), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b''.join(struct.pack('<h', int(v * 32767)) for v in demo))
    print('juego', setname, 'ok')
