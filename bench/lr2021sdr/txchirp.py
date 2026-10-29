"""One TX burst of a linear chirp (-20..+20 kHz) over the bridge (TCP).
usage: txchirp.py <board ip> <hz> <rate> <pairs> [sign]   (sign -1 sweeps +20..-20 kHz)"""
import struct, sys, time, zlib
import numpy as np
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from bridge_test import Link, TcpPort
HELLO, TX_BEGIN, TX_DATA, TX_COMMIT, BYE, TX_END = 1, 6, 7, 8, 10, 0xC2
ip, hz, rate, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]); sgn = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0
t = np.arange(n) / rate; T = n / rate
x = 38 * np.exp(2j * np.pi * sgn * (-20e3 * t + 40e3 * t * t / (2 * T)))
iq = np.empty(2 * n, np.int8); iq[0::2] = np.round(x.real); iq[1::2] = np.round(x.imag); iq = iq.tobytes()
L = Link(TcpPort(ip)); L.request(HELLO, struct.pack("<H", 1))
r, _ = L.request(TX_BEGIN, struct.pack("<IIIIbBQBB", len(iq), zlib.crc32(iq), rate, hz, -18, 1, 0, 2, 0))
assert struct.unpack_from("<i", r)[0] == 0
off = 0
while off < len(iq):
    r, _ = L.request(TX_DATA, struct.pack("<I", off) + iq[off:off + 1024]); st, off = struct.unpack_from("<iI", r); assert st == 0
t0 = time.time(); r, other = L.request(TX_COMMIT, b"", wait=5); end = None
while time.time() - t0 < 8 and end is None:
    for ty, seq, p in other + L.frames():
        if ty == TX_END: end = p
    other = []
f = struct.unpack_from("<iiiBHHIIQQQI", end)
print(f"rate {rate}: TX_END error={f[0]} irq={f[6]:#x} samples={f[7]} min_level={f[5]}")
L.request(BYE)
