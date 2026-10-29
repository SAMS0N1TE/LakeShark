"""Analyse a capture.py run: LR snapshot spectra (spur, DC, floor, in-channel SNR) and the
RTL reference's channel activity, matched in time.

usage: analyse.py <outdir> [png]
"""
import json, os, sys
import numpy as np

def db(x):
    return 10 * np.log10(np.maximum(x, 1e-20))

def welch(iq, n=4096):
    """Averaged Hann spectrum, fftshifted, DC removed first only for the floor estimate."""
    x = iq[:, 0].astype(np.float64) + 1j * iq[:, 1].astype(np.float64)
    segs = len(x) // n
    w = np.hanning(n)
    p = np.zeros(n)
    for k in range(segs):
        p += np.abs(np.fft.fft(x[k * n:(k + 1) * n] * w)) ** 2
    return np.fft.fftshift(p / max(segs, 1) / np.sum(w ** 2)), x

def lr_report(m, p, x, rate, off=0.0):
    """off: where the pager channel sits from the LR tune (Hz), for offset-LO captures."""
    n = len(p)
    f = (np.arange(n) - n // 2) * rate / n
    floor = np.median(p)
    dc = np.mean(x)
    chan = np.abs(f - off) <= 8000              # POCSAG +-4.5 kHz deviation, 12.5 k channel
    near = (np.abs(f - off) <= 12500) & (np.abs(f) > 200)
    k = np.argmax(np.where(near, p, 0))
    rms = np.sqrt(np.mean(np.abs(x) ** 2))
    return dict(rms_dbfs=20 * np.log10(rms / 32768 + 1e-12),
                dc=(float(dc.real), float(dc.imag)),
                dc_db=float(db(np.abs(dc) ** 2 * n) - db(floor)),
                chan_snr_db=float(db(np.mean(p[chan])) - db(floor)),
                peak_hz=float(f[k]), peak_db=float(db(p[k]) - db(floor)), f=f, p=p, floor=floor)

def rtl_activity(outdir, meta):
    times = np.load(os.path.join(outdir, "rtl_times.npy"))
    rate = meta["rtl"]["rate"] / meta["rtl"]["decim"]
    out = {}
    for k in meta["rtl"]["channels"]:
        path = os.path.join(outdir, f"rtl_{k}.npy")
        if not os.path.exists(path):
            continue
        y = np.load(path)
        blk = int(rate * 0.05)                   # 50 ms power bins
        nb = len(y) // blk
        pw = db(np.mean(np.abs(y[:nb * blk].reshape(nb, blk)) ** 2, axis=1))
        # host time of every sample: its chunk's start time plus its place in the chunk
        per = np.ceil(times[:, 1] / meta["rtl"]["decim"]).astype(int)   # y[::decim] per chunk
        ts = np.repeat(times[:, 0], per) + np.concatenate([np.arange(p) for p in per]) / rate
        ts = ts[: len(y)]
        t = ts[: nb * blk : blk][:nb]
        floor = np.percentile(pw, 20)
        out[k] = dict(t=t, ts=ts, pw=pw, floor=floor, active=pw > floor + 10)
    return out

def main():
    outdir = sys.argv[1]
    meta = json.load(open(os.path.join(outdir, "meta.json")))
    act = rtl_activity(outdir, meta)
    for k, a in act.items():
        on = a["active"]
        bursts = int(np.sum(on[1:] & ~on[:-1]))
        print(f"RTL {k}: {len(on)*0.05:6.1f} s, floor {a['floor']:.1f} dB, active {100*on.mean():4.1f}% "
              f"of the time, {bursts} bursts, peak {a['pw'].max()-a['floor']:+.1f} dB over floor")
    rows = []
    for m in meta["lr"]:
        iq = np.load(os.path.join(outdir, m["file"]))
        if len(iq) < 4096:
            print(m["file"], "short capture", m["head"]); continue
        p, x = welch(iq)
        ch = min(meta["rtl"]["channels"], key=lambda c: abs(int(c) - m["khz"]))
        off = (int(ch) - m["khz"]) * 1e3
        r = lr_report(m, p, x, m["rate"], off)
        m["chan"] = ch
        ch = ch if ch in act else None
        rtl_on = None
        if ch:
            a = act[ch]
            sel = (a["t"] >= m["t_cmd"] - 0.5) & (a["t"] <= m["t_cmd"] + 1.5)
            rtl_on = (f"{a['pw'][sel].max()-a['floor']:+.0f}dB" if sel.any() else "n/a")
        print(f"{m['file']:32s} rms {r['rms_dbfs']:6.1f} dBFS  DC {r['dc_db']:+5.1f} dB  "
              f"in-chan {r['chan_snr_db']:+5.1f} dB  top tone {r['peak_hz']:+8.0f} Hz {r['peak_db']:+5.1f} dB  "
              f"RTL same time {rtl_on}  | {m['agc']}")
        rows.append((m, r))
    if len(sys.argv) > 2:
        import matplotlib; matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, axs = plt.subplots(2 + len(act), 1, figsize=(12, 4 * (2 + len(act))))
        for m, r in rows:
            axs[0].plot(r["f"] / 1e3, db(r["p"]) - db(r["floor"]), lw=0.6,
                        label=f"{m['khz']} g{m['gain']} #{m['file'][3:7]}")
        axs[0].set_xlabel("offset from LR tune (kHz)"); axs[0].set_ylabel("dB over median floor")
        axs[0].axvspan(-6.25, 6.25, color="0.9", zorder=-1); axs[0].legend(fontsize=6, ncol=3)
        axs[0].set_title("LR2021 lrprobe snapshots (12.5 kHz channel shaded)")
        t0 = min(a["t"][0] for a in act.values()) if act else 0
        for k, a in act.items():
            axs[1].plot(a["t"] - t0, a["pw"] - a["floor"], lw=0.6, label=f"RTL {k}")
        for m, r in rows:
            axs[1].axvline(m["t_cmd"] - t0, color="r" if m["khz"] == 151575 else "g", lw=0.5, alpha=0.5)
        axs[1].set_xlabel("s"); axs[1].set_ylabel("dB over floor"); axs[1].legend()
        axs[1].set_title("RTL channel power (lines: LR snapshot times)")
        rate = meta["rtl"]["rate"] / meta["rtl"]["decim"]
        for ax, k in zip(axs[2:], act):
            y = np.load(os.path.join(outdir, f"rtl_{k}.npy"))
            nfft = 512
            nb = len(y) // nfft
            sg = db(np.abs(np.fft.fftshift(np.fft.fft(y[:nb * nfft].reshape(nb, nfft) * np.hanning(nfft), axis=1), axes=1)) ** 2)
            sg -= np.median(sg)
            tc = act[k]["ts"][: nb * nfft : nfft] - t0
            fk = (np.arange(nfft) - nfft // 2) * rate / nfft / 1e3
            ax.pcolormesh(tc, fk, sg.T, vmin=0, vmax=40, cmap="viridis", shading="nearest")
            for m, r in rows:
                if m["chan"] == k:
                    ax.axvline(m["t_cmd"] - t0, color="r" if str(m["khz"]) == k else "orange", lw=0.6)
            ax.set_ylim(-12.8, 12.8); ax.set_ylabel("kHz"); ax.set_xlabel("s")
            ax.set_title(f"RTL {k} spectrogram (red: LR snapshot on this channel)")
        fig.tight_layout(); fig.savefig(sys.argv[2], dpi=110)
        print("plot:", sys.argv[2])

if __name__ == "__main__":
    main()
