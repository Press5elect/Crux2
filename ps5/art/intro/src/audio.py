#!/usr/bin/env python3
# Crux2 - the startup video (ps5/art/intro, README.md there): its sound, synthesized on main.js's
# timeline (T): a warm low drone; a traced hum while the guides draw; two slides that end in a
# mechanical clack and a muted note each; the orange bar's rising charge, its latch and a chord; a run
# of bells along it; four ticks and four rising notes as the letters land; the chord opening out as the
# light crosses the name, and fading with the picture. All synthesized here (NumPy), nothing sampled.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
#
#   OUT=audio.wav python3 audio.py
import os
import wave

import numpy as np

# main.js's T (the letters land at l0 + i * LET + LDUR)
T = dict(guide=0.2, a0=0.5, a1=0.82, b0=0.92, b1=1.24, o0=1.5, o1=1.96, run=2.06, move=2.7, move1=3.2,
         l0=3.02, LET=0.13, LDUR=0.36, sweep=4.0, sweep1=4.6, fade=5.9, end=6.6)
LAND = [T["l0"] + i * T["LET"] + T["LDUR"] for i in range(4)]
SR = 48000
N = int(round(T["end"] * SR))
rng = np.random.default_rng(2026111502)
t = np.arange(N) / SR
L = np.zeros(N); R = np.zeros(N)        # dry
WL = np.zeros(N); WR = np.zeros(N)      # to the room


def hz(note):                           # semitones from A4
    return 440.0 * 2 ** (note / 12)


D2, A2, D3, E3, Fs3, A3, B3, D4, E4, Fs4, A4, B4, Cs5, D5, E5, Fs5, A5, B5, D6, E6 = (
    hz(n) for n in (-31, -24, -19, -17, -15, -12, -10, -7, -5, -3, 0, 2, 4, 5, 7, 9, 12, 14, 17, 19))


def tt(d):
    return np.arange(int(d * SR)) / SR


def add(sig, t0, pan=0.0, gain=1.0, wet=0.0):
    """Mix sig in at t0 (s), panned (-1 left .. 1 right), with some of it sent to the room."""
    i = int(round(t0 * SR))
    if i < 0:
        sig, i = sig[-i:], 0
    n = min(len(sig), N - i)
    if n <= 0:
        return
    s = sig[:n] * gain
    if np.ndim(pan) == 0:
        gl, gr = np.cos((pan + 1) * np.pi / 4), np.sin((pan + 1) * np.pi / 4)
    else:
        p = np.asarray(pan)[:n]; gl, gr = np.cos((p + 1) * np.pi / 4), np.sin((p + 1) * np.pi / 4)
    L[i:i + n] += s * gl; R[i:i + n] += s * gr
    if wet:
        WL[i:i + n] += s * gl * wet; WR[i:i + n] += s * gr * wet


def band(x, lo, hi, order=2):
    """Filter x (FFT, smooth band edges)."""
    n = len(x); X = np.fft.rfft(x); f = np.fft.rfftfreq(n, 1 / SR)
    m = 1 / np.sqrt(1 + (lo / np.maximum(f, 1e-3)) ** (2 * order)) / np.sqrt(1 + (f / hi) ** (2 * order))
    return np.fft.irfft(X * m, n)


def norm(x):
    return x / (np.abs(x).max() + 1e-12)


def env(d, a, r, shape=1.0):
    """Attack a, then release over the rest of d."""
    x = tt(d); e = np.minimum(x / max(a, 1e-4), 1) * np.clip(1 - (x - a) / max(d - a, 1e-4), 0, 1) ** shape
    return e


def saw(f, d, voices=3, spread=0.006):
    x = tt(d); out = np.zeros(len(x))
    for k in range(voices):
        ph = rng.uniform(0, 1); fk = f * (1 + spread * (k - (voices - 1) / 2))
        out += 2 * ((fk * x + ph) % 1) - 1
    return out / voices


def lowpass_sweep(x, f0, f1):
    """A one-pole low-pass whose corner moves from f0 to f1 (log) over x."""
    fc = np.exp(np.linspace(np.log(f0), np.log(f1), len(x)))
    a = np.exp(-2 * np.pi * fc / SR); y = np.zeros(len(x)); z = 0.0
    for i in range(len(x)):
        z = (1 - a[i]) * x[i] + a[i] * z; y[i] = z
    return y


def pluck(f, d, bright=1.0):
    """A soft, muted synth pluck: a few harmonics that die fast, the higher ones faster."""
    x = tt(d); out = np.zeros(len(x))
    for k, g in ((1, 1.0), (2, 0.45 * bright), (3, 0.22 * bright), (4, 0.10 * bright), (5, 0.05 * bright)):
        out += g * np.sin(2 * np.pi * f * k * x + rng.uniform(0, 6.28)) * np.exp(-x * (5 + 6 * k))
    return out * np.minimum(x / 0.002, 1)


def bell(f, d, dec):
    """A small FM bell (two operators, the modulator's index dying away)."""
    x = tt(d); idx = 1.6 * np.exp(-x / (dec * 0.25))
    s = np.sin(2 * np.pi * f * x + idx * np.sin(2 * np.pi * f * 3.5 * x))
    return s * np.exp(-x / dec) * np.minimum(x / 0.003, 1)


def clack(size=1.0, tone=1.0):
    """A part hitting its stop: a click, a short metal ring (inharmonic), a body thump."""
    d = 0.35; x = tt(d)
    click = band(rng.standard_normal(len(x)), 2500, 12000) * np.exp(-x / 0.0018)
    ring = sum(g * np.sin(2 * np.pi * f * tone * x + rng.uniform(0, 6.28)) * np.exp(-x / dd)
               for f, g, dd in ((1730, 0.5, 0.045), (2610, 0.35, 0.03), (3870, 0.25, 0.02), (5320, 0.15, 0.012)))
    body = np.sin(2 * np.pi * (150 * x - 60 * 0.03 * (1 - np.exp(-x / 0.03)))) * np.exp(-x / (0.05 * size))
    return norm(click) * 0.9 + ring * 0.6 + body * 0.9 * size


def whoosh(d, f0, f1, peak=0.8):
    """Air: band-passed noise whose band rises from f0 to f1 and swells to `peak` of d, then stops short."""
    n = int(d * SR); x = rng.standard_normal(n)
    hi = band(x, 300, 9000, 1)
    y = lowpass_sweep(hi, f0, f1)
    u = np.linspace(0, 1, n); e = (u / peak) ** 2 * (u < peak) + (u >= peak) * np.exp(-(u - peak) / (1 - peak) * 4)
    return norm(y) * e


# ---- the drone: D under everything, an A over it from the second bar on; a slow beat between voices
dr = (np.sin(2 * np.pi * D2 * t) * 0.55 + np.sin(2 * np.pi * D2 * 1.003 * t + 1) * 0.45 + 0.25 * np.sin(2 * np.pi * D3 * t + 2))
dr += 0.35 * np.sin(2 * np.pi * A2 * t + 0.5) * np.clip((t - T["b1"]) / 0.6, 0, 1)
dr_env = np.clip(t / 1.0, 0, 1) ** 2 * (0.55 + 0.45 * np.clip((t - T["o0"]) / (T["o1"] - T["o0"]), 0, 1))
dr_env *= 1 - 0.5 * np.clip((t - T["o1"]) / 0.8, 0, 1)                   # the chord takes over
add(dr * dr_env, 0, 0, 0.16, 0.2)

# ---- the guides draw: a thin traced hum and a hiss
d = 0.6; x = tt(d)
hum = np.sin(2 * np.pi * (1800 * x + 600 * x * x)) * 0.5 + np.sin(2 * np.pi * (2700 * x + 900 * x * x)) * 0.25
hum *= np.sin(np.pi * np.clip(x / d, 0, 1)) ** 2 * (0.8 + 0.2 * np.sin(2 * np.pi * 31 * x))
add(hum, T["guide"], 0.0, 0.035, 0.6)
add(norm(band(rng.standard_normal(len(x)), 4000, 12000)) * np.sin(np.pi * x / d) ** 3, T["guide"], 0, 0.03, 0.5)

# ---- the cream bars: each slides in (from the top left, then the bottom right) and clacks home
for t0, t1, p0, note, tone in ((T["a0"], T["a1"], -0.7, A3, 1.0), (T["b0"], T["b1"], 0.7, D4, 1.12)):
    d = t1 - t0 + 0.02
    w = whoosh(d, 900, 6000, peak=0.92)
    add(w, t0, np.linspace(p0, p0 * 0.2, len(w)), 0.22, 0.3)
    add(clack(1.0, tone), t1, p0 * 0.25, 0.42, 0.35)
    add(pluck(note, 1.2, 0.8), t1, p0 * 0.2, 0.22, 0.6)

# ---- the orange bar: a rising charge on its way in, then the latch ("ka-chunk"), a deep thump, the chord
d = T["o1"] - T["o0"] + 0.01; x = tt(d)
f = D3 * 2 ** (np.clip(x / d, 0, 1) ** 1.6 * 12 / 12)                 # D3 up to D4
ph = 2 * np.pi * np.cumsum(f) / SR
ch = (2 * ((ph / (2 * np.pi)) % 1) - 1) * 0.6 + np.sin(ph * 0.5) * 0.5
ch = lowpass_sweep(ch, 300, 4500) * (x / d) ** 1.5
add(ch, T["o0"], np.linspace(-0.8, 0.0, len(x)), 0.20, 0.4)
add(whoosh(d, 600, 9000, peak=0.95), T["o0"], np.linspace(-0.8, 0, len(x)), 0.30, 0.3)
add(clack(1.8, 0.8), T["o1"] - 0.012, 0, 0.45, 0.35)
add(clack(1.4, 0.68), T["o1"] + 0.035, 0, 0.35, 0.4)
x = tt(1.2)
thump = np.sin(2 * np.pi * (42 * x + (95 - 42) * 0.06 * (1 - np.exp(-x / 0.06)))) * np.exp(-x / 0.32) * np.minimum(x / 0.002, 1)
add(np.tanh(thump * 1.6), T["o1"], 0, 0.75, 0.15)
stab = sum(saw(fq, 2.4, 3, 0.005) * g for fq, g in ((D3, 0.8), (A3, 0.6), (D4, 0.5), (Fs4, 0.4), (A4, 0.3)))
x = tt(2.4)
stab = lowpass_sweep(stab, 5000, 500) * np.exp(-x / 0.7) * np.minimum(x / 0.004, 1)
add(stab, T["o1"], 0, 0.17, 0.9)

# ---- the light runs along the bar: bells, low left to high right
for k, fq in enumerate((D5, Fs5, A5, D6, E6)):
    add(bell(fq, 1.4, 0.45), T["run"] + 0.085 * k, -0.6 + 0.3 * k, 0.05 * (1 - 0.08 * k), 0.8)

# ---- the mark draws back to its place: a short falling breath of air
w = whoosh(0.5, 5000, 700, peak=0.5); add(w[::-1] * 0.6 + w * 0.4, T["move"], 0.15, 0.10, 0.4)

# ---- the letters: each slips out (a tiny air) and lands with a tick and a note, rising
for i, (tl, fq) in enumerate(zip(LAND, (A4, B4, D5, Fs5))):
    pan = (-0.55, -0.35, -0.12, 0.45)[i]
    add(whoosh(T["LDUR"], 2000, 8000, peak=0.85), tl - T["LDUR"], pan, 0.06, 0.2)
    add(clack(0.45, 1.5 + 0.08 * i), tl, pan, 0.26, 0.3)
    add(pluck(fq, 1.4, 1.0) + 0.3 * pluck(fq * 2, 1.4, 0.5), tl, pan, 0.20 if i < 3 else 0.26, 0.7)

# ---- the light crosses the name: the chord opens out (D, A, E over it), a shimmer from left to right
d = T["end"] - T["sweep"] + 0.1; x = tt(d)
pad = sum(saw(fq, d, 4, 0.004) * g for fq, g in ((D2, 0.7), (A2, 0.6), (D3, 0.5), (Fs3, 0.45), (A3, 0.4), (E4, 0.3), (A4, 0.18)))
pad = lowpass_sweep(pad, 400, 2600) * np.minimum(x / 0.35, 1) ** 1.5 * np.exp(-x / 3.0)
add(pad, T["sweep"], 0, 0.22, 0.7)
sh = norm(band(rng.standard_normal(int(0.8 * SR)), 5000, 14000)) * np.sin(np.pi * np.linspace(0, 1, int(0.8 * SR))) ** 2
add(sh, T["sweep"] - 0.05, np.linspace(-0.8, 0.8, len(sh)), 0.035, 0.5)
for fq, dt, p, g in ((D5, 0.0, -0.3, 0.08), (A5, 0.12, 0.1, 0.065), (E6, 0.24, 0.4, 0.045)):
    add(bell(fq, 2.4, 0.9), T["sweep"] + 0.05 + dt, p, g, 0.9)

# ---- the room, the fade, the level
irn = int(2.2 * SR); ti = np.arange(irn) / SR


def ir(seed):
    r = np.random.default_rng(seed); y = band(r.standard_normal(irn), 200, 7000, 1) * (np.exp(-ti / 0.5) * 0.8 + np.exp(-ti / 0.1) * 0.5)
    pre = int(0.018 * SR); y = np.concatenate([np.zeros(pre), y[:-pre]])
    return y / np.sqrt((y ** 2).sum())


def conv(a, h):
    n = len(a) + len(h); m = 1 << int(np.ceil(np.log2(n)))
    return np.fft.irfft(np.fft.rfft(a, m) * np.fft.rfft(h, m), m)[:len(a)]


ML = L + conv(WL, ir(1)) * 0.8
MR = R + conv(WR, ir(2)) * 0.8
fade = np.clip((T["end"] - 0.03 - t) / (T["end"] - 0.03 - T["fade"]), 0, 1) ** 1.6 * np.minimum(t / 0.02, 1)
ML *= fade; MR *= fade
pk = max(np.abs(ML).max(), np.abs(MR).max())
ML = np.tanh(ML / pk * 2.2) / np.tanh(2.2); MR = np.tanh(MR / pk * 2.2) / np.tanh(2.2)
pk = max(np.abs(ML).max(), np.abs(MR).max()); ML *= 0.89 / pk; MR *= 0.89 / pk       # -1 dBFS
out = np.stack([ML, MR], 1)
OUT = os.environ.get("OUT", "audio.wav")
with wave.open(OUT, "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes(np.round(out * 32767).astype(np.int16).tobytes())
blk = int(SR * 0.1)
print(OUT, f"{N / SR:.2f} s;", "level per 0.1 s (dB + 60):",
      " ".join("%2d" % int(max(-60, 20 * np.log10(np.sqrt((out[i:i + blk] ** 2).mean()) + 1e-6)) + 60) for i in range(0, N, blk)))
