#!/usr/bin/env python3
"""Convert a WAV recording into src/custom_horn.h for HORN_RECORDING mode.

Usage:  python3 tools/wav2header.py my_bus_horn.wav [--loop 0.3]

- Accepts 8/16-bit PCM WAV, mono or stereo, any sample rate
  (convert an mp3 first, e.g. in Audacity: File > Export > WAV 16-bit).
- Trims leading/trailing silence, normalizes, resamples to 22050 Hz mono,
  keeps at most 4 seconds (about 176 KB of flash).
- --loop sets where playback restarts while the button stays held,
  as a fraction of the clip (default 0.3 = skip the attack when looping).
"""
import argparse
import array
import os
import sys
import wave

OUT_RATE = 22050
MAX_SECONDS = 4.0


def read_wav(path):
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 2:
        data = array.array("h", raw)
        if sys.byteorder == "big":
            data.byteswap()
        samples = [x / 32768.0 for x in data]
    elif width == 1:
        samples = [(b - 128) / 128.0 for b in raw]
    else:
        sys.exit("Only 8 or 16-bit PCM WAV is supported. Re-export as 16-bit.")
    mono = [sum(samples[i:i + ch]) / ch for i in range(0, len(samples), ch)]
    return mono, rate


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--loop", type=float, default=0.3)
    args = ap.parse_args()

    x, rate = read_wav(args.wav)

    # trim silence (below 3% of peak)
    peak = max((abs(v) for v in x), default=0)
    if peak == 0:
        sys.exit("The file is silent.")
    thr = peak * 0.03
    start = next(i for i, v in enumerate(x) if abs(v) > thr)
    end = len(x) - next(i for i, v in enumerate(reversed(x)) if abs(v) > thr)
    x = x[start:end]

    # resample (linear) to OUT_RATE, cap length
    step = rate / OUT_RATE
    n = min(int(len(x) / step), int(MAX_SECONDS * OUT_RATE))
    y = []
    for i in range(n):
        p = i * step
        j = int(p)
        f = p - j
        a = x[j]
        b = x[j + 1] if j + 1 < len(x) else 0.0
        y.append(a + (b - a) * f)

    # normalize to 95%, short fade-out to avoid a click
    g = 0.95 / max(abs(v) for v in y)
    fade = min(len(y), int(0.01 * OUT_RATE))
    out = []
    for i, v in enumerate(y):
        k = min(1.0, (len(y) - i) / fade) if fade else 1.0
        out.append(int(max(-32767, min(32767, v * g * k * 32767))))

    loop = max(0, min(len(out) - 1, int(len(out) * args.loop)))
    dst = os.path.join(os.path.dirname(__file__), "..", "src", "custom_horn.h")
    with open(dst, "w") as f:
        f.write(f"// Generated from {os.path.basename(args.wav)} by tools/wav2header.py\n#pragma once\n")
        f.write(f"static const uint32_t CUSTOM_HORN_RATE = {OUT_RATE};\n")
        f.write(f"static const uint32_t CUSTOM_HORN_LEN = {len(out)};\n")
        f.write(f"static const uint32_t CUSTOM_HORN_LOOP_START = {loop};\n")
        f.write("static const int16_t customHorn[] = {\n")
        for i in range(0, len(out), 16):
            f.write("  " + ",".join(str(v) for v in out[i:i + 16]) + ",\n")
        f.write("};\n")
    print(f"Wrote {os.path.normpath(dst)}: {len(out) / OUT_RATE:.2f} s, "
          f"{len(out) * 2 // 1024} KB. Set HORN_STYLE = HORN_RECORDING in main.cpp.")


if __name__ == "__main__":
    main()
