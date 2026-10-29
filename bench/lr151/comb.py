"""Level of the 16 kHz comb (151.553 + n*0.016 MHz) in a capture.py run, against the
running-median floor, averaged over the LR snapshots at each tune; a few off-comb points
(+8 kHz between the lines) give the same measure where there is no line.
usage: comb.py <outdir> [<outdir> ...]"""
import json, os, sys
import numpy as np

COMB0, STEP = 151.553e6, 16e3

def spectrum(d, m):
    iq = np.load(os.path.join(d, m["file"])).astype(np.float64)
    x = iq[:, 0] + 1j * iq[:, 1]; x -= x.mean()
    n = 4096; w = np.hanning(n)
    return np.fft.fftshift(sum(np.abs(np.fft.fft(x[k*n:(k+1)*n] * w)) ** 2 for k in range(len(x) // n)))

for d in sys.argv[1:]:
    meta = json.load(open(os.path.join(d, "meta.json")))
    groups = {}
    for m in meta["lr"]:
        groups.setdefault((m["khz"], m["rate"]), []).append(spectrum(d, m))
    for (khz, rate), ps in sorted(groups.items()):
        p = np.mean(ps, axis=0); n = len(p); f = (np.arange(n) - n // 2) * rate / n + khz * 1e3
        k = max(3, int(1000 / (rate / n)) | 1); pad = np.pad(p, k // 2, mode="edge")
        floor = np.array([np.median(pad[i:i + k]) for i in range(n)])
        r = 10 * np.log10(p / floor)
        def level(fr):
            i = np.flatnonzero(np.abs(f - fr) < 300)
            return r[i].max() if len(i) else np.nan
        lo, hi = f[int(0.1 * n)], f[int(0.9 * n)]           # stay off the filter edges
        n0, n1 = int(np.ceil((lo - COMB0) / STEP)), int(np.floor((hi - COMB0) / STEP))
        on = [fr for fr in COMB0 + STEP * np.arange(n0, n1 + 1) if abs(fr - khz * 1e3) > 1500]
        off = [fr + STEP / 2 for fr in on if lo < fr + STEP / 2 < hi]
        lv_on = [level(fr) for fr in on]; lv_off = [level(fr) for fr in off]
        print(f"{os.path.basename(d.rstrip(os.sep))}: tune {khz} kHz, {len(ps)} snapshots, floor {10*np.log10(np.median(p)):.1f} dB")
        print("   comb " + "  ".join(f"{fr/1e6:.3f}:{v:+.1f}" for fr, v in zip(on, lv_on)))
        print(f"   comb mean {np.nanmean(lv_on):+.1f} dB, between-lines mean {np.nanmean(lv_off):+.1f} dB")
