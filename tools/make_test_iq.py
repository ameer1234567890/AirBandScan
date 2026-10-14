#!/usr/bin/env python3
"""Build a synthetic airband IQ capture (8-bit stereo WAV) for testing
AirBandScan without an SDR:  make_test_iq.py out.wav [speech1.wav ...]

Contains AM voice (from the given speech files), an ACARS-like data tone, an
unmodulated carrier, a noise-modulated carrier and a very short burst.
Needs numpy and ffmpeg (to read the speech files)."""
import subprocess, sys, wave
import numpy as np

FS = 1_024_000
CENTER = 120.0e6
DUR = 42.0
rng = np.random.default_rng(1)

def load(path, rate=16000):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(rate),
                          "-f", "f32le", "-"], capture_output=True, check=True).stdout
    a = np.frombuffer(raw, np.float32).astype(np.float64)
    return a / (np.max(np.abs(a)) + 1e-9)

def upsample(audio, rate):
    t_out = np.arange(int(len(audio) * FS / rate)) / FS
    return np.interp(t_out, np.arange(len(audio)) / rate, audio)

n = int(FS * DUR)
iq = (rng.normal(0, 0.02, n) + 1j * rng.normal(0, 0.02, n)).astype(np.complex64)

def add_am(freq, t0, audio, rate, amp, mod=0.8):
    m = upsample(audio, rate) if rate != FS else audio
    s = int(t0 * FS)
    m = m[: n - s]
    t = (np.arange(len(m)) + s) / FS
    carrier = np.exp(2j * np.pi * (freq - CENTER) * t)
    iq[s:s + len(m)] += (amp * (1.0 + mod * np.clip(m, -1, 1)) * carrier).astype(np.complex64)
    print(f"  {freq/1e6:.4f} MHz  t={t0:5.1f}s  {len(m)/FS:4.1f}s  amp={amp}")

speech = [load(p) for p in sys.argv[2:]]
if not speech:
    sys.exit("give at least one speech file")
gap = np.zeros(int(0.35 * 16000))

print("signals:")
add_am(119.700e6, 3.0, np.concatenate([speech[0], gap, speech[0][::-1][:8000]]) , 16000, 0.08)
seq = np.concatenate([x for s in speech[1:4] or speech for x in (s, gap)])
add_am(120.208333e6, 11.0, seq, 16000, 0.06)
# ACARS-like: 2400 bd MSK -> audio tones 1200/2400 Hz, constant envelope
bits = rng.integers(0, 2, int(4.0 * 2400))
f_inst = np.repeat(np.where(bits == 1, 1200.0, 2400.0), 16000 // 2400 + 1)[: int(4.0 * 16000)]
tone = np.sin(2 * np.pi * np.cumsum(f_inst) / 16000)
add_am(120.100e6, 5.0, tone, 16000, 0.07, mod=0.5)
add_am(120.350e6, 20.0, np.zeros(int(4.0 * 16000)), 16000, 0.07)          # dead carrier
add_am(119.900e6, 26.0, rng.normal(0, 0.3, int(0.25 * 16000)), 16000, 0.07)  # short burst
add_am(120.025e6, 28.0, speech[-1], 16000, 0.025)                           # weak voice
add_am(119.800e6, 34.0, np.clip(rng.normal(0, 0.4, int(3.0 * 16000)), -1, 1), 16000, 0.07)  # noise

pk = np.max(np.abs(np.concatenate([iq.real, iq.imag])))
print(f"peak {pk:.2f}")
u8 = np.empty(2 * n, np.uint8)
u8[0::2] = np.clip(np.round(iq.real * 127 + 128), 0, 255)
u8[1::2] = np.clip(np.round(iq.imag * 127 + 128), 0, 255)
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2); w.setsampwidth(1); w.setframerate(FS); w.writeframes(u8.tobytes())
print("wrote", sys.argv[1], f"({DUR:.0f} s @ {FS/1e6} MS/s, centre {CENTER/1e6} MHz)")
