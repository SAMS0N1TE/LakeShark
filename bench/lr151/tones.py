"""Tones in averaged LR snapshots, per tune, as RF frequencies; a tone that keeps its RF
frequency when the LO moves is on the air (or radiated by the board), one that keeps its
baseband offset is made in the receiver.   usage: tones.py <outdir> [min_db]"""
import json, os, sys
import numpy as np
d = sys.argv[1]; thr = float(sys.argv[2]) if len(sys.argv) > 2 else 8
meta = json.load(open(os.path.join(d, "meta.json")))
groups = {}
for m in meta["lr"]:
    iq = np.load(os.path.join(d, m["file"])).astype(np.float64)
    x = iq[:, 0] + 1j * iq[:, 1]; x -= x.mean()
    n = 4096; w = np.hanning(n)
    p = sum(np.abs(np.fft.fft(x[k*n:(k+1)*n] * w)) ** 2 for k in range(len(x) // n))
    groups.setdefault((m["khz"], m["rate"]), []).append(np.fft.fftshift(p))
for (khz, rate), ps in sorted(groups.items()):
    p = np.mean(ps, axis=0); n = len(p)
    f = (np.arange(n) - n // 2) * rate / n
    # floor follows the filter shape: a running median over 1 kHz
    k = max(3, int(1000 / (rate / n)) | 1)
    pad = np.pad(p, k // 2, mode="edge")
    floor = np.array([np.median(pad[i:i + k]) for i in range(n)])
    r = 10 * np.log10(p / floor)
    pk = [i for i in range(2, n - 2) if r[i] > thr and r[i] == r[i-2:i+3].max() and abs(f[i]) > 150]
    print(f"tune {khz} kHz, {rate/1e3:.0f} kS/s, {len(ps)} snapshots:")
    for i in sorted(pk, key=lambda i: -r[i])[:12]:
        print(f"   {f[i]:+9.0f} Hz  RF {khz/1e3 + f[i]/1e6:10.5f} MHz  {r[i]:+5.1f} dB")
