"""LR2021 151.575 pager research, step 1: raw I/Q from the LR and an RTL reference at once.

The RTL records 152.0875 MHz +-1.024 MHz the whole time and keeps two channels, 151.575 and
152.600, decimated to 25.6 kS/s with host timestamps. Meanwhile the LR takes untriggered
`lora lrprobe` snapshots (hex) in a loop over the configurations given, each saved with its
host time, so a page the RTL heard can be matched to the LR snapshot taken during it.

usage: capture.py <outdir> <minutes> <cfg> [cfg ...]
  cfg = kHz[:bw[:gain[:ms]]]   e.g. 151575  152600  151545:62500  151575:31250:6
  gain 14 = the board's auto (default), 0..13 fixed steps.
"""
import json, os, re, sys, threading, time
import numpy as np
from scipy import signal
import serial
from serial.tools import list_ports

LR_SN = "5B91081095"
RTL_RATE, RTL_CENTRE, RTL_GAIN = 2.048e6, 152.0875e6, 28.0
CHANNELS = {"151575": 151.575e6, "152600": 152.600e6}
DECIM = 80                                    # 2.048 MS/s -> 25.6 kS/s

def open_lr():
    port = next(p.device for p in list_ports.comports() if p.serial_number == LR_SN)
    s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.05
    s.dtr = False; s.rts = False; s.open()
    return s

def lr_cmd(s, c, until, wait):
    s.reset_input_buffer()
    s.write(b"\r\n" + c.encode() + b"\r\n")
    end, buf = time.time() + wait, bytearray()
    while time.time() < end:
        buf += s.read(1 << 16)
        if until.encode() in buf and buf.endswith(b"\n"):
            break
    return buf.decode("utf-8", "replace")

def lr_settle(s, secs=14):
    """Opening the CH343 port resets the board: wait out the boot, then get a prompt."""
    end = time.time() + secs
    while time.time() < end:
        s.read(1 << 16)
    return lr_cmd(s, "lora lrgain", "lrgain:", 3)

class RtlRef(threading.Thread):
    def __init__(self, outdir):
        super().__init__(daemon=True)
        self.outdir, self.stop, self.err = outdir, threading.Event(), None
        self.chunks = {k: [] for k in CHANNELS}
        self.times = []

    def run(self):
        try:
            from rtlsdr import RtlSdr
            sdr = RtlSdr(); sdr.sample_rate = RTL_RATE; sdr.center_freq = RTL_CENTRE
            sdr.gain = RTL_GAIN
            n = 1 << 18
            sos = signal.butter(8, 10e3, fs=RTL_RATE, output="sos")
            zi = {k: signal.sosfilt_zi(sos) * 0 for k in CHANNELS}
            phase = {k: 0 for k in CHANNELS}
            sdr.read_samples(n)                       # settle
            while not self.stop.is_set():
                t0 = time.time()
                x = sdr.read_samples(n).astype(np.complex64)
                self.times.append((t0, len(x)))
                idx = np.arange(len(x))
                for k, f in CHANNELS.items():
                    w = -2j * np.pi * (f - RTL_CENTRE) / RTL_RATE
                    y = x * np.exp(w * (idx + phase[k])).astype(np.complex64)
                    phase[k] += len(x)
                    y, zi[k] = signal.sosfilt(sos, y, zi=zi[k])
                    self.chunks[k].append(y[::DECIM].astype(np.complex64))
            sdr.close()
        except Exception as e:                        # report, do not kill the LR loop
            self.err = repr(e)

    def save(self):
        for k in CHANNELS:
            if self.chunks[k]:
                np.save(os.path.join(self.outdir, f"rtl_{k}.npy"), np.concatenate(self.chunks[k]))
        np.save(os.path.join(self.outdir, "rtl_times.npy"), np.array(self.times, dtype=np.float64))

def parse_cfg(c):
    p = c.split(":")
    khz = int(p[0]); bw = int(p[1]) if len(p) > 1 else 31250
    gain = int(p[2]) if len(p) > 2 else 14
    ms = int(p[3]) if len(p) > 3 else 250
    return khz, bw, gain, ms

def lr_capture(s, khz, bw, gain, ms):
    c = f"lora lrprobe {khz} {bw} {gain} -1 auto {ms} hex"
    t0 = time.time()
    text = lr_cmd(s, c, "lrprobe-hex: end", 120)
    t1 = time.time()
    data = bytearray()
    for m in re.finditer(r"^lrprobe-hex: (\d+) ([0-9A-F]+)\s*$", text, re.M):
        if int(m.group(1)) != len(data):
            break
        data += bytes.fromhex(m.group(2))
    head = re.search(r"^lrprobe: .*$", text, re.M)
    spec = re.search(r"^lrprobe-spec: .*$", text, re.M)
    agc = re.search(r"^lrprobe-agc: .*$", text, re.M)
    iq = np.frombuffer(bytes(data[: len(data) // 4 * 4]), dtype="<i2").reshape(-1, 2)
    return t0, t1, iq, (head.group(0) if head else text[-300:]), spec.group(0) if spec else "", agc.group(0) if agc else ""

def main():
    outdir, minutes, cfgs = sys.argv[1], float(sys.argv[2]), [parse_cfg(c) for c in sys.argv[3:]]
    os.makedirs(outdir, exist_ok=True)
    s = open_lr()
    print("LR:", lr_settle(s).strip().splitlines()[-1] if True else "", flush=True)
    rtl = RtlRef(outdir); rtl.start()
    meta, end, n = [], time.time() + minutes * 60, 0
    try:
        while time.time() < end:
            for khz, bw, gain, ms in cfgs:
                if time.time() >= end:
                    break
                t0, t1, iq, head, spec, agc = lr_capture(s, khz, bw, gain, ms)
                name = f"lr_{n:04d}_{khz}_{bw}_g{gain}.npy"
                np.save(os.path.join(outdir, name), iq)
                meta.append(dict(file=name, khz=khz, bw=bw, rate=4 * bw, gain=gain, ms=ms,
                                 t_cmd=t0, t_done=t1, pairs=len(iq), head=head, spec=spec, agc=agc))
                print(f"{n:4d} {khz} bw={bw} g={gain} pairs={len(iq)} {t1-t0:5.1f}s | {spec[14:]} {agc[13:]}", flush=True)
                n += 1
                if rtl.err:
                    print("RTL error:", rtl.err, flush=True)
    finally:
        rtl.stop.set(); rtl.join(10); rtl.save()
        meta_doc = dict(rtl=dict(rate=RTL_RATE, centre=RTL_CENTRE, gain=RTL_GAIN, decim=DECIM,
                                 channels=CHANNELS, err=rtl.err), lr=meta)
        with open(os.path.join(outdir, "meta.json"), "w") as f:
            json.dump(meta_doc, f, indent=1)
        s.close()

if __name__ == "__main__":
    main()
