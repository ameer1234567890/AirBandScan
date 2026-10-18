#!/usr/bin/env python3
"""Generate SSTV test transmissions (VIS header + image) for AirBandScan.

  python3 tools/make_test_sstv.py PD120 sstv.wav                  # audio WAV
  python3 tools/make_test_sstv.py PD120 sstv.wav --image pic.png  # your picture (needs Pillow)
  python3 tools/make_test_sstv.py PD180 iq.wav --iq --rate 1024000 --offset 250000

--iq writes a 16-bit stereo IQ WAV with the audio frequency-modulated onto a
carrier (+-deviation), with Doppler drift and noise: a stand-in for the ISS on
145.800 MHz. Without --iq the output is the receiver audio. --ref saves the
transmitted picture for comparison.
"""
import argparse
import wave

import numpy as np

# name: (vis, layout, width, height, sync ms, channel ms, line ms)
MODES = {
    "PD50": (93, "pd", 320, 256, 20.0, 91.520, 388.160),
    "PD90": (99, "pd", 320, 256, 20.0, 170.240, 703.040),
    "PD120": (95, "pd", 640, 496, 20.0, 121.600, 508.480),
    "PD160": (98, "pd", 512, 400, 20.0, 195.584, 804.416),
    "PD180": (96, "pd", 640, 496, 20.0, 183.040, 754.240),
    "PD240": (97, "pd", 640, 496, 20.0, 244.480, 1000.000),
    "PD290": (94, "pd", 800, 616, 20.0, 228.800, 937.280),
    "Robot36": (8, "r36", 320, 240, 9.0, 88.0, 150.0),
    "Robot72": (12, "r72", 320, 240, 9.0, 138.0, 300.0),
    "MartinM1": (44, "martin", 320, 256, 4.862, 146.432, 446.446),
    "MartinM2": (40, "martin", 320, 256, 4.862, 73.216, 226.798),
    "ScottieS1": (60, "scottie", 320, 256, 9.0, 138.240, 428.220),
    "ScottieS2": (56, "scottie", 320, 256, 9.0, 88.064, 277.692),
    "ScottieDX": (76, "scottie", 320, 256, 9.0, 345.600, 1050.300),
}


def test_pattern(w, h):
    y, x = np.mgrid[0:h, 0:w].astype(float)
    img = np.zeros((h, w, 3))
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
            (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
    top = y < h * 0.4
    for i, c in enumerate(bars):
        m = top & (x >= i * w / 8) & (x < (i + 1) * w / 8)
        img[m] = c
    mid = (y >= h * 0.4) & (y < h * 0.6)
    img[mid] = (x[mid] / w * 255)[:, None]
    bot = y >= h * 0.6
    cx, cy = w / 2, h * 0.8
    r = np.hypot(x - cx, (y - cy) * 1.0)
    rings = (np.sin(r / 6.0) > 0) * 255.0
    img[bot] = np.stack([rings[bot], (x[bot] / w * 255), (y[bot] / h * 255)], -1)
    return img.clip(0, 255)


def to_ycc(img):
    R, G, B = img[..., 0], img[..., 1], img[..., 2]
    Y = 0.299 * R + 0.587 * G + 0.114 * B
    Cr = 128 + 0.5 * R - 0.418688 * G - 0.081312 * B
    Cb = 128 - 0.168736 * R - 0.331264 * G + 0.5 * B
    return Y.clip(0, 255), Cr.clip(0, 255), Cb.clip(0, 255)


class Tx:
    def __init__(self, rate, clock):
        self.rate, self.clock = rate, clock
        self.t = 0.0          # transmitter time (s)
        self.f = []           # (end time, freq) segments

    def tone(self, freq, ms):
        self.t += ms / 1000.0 * self.clock
        self.f.append((self.t, freq))

    def pixels(self, vals, ms):
        step = ms / len(vals)
        for v in vals:
            self.tone(1500.0 + v * 800.0 / 255.0, step)

    def render(self):
        ends = np.array([e for e, _ in self.f])
        freqs = np.array([f for _, f in self.f])
        n = int(ends[-1] * self.rate)
        t = np.arange(n) / self.rate
        idx = np.searchsorted(ends, t, side="right").clip(0, len(freqs) - 1)
        inst = freqs[idx]
        return np.sin(2 * np.pi * np.cumsum(inst) / self.rate), inst


def encode(mode, img, rate, clock):
    vis, lay, w, h, sync, ch, line = MODES[mode]
    tx = Tx(rate, clock)
    tx.tone(1900, 300); tx.tone(1200, 10); tx.tone(1900, 300); tx.tone(1200, 30)
    ones = 0
    for b in range(7):
        bit = (vis >> b) & 1
        ones += bit
        tx.tone(1100 if bit else 1300, 30)
    tx.tone(1100 if ones & 1 else 1300, 30)  # even parity
    tx.tone(1200, 30)
    Y, Cr, Cb = to_ycc(img)
    R, G, B = img[..., 0], img[..., 1], img[..., 2]
    if lay == "pd":
        for n in range(h // 2):
            tx.tone(1200, sync); tx.tone(1500, 2.08)
            y0, y1 = 2 * n, 2 * n + 1
            tx.pixels(Y[y0], ch)
            tx.pixels((Cr[y0] + Cr[y1]) / 2, ch)
            tx.pixels((Cb[y0] + Cb[y1]) / 2, ch)
            tx.pixels(Y[y1], ch)
    elif lay == "martin":
        for n in range(h):
            tx.tone(1200, sync); tx.tone(1500, 0.572)
            tx.pixels(G[n], ch); tx.tone(1500, 0.572)
            tx.pixels(B[n], ch); tx.tone(1500, 0.572)
            tx.pixels(R[n], ch); tx.tone(1500, 0.572)
    elif lay == "scottie":
        tx.tone(1200, 9.0)
        for n in range(h):
            tx.tone(1500, 1.5); tx.pixels(G[n], ch)
            tx.tone(1500, 1.5); tx.pixels(B[n], ch)
            tx.tone(1200, 9.0); tx.tone(1500, 1.5); tx.pixels(R[n], ch)
    elif lay == "r36":
        for n in range(h):
            tx.tone(1200, 9.0); tx.tone(1500, 3.0)
            tx.pixels(Y[n], 88.0)
            if n % 2 == 0:
                tx.tone(1500, 4.5); tx.tone(1900, 1.5)
                tx.pixels((Cr[n] + Cr[min(n + 1, h - 1)]) / 2, 44.0)
            else:
                tx.tone(2300, 4.5); tx.tone(1900, 1.5)
                tx.pixels((Cb[n] + Cb[n - 1]) / 2, 44.0)
    elif lay == "r72":
        for n in range(h):
            tx.tone(1200, 9.0); tx.tone(1500, 3.0)
            tx.pixels(Y[n], 138.0)
            tx.tone(1500, 4.5); tx.tone(1900, 1.5); tx.pixels(Cr[n], 69.0)
            tx.tone(2300, 4.5); tx.tone(1900, 1.5); tx.pixels(Cb[n], 69.0)
    return tx.render()


def write_wav(path, rate, data, channels):
    pcm = np.clip(data * 32767, -32768, 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(int(rate))
        w.writeframes(pcm.tobytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=sorted(MODES))
    ap.add_argument("out")
    ap.add_argument("--image", help="picture to send (resized; needs Pillow)")
    ap.add_argument("--ref", help="save the transmitted picture (PNG, needs Pillow)")
    ap.add_argument("--audio-rate", type=float, default=11025.0)
    ap.add_argument("--clock-ppm", type=float, default=300.0, help="transmitter clock error (slant)")
    ap.add_argument("--snr", type=float, default=30.0, help="audio SNR in dB (audio output only)")
    ap.add_argument("--lead", type=float, default=2.0, help="silence/noise before and after (s)")
    ap.add_argument("--iq", action="store_true", help="write FM-modulated IQ instead of audio")
    ap.add_argument("--rate", type=float, default=250000.0, help="IQ sample rate")
    ap.add_argument("--offset", type=float, default=0.0, help="carrier offset from the centre (Hz)")
    ap.add_argument("--deviation", type=float, default=3500.0, help="FM deviation (Hz)")
    ap.add_argument("--doppler", type=float, default=3000.0, help="Doppler swing +-Hz (ISS: ~3.5 kHz)")
    ap.add_argument("--cnr", type=float, default=20.0, help="carrier to noise in 25 kHz (dB) for --iq")
    a = ap.parse_args()

    vis, lay, w, h = MODES[a.mode][:4]
    if a.image:
        from PIL import Image
        img = np.asarray(Image.open(a.image).convert("RGB").resize((w, h)), dtype=float)
    else:
        img = test_pattern(w, h)
    if a.ref:
        from PIL import Image
        Image.fromarray(img.astype(np.uint8)).save(a.ref)

    clock = 1.0 + a.clock_ppm * 1e-6
    arate = a.rate if a.iq else a.audio_rate
    audio, inst = encode(a.mode, img, arate, clock)
    lead = np.zeros(int(a.lead * arate))
    rng = np.random.default_rng(5)
    if not a.iq:
        sig = np.concatenate([lead, audio, lead]) * 0.5
        sig += rng.standard_normal(len(sig)) * 0.5 / np.sqrt(2) * 10 ** (-a.snr / 20)
        write_wav(a.out, arate, sig, 1)
        print(f"{a.out}: {a.mode} {w}x{h}, {len(sig) / arate:.1f} s audio at {arate:.0f} Hz")
        return

    # FM: the instantaneous audio frequency drives nothing directly; the audio
    # waveform itself modulates the carrier (+-deviation at full scale).
    msg = np.concatenate([lead, audio, lead])
    n = len(msg)
    t = np.arange(n) / a.rate
    dur = t[-1]
    dop = -a.doppler * np.tanh((t - dur / 2) / (a.doppler / 25.0))  # ~25 Hz/s at the centre
    finst = a.offset + dop + a.deviation * msg
    sig = np.exp(2j * np.pi * np.cumsum(finst) / a.rate)
    noise_bw = 25000.0
    n0 = 10 ** (-a.cnr / 10) / noise_bw * a.rate  # per-sample noise power
    sig += np.sqrt(n0 / 2) * (rng.standard_normal(n) + 1j * rng.standard_normal(n))
    sig *= 0.3 / np.sqrt(np.mean(np.abs(sig) ** 2))
    iq = np.empty(2 * n)
    iq[0::2], iq[1::2] = sig.real, sig.imag
    write_wav(a.out, a.rate, iq, 2)
    print(f"{a.out}: {a.mode} {w}x{h}, {n / a.rate:.1f} s IQ at {a.rate:.0f} S/s, "
          f"offset {a.offset:.0f} Hz, Doppler +-{a.doppler:.0f} Hz, C/N {a.cnr} dB")


if __name__ == "__main__":
    main()
