"""Measure the offset between two speakers playing the same click track.

Records nothing itself: analyses a mono WAV. Each click arrives twice (once per speaker); the
autocorrelation of the onset-strength signal then peaks at the gap between the two arrivals.
Also lists per-click double-onset gaps as a sanity check.
"""
import sys
import wave

import numpy as np


def load(path):
    w = wave.open(path)
    rate = w.getframerate()
    a = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float64)
    return a, rate


def onset_strength(a, rate):
    # High-pass (first difference) to emphasise click transients, rectify, smooth ~0.3 ms.
    hp = np.diff(a, prepend=a[0])
    env = np.abs(hp)
    k = max(1, int(rate * 0.0003))
    env = np.convolve(env, np.ones(k) / k, mode="same")
    # Onset = positive slope of the envelope.
    d = np.diff(env, prepend=env[0])
    d[d < 0] = 0
    return d


def main(path):
    a, rate = load(path)
    o = onset_strength(a, rate)
    o = o - o.mean()
    n = len(o)
    f = np.fft.rfft(o, 2 * n)
    ac = np.fft.irfft(f * np.conj(f))[:n]
    ac /= ac[0]
    lo, hi = int(rate * 0.002), int(rate * 0.150)
    seg = ac[lo:hi]
    order = np.argsort(seg)[::-1]
    peaks = []
    for i in order:
        lag = (i + lo) / rate * 1000
        if all(abs(lag - p[0]) > 3 for p in peaks):
            peaks.append((lag, seg[i]))
        if len(peaks) == 4:
            break
    print(f"rms {np.sqrt((a ** 2).mean()):.1f}  peak {abs(a).max():.0f}  ({n / rate:.1f} s)")
    print("autocorrelation peaks (lag ms, strength):", ", ".join(f"{l:.1f} ({s:.3f})" for l, s in peaks))

    # Per-click: find strong onsets, then within 150 ms after each, the next strong onset.
    thr = np.percentile(o, 99.9) * 0.3
    idx = np.where(o > thr)[0]
    clicks = []
    last = -rate
    for i in idx:
        if i - last > rate * 0.25:  # new click group (metronome >= 0.25 s apart)
            clicks.append(i)
        last = i
    gaps = []
    for c in clicks:
        w = o[c : c + int(rate * 0.150)]
        if len(w) < 10:
            continue
        p1 = int(np.argmax(w[: max(1, int(rate * 0.002))]))  # first arrival, at the group start
        start = int(rate * 0.003)
        rest = w[start:]
        if len(rest) == 0:
            continue
        p2 = int(np.argmax(rest)) + start
        if rest.max() > 0.25 * w.max():
            gaps.append((p2 - p1) / rate * 1000)
    if gaps:
        g = np.array(gaps)
        print(f"per-click second-onset gap: median {np.median(g):.1f} ms, n={len(g)}, "
              f"spread (IQR) {np.percentile(g, 75) - np.percentile(g, 25):.1f} ms")
    else:
        print("per-click: no clear double onsets")


if __name__ == "__main__":
    main(sys.argv[1])
