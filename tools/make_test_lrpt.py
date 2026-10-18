#!/usr/bin/env python3
"""Build a Meteor-M LRPT test baseband from decoded CADUs (e.g. a SatDump
.cadu file): randomise, NRZ-M, rate 1/2 K=7 convolutional code, (O)QPSK with
an RRC(0.6) pulse at 72 ksym/s, then Doppler, noise and a clock error.

  python3 tools/make_test_lrpt.py pass.cadu test_lrpt.wav
  python3 tools/make_test_lrpt.py pass.cadu iq.wav --rate 1024000 --offset 250000

Output is a 16-bit stereo IQ WAV. With --offset the signal sits that far from
the centre (for the receiver's own tuning offset), otherwise it is centred.
"""
import argparse
import wave

import numpy as np


def pn_sequence(n=1020):
    sr, out = 0xFF, []
    for _ in range(n):
        byte = 0
        for _ in range(8):
            bit = sr & 1
            byte = (byte << 1) | bit
            fb = (sr ^ (sr >> 3) ^ (sr >> 5) ^ (sr >> 7)) & 1
            sr = (sr >> 1) | (fb << 7)
        out.append(byte)
    return np.array(out, dtype=np.uint8)


def conv_encode(bits):
    # G1 = 0x4F, G2 = 0x6D on a 7-bit register with the newest bit in the LSB.
    n = len(bits)
    reg = np.zeros(n, dtype=np.int64)
    padded = np.concatenate([np.zeros(6, dtype=np.int64), bits.astype(np.int64)])
    for k in range(7):  # register bit k = input bit from k steps ago
        reg |= padded[6 - k:6 - k + n] << k
    def parity(x):
        x = x.copy(); x ^= x >> 4; x ^= x >> 2; x ^= x >> 1
        return x & 1
    return parity(reg & 0x4F).astype(np.uint8), parity(reg & 0x6D).astype(np.uint8)


def rrc(alpha, span, sps):
    t = np.arange(-span * sps, span * sps + 1) / sps
    h = np.zeros_like(t)
    for i, x in enumerate(t):
        if abs(x) < 1e-9:
            h[i] = 1 - alpha + 4 * alpha / np.pi
        elif abs(abs(x) - 1 / (4 * alpha)) < 1e-9:
            h[i] = alpha / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * alpha)) +
                                         (1 - 2 / np.pi) * np.cos(np.pi / (4 * alpha)))
        else:
            h[i] = (np.sin(np.pi * x * (1 - alpha)) + 4 * alpha * x * np.cos(np.pi * x * (1 + alpha))) / \
                   (np.pi * x * (1 - (4 * alpha * x) ** 2))
    return h / np.sqrt((h ** 2).sum())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cadu")
    ap.add_argument("out")
    ap.add_argument("--frames", type=int, default=400)
    ap.add_argument("--rate", type=float, default=250000.0)
    ap.add_argument("--offset", type=float, default=0.0, help="signal offset from the centre (Hz)")
    ap.add_argument("--doppler", type=float, default=3000.0, help="Doppler swing +-Hz (pass peak)")
    ap.add_argument("--doppler-rate", type=float, default=40.0,
                    help="max Doppler rate in Hz/s at the pass centre (real Meteor passes: ~30-40)")
    ap.add_argument("--snr", type=float, default=9.0, help="Es/N0 in dB")
    ap.add_argument("--ppm", type=float, default=25.0, help="symbol clock error")
    ap.add_argument("--qpsk", action="store_true", help="old Meteor-M N2 mode (QPSK, no NRZ-M)")
    ap.add_argument("--lead", type=float, default=2.0, help="seconds of noise before the signal")
    a = ap.parse_args()

    raw = np.fromfile(a.cadu, dtype=np.uint8)
    nf = min(a.frames, len(raw) // 1024)
    frames = raw[:nf * 1024].reshape(nf, 1024).copy()
    frames[:, 4:] ^= pn_sequence()
    bits = np.unpackbits(frames.reshape(-1)).astype(np.uint8)
    if not a.qpsk:
        bits = np.bitwise_xor.accumulate(bits)  # NRZ-M: a 1 toggles the level
    c0, c1 = conv_encode(bits)
    sym_i = 1.0 - 2.0 * c0
    sym_q = 1.0 - 2.0 * c1

    rs = 72000.0 * (1 + a.ppm * 1e-6)
    os_ = 8  # build at 8 samples per symbol, then resample to the output rate
    h = rrc(0.6, 8, os_)
    n = len(sym_i) * os_
    ui = np.zeros(n); ui[::os_] = sym_i
    uq = np.zeros(n); uq[::os_] = sym_q
    if not a.qpsk:
        uq = np.roll(uq, os_ // 2)  # OQPSK: Q half a symbol late
    sig = np.convolve(ui, h, "same") + 1j * np.convolve(uq, h, "same")
    t_src = np.arange(n) / (rs * os_)
    dur = t_src[-1]
    t_out = np.arange(int(dur * a.rate)) / a.rate
    sig = np.interp(t_out, t_src, sig.real) + 1j * np.interp(t_out, t_src, sig.imag)

    # Doppler around the time of closest approach (file centre): -D tanh(t / tau).
    tau = a.doppler / a.doppler_rate if a.doppler_rate > 0 else 1e9
    f = a.offset - a.doppler * np.tanh((t_out - dur / 2) / tau)
    sig *= np.exp(2j * np.pi * np.cumsum(f) / a.rate)

    lead = int(a.lead * a.rate)
    sig = np.concatenate([np.zeros(lead, complex), sig, np.zeros(lead // 2, complex)])
    p = np.mean(np.abs(sig[lead:-lead // 2]) ** 2)
    sps = a.rate / 72000.0
    # Es/N0 -> noise power in the full sample bandwidth.
    n0 = p * sps / (10 ** (a.snr / 10))
    rng = np.random.default_rng(7)
    sig += np.sqrt(n0 / 2) * (rng.standard_normal(len(sig)) + 1j * rng.standard_normal(len(sig)))
    sig *= 0.25 / np.sqrt(np.mean(np.abs(sig) ** 2))

    pcm = np.empty(2 * len(sig), dtype=np.int16)
    pcm[0::2] = np.clip(sig.real * 32767, -32768, 32767)
    pcm[1::2] = np.clip(sig.imag * 32767, -32768, 32767)
    with wave.open(a.out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(int(a.rate))
        w.writeframes(pcm.tobytes())
    print(f"{a.out}: {nf} frames, {len(sig) / a.rate:.1f} s at {a.rate:.0f} S/s, "
          f"{'QPSK' if a.qpsk else 'OQPSK + NRZ-M'}, Es/N0 {a.snr} dB, Doppler +-{a.doppler} Hz")


if __name__ == "__main__":
    main()
