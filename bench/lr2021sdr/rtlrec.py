"""Gap-free RTL-SDR recording with the async reader. usage: rtlrec.py <out.npy> <center_hz> <seconds> [gain]"""
import sys
import numpy as np
from rtlsdr import RtlSdr
out, fc, secs = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
s = RtlSdr(); s.sample_rate = 2.048e6; s.center_freq = fc; s.gain = float(sys.argv[4]) if len(sys.argv) > 4 else 30
chunks, want = [], int(secs * 2.048e6)
def cb(buf, ctx):
    chunks.append(np.frombuffer(bytes(buf), np.uint8).copy())
    if sum(len(c) for c in chunks) // 2 >= want: s.cancel_read_async()
try: s.read_bytes_async(cb, 1 << 18)
except Exception: pass                                   # cancelling reports an error
s.close()
b = np.concatenate(chunks).astype(np.float32) - 127.5
np.save(out, ((b[0::2] + 1j * b[1::2]) / 128).astype(np.complex64))
print("samples", len(b) // 2)
