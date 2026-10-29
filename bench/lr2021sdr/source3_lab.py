"""Does source 3 follow the modem? Source 3 at input code 28 under FSK, FLRC and BLE, on the
152.6 MHz pager, plus small tune offsets: a frequency discriminator gives a DC level that moves
with the carrier offset and two-level data; raw baseband would not."""
import json, re, subprocess, sys, time
import numpy as np
import serial
from serial.tools import list_ports
SP = sys.argv[1]
CFGS = [
    ("FSK, source 3, 152.600", "fsk 0 3", 152600000),
    ("FSK, source 3, 152.596 (pager +4 kHz)", "fsk 0 3", 152596000),
    ("FSK, source 3, 152.604 (pager -4 kHz)", "fsk 0 3", 152604000),
    ("FSK, source 3, 151.500 (quiet)", "fsk 0 3", 151500000),
    ("FLRC 307 kHz, source 3, 152.600", "flrc 7 3", 152600000),
    ("BLE 1M, source 3, 152.600", "ble 0 3", 152600000),
    ("FSK, source 1, 152.600 (I/Q reference)", "fsk 0 -1", 152600000),
]
port = next(p.device for p in list_ports.comports() if p.serial_number == "5B91081095")
s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.1; s.dtr = False; s.rts = False; s.open()
buf = b""; end = time.time() + 25
while time.time() < end and b"connected ip=" not in buf: buf += s.read(65536)
for k, (name, lab, hz) in enumerate(CFGS):
    s.reset_input_buffer(); s.write(f"labmode {lab}\r\n".encode()); time.sleep(0.6); s.read(4096)
    npy = SP + "\s3lab_" + str(k) + ".npy"
    r = subprocess.run(["python", "-I", r"C:\Users\DEV\LakeShark-temp-snapshot\bench\lr2021sdr\bridge_test.py", "0", "--tcp", "192.168.16.208",
                        "--hz", str(hz), "--route", "2", "--input", "28", "--rate", "125000", "--secs", "3", "--save", npy],
                       capture_output=True, text=True, timeout=90)
    m = re.search(r"flags=0x(\w+).*pairs_checked=(\d+) elapsed=([\d.]+)s", r.stdout)
    row = dict(name=name)
    if m: row.update(flags=m[1], rate=round(int(m[2]) / float(m[3])) if float(m[3]) > 0.01 else None)
    try:
        x = np.load(npy).astype(np.float64); I, Q = x[:, 0], x[:, 1]
        h, edges = np.histogram(I, bins=np.arange(-128.5, 128.5, 1))
        sm = np.convolve(h, np.ones(5) / 5, mode="same")
        pk = [int(edges[i] + .5) for i in range(2, len(sm) - 2) if sm[i] == sm[i-2:i+3].max() and sm[i] > 0.15 * sm.max()]
        fs = row.get("rate") or 250000; n = 1 << 13; nb = len(I) // n; ii = I - I.mean()
        S = sum(np.abs(np.fft.rfft(ii[j*n:(j+1)*n] * np.hanning(n))) ** 2 for j in range(nb)); f = np.fft.rfftfreq(n, 1 / fs)
        low = S[f < 5000].sum() / S.sum()
        row.update(mean_I=round(float(I.mean()), 2), std_I=round(float(I.std()), 2), corr_IQ=round(float(np.corrcoef(I, Q)[0, 1]), 3),
                   hist_peaks=pk[:6], power_below_5kHz=round(float(low), 3), rng=[int(I.min()), int(I.max())])
    except Exception as e:
        row["data"] = f"none ({type(e).__name__})"
    print(json.dumps(row), flush=True)
s.close()
