"""Raw-tap survey across the LR2021's receive modems (lab firmware `labmode`).
For each configuration: true pair rate (device), sample range and distinct levels,
and the board's fixed 151.5535 MHz line in dB over its neighbourhood at a 151.500 tune."""
import json, re, subprocess, sys, time
import numpy as np
import serial
from serial.tools import list_ports
SP = sys.argv[1]
CFGS = [
    ("FSK code 28, source 3", "fsk 0 3", 2, 28, 125000),
    ("FSK code 28, source 4", "fsk 0 4", 2, 28, 125000),
    ("FSK code 28, source 5", "fsk 0 5", 2, 28, 125000),
    ("FLRC 307 kHz, code 28, source 4", "flrc 7 4", 2, 28, 125000),
    ("FSK code 28, source 1 (reference)", "fsk 0 -1", 2, 28, 125000),
]
port = next(p.device for p in list_ports.comports() if p.serial_number == "5B91081095")
s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.1; s.dtr = False; s.rts = False; s.open()
buf = b""; end = time.time() + 25
while time.time() < end and b"connected ip=" not in buf: buf += s.read(65536)
results = []
for name, lab, route, code, rate in CFGS:
    s.reset_input_buffer(); s.write(f"labmode {lab}\r\n".encode()); time.sleep(0.6); s.read(4096)
    npy = SP + "\lab2_" + str(len(results)) + ".npy"
    r = subprocess.run(["python", "-I", r"C:\Users\DEV\LakeShark-temp-snapshot\bench\lr2021sdr\bridge_test.py", "0", "--tcp", "192.168.16.208",
                        "--hz", "151500000", "--route", str(route), "--input", str(code), "--rate", str(rate), "--secs", "5", "--save", npy],
                       capture_output=True, text=True, timeout=90)
    end_l = next((l for l in r.stdout.splitlines() if l.startswith("RX_END")), "")
    m = re.search(r"capture=(-?\d+).*flags=0x(\w+) fifo_peak=(\d+).*pairs_checked=(\d+) elapsed=([\d.]+)s", end_l)
    row = dict(name=name)
    if m:
        cap, flags, peak, pairs, el = int(m[1]), m[2], int(m[3]), int(m[4]), float(m[5])
        row.update(capture=cap, flags=flags, fifo_peak=peak, pairs=pairs, rate=round(pairs / el) if el > 0.01 else None)
    try:
        x = np.load(npy).astype(np.float64); z = x[:, 0] + 1j * x[:, 1]
        row.update(i_min=int(x[:, 0].min()), i_max=int(x[:, 0].max()), distinct=int(len(np.unique(x[:, 0]))),
                   rms=round(float(np.sqrt(np.mean(np.abs(z - z.mean()) ** 2))), 2))
        if row.get("rate") and len(z) > 65536:
            fs = float(row["rate"]); n = 1 << 15; nb = len(z) // n; zz = z - z.mean()
            S = np.fft.fftshift(sum(np.abs(np.fft.fft(zz[k*n:(k+1)*n] * np.hanning(n))) ** 2 for k in range(nb)))
            f = (np.arange(n) - n // 2) * fs / n; want = 151.5535e6 - 151.5e6
            if abs(want) < fs / 2:
                w = np.abs(f - want) < 1500; i = np.argmax(np.where(w, S, 0)); near = np.median(S[np.abs(f - want) < 6000])
                row.update(line_khz=round(f[i] / 1e3, 2), line_db=round(10 * np.log10(S[i] / near), 1))
    except Exception as e:
        row["data"] = f"none ({type(e).__name__})"
    print(json.dumps(row), flush=True)
    results.append(row)
s.close()
