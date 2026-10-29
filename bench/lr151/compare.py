"""Antenna on vs off: averaged LR spectra per tune in raw dB (same gain step both runs),
on an RF axis, with the 16 kHz comb lines found with the antenna off marked.
usage: compare.py <on_dir> <off_dir> <png>"""
import json, os, sys
import numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

def avg(d):
    meta = json.load(open(os.path.join(d, "meta.json"))); g = {}
    for m in meta["lr"]:
        iq = np.load(os.path.join(d, m["file"])).astype(np.float64)
        x = iq[:, 0] + 1j * iq[:, 1]; n = 4096; w = np.hanning(n)
        p = sum(np.abs(np.fft.fft(x[k*n:(k+1)*n] * w)) ** 2 for k in range(len(x) // n)) / (len(x) // n)
        g.setdefault((m["khz"], m["rate"], m["gain"]), []).append(np.fft.fftshift(p))
    return {k: np.mean(v, axis=0) for k, v in g.items()}

on, off = avg(sys.argv[1]), avg(sys.argv[2])
keys = sorted(set(on) & set(off))
fig, axs = plt.subplots(len(keys), 1, figsize=(12, 3.6 * len(keys)))
for ax, k in zip(np.atleast_1d(axs), keys):
    khz, rate, gain = k; n = len(on[k])
    f = khz / 1e3 + (np.arange(n) - n // 2) * rate / n / 1e6
    ax.plot(f, 10 * np.log10(on[k]), lw=0.5, label="antenna on")
    ax.plot(f, 10 * np.log10(off[k]), lw=0.5, label="antenna off")
    comb = 151.56899 + 0.016 * np.arange(-200, 200)
    for c in comb[(comb > f[0]) & (comb < f[-1])]:
        ax.axvline(c, color="r", lw=0.4, alpha=0.35)
    for ch in (151.575, 152.600):
        if f[0] < ch < f[-1]:
            ax.axvspan(ch - 0.00625, ch + 0.00625, color="0.85", zorder=-1)
    ax.set_title(f"LR tuned {khz/1e3:.3f} MHz, {rate/1e3:.0f} kS/s, gain step 13 (AGC)  -  red: 16 kHz comb through 151.569")
    ax.set_xlabel("MHz"); ax.set_ylabel("dB (raw)"); ax.legend(fontsize=8)
fig.tight_layout(); fig.savefig(sys.argv[3], dpi=110)
for k in keys:
    print(k, "floor on %.1f off %.1f dB" % (10*np.log10(np.median(on[k])), 10*np.log10(np.median(off[k]))))
