"""Bench tests for lr2021-sdr's ESP32-P4 console firmware (prompt "lriq> ") on the LR T-Display.

  continuity: `check 3000` N times; every run must PASS with no FIFO error flags.
  comb:       the board's own spur comb (16.0 kHz apart, fixed in RF: ...151.553, 151.569,
              151.585, 151.601 MHz...) as a known signal. Retuning by d Hz must move each
              line by -d Hz in baseband: that gives the I/Q orientation, and the slope of
              cycles/pair against d gives the real sample rate, with no transmitter.

usage: fwtest.py <outdir> [continuity N] [comb]
"""
import json, os, re, sys, time
import numpy as np
import serial
from serial.tools import list_ports

SN = "5B91081095"
COMB_REF, COMB_STEP = 151.569e6, 16.0e3

def open_port():
    port = next(p.device for p in list_ports.comports() if p.serial_number == SN)
    s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.05
    s.dtr = False; s.rts = False; s.open()
    return s

def read_for(s, secs, until=None):
    end, buf = time.time() + secs, bytearray()
    while time.time() < end:
        buf += s.read(1 << 16)
        if until and re.search(until.encode(), buf):
            buf += s.read(1 << 16); break
    return buf.decode("utf-8", "replace")

def cmd(s, c, until, wait):
    s.reset_input_buffer()
    s.write(c.encode() + b"\r\n")
    return read_for(s, wait, until)

def boot(s):
    text = read_for(s, 8, r"lriq> ")
    m = re.search(r"radio_init=(-?\d+)", text)
    info = cmd(s, "info", r"LR2021 ROM .*\n", 3)
    return (m.group(1) if m else "?"), (re.search(r"LR2021 ROM .*", info) or [None])[0], text

def continuity(s, n):
    rows = []
    for k in range(n):
        out = cmd(s, "check 3000", r"result=(PASS|FAIL)", 10)
        m = re.search(r"check: accepted=(\d+) elapsed_us=(\d+) peak=(\d+) service_gap_us=(\d+) flags=(\w+)"
                      r" capture=(-?\d+) recovery=(-?\d+) rejected=(\d+) result=(\w+)", out)
        if not m:
            rows.append(dict(raw=out[-300:])); print(k, "no result:", out[-200:].strip()); continue
        r = dict(zip(("accepted", "elapsed_us", "peak", "gap_us", "flags", "capture", "recovery",
                      "rejected", "result"), m.groups()))
        rows.append(r)
        rate = int(r["accepted"]) / 4 / (int(r["elapsed_us"]) / 1e6)
        print(f"check {k:2d}: {r['result']} bytes={r['accepted']} peak={r['peak']} gap={r['gap_us']} us "
              f"flags={r['flags']} rate={rate:.0f} pairs/s", flush=True)
    return rows

def capture(s, ms=100):
    out = cmd(s, f"capture {ms}", r"result=(PASS|FAIL)", 10)
    if "result=PASS" not in out:
        return None, out[-300:]
    text = cmd(s, "dump", r"lrfifo: bytes=\d+.*\n", 60)
    data = bytearray()
    for m in re.finditer(r"^lrfifo-hex: (\d+) ([0-9A-F]+)\s*$", text, re.M):
        if int(m.group(1)) != len(data): return None, "offset gap"
        data += bytes.fromhex(m.group(2))
    crc = re.search(r"lrfifo-crc32: ([0-9A-F]{8}) bytes=(\d+)", text)
    import zlib
    if not crc or int(crc.group(2)) != len(data) or zlib.crc32(bytes(data)) != int(crc.group(1), 16):
        return None, "crc/length mismatch"
    iq = np.frombuffer(bytes(data), dtype="<i2").reshape(-1, 2).astype(np.float64)
    return iq[:, 0] + 1j * iq[:, 1], "ok"

def spectrum(x, n=4096):
    x = x - x.mean(); w = np.hanning(n)
    p = sum(np.abs(np.fft.fft(x[k*n:(k+1)*n] * w)) ** 2 for k in range(len(x) // n))
    return np.fft.fftshift(p)

def comb(s, outdir, tunes):
    """Find each comb line near where it should be for both orientations; fit the rate."""
    pts, rows = {+1: [], -1: []}, []
    for hz in tunes:
        out = cmd(s, f"tune {hz}", r"tune: .*\n", 4)
        x, why = capture(s)
        if x is None:
            print("tune", hz, "capture failed:", why); continue
        np.save(os.path.join(outdir, f"comb_{hz}.npy"), x.astype(np.complex64))
        p = spectrum(x); n = len(p)
        cyc = (np.arange(n) - n // 2) / n                      # cycles per pair
        floor = np.median(p)
        # comb lines within +-50 kHz of the tune, expected at (line - tune) for the right sign
        lines = COMB_REF + COMB_STEP * np.arange(-12, 13)
        lines = lines[np.abs(lines - hz) < 50e3]
        for sign in (+1, -1):
            hit = []
            for L in lines:
                want = sign * (L - hz) / 125000.0
                win = np.abs(cyc - want) < 400 / 125000.0       # +-400 Hz search (rate unknown)
                if not win.any(): continue
                k = np.argmax(np.where(win, p, 0))
                hit.append((L, cyc[k], 10 * np.log10(p[k] / floor)))
            score = np.mean([h[2] for h in hit]) if hit else 0
            rows.append(dict(hz=hz, sign=sign, score=score, lines=len(hit)))
            pts[sign] += [(L - hz, c, d) for L, c, d in hit if d > 10]
        print(f"tune {hz}: comb score +orientation {rows[-2]['score']:+.1f} dB  -orientation {rows[-1]['score']:+.1f} dB",
              flush=True)
    best = max((+1, -1), key=lambda g: np.mean([r["score"] for r in rows if r["sign"] == g] or [0]))
    pb = pts[best]
    print("orientation:", "I + jQ as stored (RF above the tune is a positive frequency)" if best > 0
          else "inverted (RF above the tune shows as a negative frequency)", flush=True)
    if len(pb) >= 3:
        d = np.array([p[0] for p in pb]); c = np.array([p[1] for p in pb])
        slope, icpt = np.polyfit(d, c, 1)
        rate = best / slope
        print(f"rate fit over {len(pb)} lines: {rate:.1f} pairs/s (nominal 125000, {1e6*(rate/125000-1):+.0f} ppm), "
              f"offset {best*icpt*rate:+.1f} Hz", flush=True)
        return rows, dict(orientation=best, rate=rate, lines=len(pb), offset_hz=best * icpt * rate)
    return rows, dict(orientation=best)

def main():
    outdir = sys.argv[1]; os.makedirs(outdir, exist_ok=True)
    args = sys.argv[2:]
    s = open_port()
    init, rom, banner = boot(s)
    print("radio_init", init, "|", rom, flush=True)
    res = dict(radio_init=init, rom=rom)
    if "continuity" in args:
        n = int(args[args.index("continuity") + 1])
        res["continuity"] = continuity(s, n)
    if "comb" in args:
        res["comb"], res["rate"] = comb(s, outdir, [151560000, 151565000, 151570000, 151575000, 151580000, 151590000])
    json.dump(res, open(os.path.join(outdir, "fwtest.json"), "w"), indent=1, default=str)
    s.close()

if __name__ == "__main__":
    main()
