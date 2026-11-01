"""Write a click track for sync measurements: sharp 2 ms / 3 kHz clicks every 0.5 s.

    python3 tools/sync/make_clicks.py clicks.wav [seconds]
    ffmpeg -i clicks.wav clicks.mp3   # if the server wants a compressed file

Serve it so Music Assistant can play it by URL, e.g.
    python3 -m http.server 8765 --bind <lan-ip> --directory <dir>
"""
import sys
import wave

import numpy as np

RATE = 44100
PERIOD_S = 0.5


def main(path, seconds=180):
    a = np.zeros(RATE * seconds)
    t = np.arange(int(RATE * 0.002)) / RATE
    click = np.sin(2 * np.pi * 3000 * t) * np.exp(-t / 0.0004)
    for k in range(int(seconds / PERIOD_S)):
        i = int(k * PERIOD_S * RATE)
        a[i : i + len(click)] += click
    pcm = (np.repeat(a[:, None], 2, 1) * 0.8 * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


if __name__ == "__main__":
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 180)
