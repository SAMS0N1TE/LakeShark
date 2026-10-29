"""One TX burst over the bridge protocol (TCP), printing the device's TX_END in full.
usage: tx_test.py <board ip> <hz> <rate> <tone_hz> <pairs> [power_half_db] [radius] [unchecked]"""
import math, struct, sys, time, zlib
sys.path.insert(0, __file__.rsplit("\\", 1)[0] if "\\" in __file__ else ".")
from bridge_test import Link, TcpPort
HELLO, SET_RX, TX_BEGIN, TX_DATA, TX_COMMIT, BYE, TX_END = 1, 2, 6, 7, 8, 10, 0xC2
ip, hz, rate, tone, pairs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4]), int(sys.argv[5])
power = int(sys.argv[6]) if len(sys.argv) > 6 else -18
radius = int(sys.argv[7]) if len(sys.argv) > 7 else 50
flags = 1 if len(sys.argv) > 8 and sys.argv[8] == "unchecked" else 0
iq = bytearray()
for k in range(pairs):
    ph = 2 * math.pi * tone * k / rate
    iq += struct.pack("bb", int(round(radius * math.cos(ph))), int(round(radius * math.sin(ph))))
if ip.startswith("serial"):
    import serial as _s
    from serial.tools import list_ports
    port = next(p.device for p in list_ports.comports() if p.serial_number == "5B91081095")
    sp = _s.Serial(); sp.port = port; sp.baudrate = 115200; sp.timeout = 0.05; sp.dtr = False; sp.rts = False; sp.open()
    b, end = b"", time.time() + 8
    while time.time() < end and b"lriq> " not in b: b += sp.read(4096)
    sp.write(b"\r\nbridge 4000000\r\n"); b, end = b"", time.time() + 2
    while time.time() < end and b"lrbridge: baud" not in b: b += sp.read(4096)
    time.sleep(0.1); sp.baudrate = 4000000
    try: sp.set_buffer_size(rx_size=1 << 22)
    except Exception: pass
    sp.reset_input_buffer(); L = Link(sp)
else:
    L = Link(TcpPort(ip))
r, _ = L.request(HELLO, struct.pack("<H", 1)); print("HELLO caps=0x%02X" % struct.unpack_from("<iHH", r)[2])
rec = struct.pack("<IIIIbBQBB", len(iq), zlib.crc32(bytes(iq)), rate, hz, power, 1, 0, 2, flags)
r, _ = L.request(TX_BEGIN, rec); print("TX_BEGIN status", struct.unpack_from("<i", r)[0], "len", len(rec))
off = 0
while off < len(iq):
    chunk = bytes(iq[off:off + 1024])
    r, _ = L.request(TX_DATA, struct.pack("<I", off) + chunk)
    st, nxt = struct.unpack_from("<iI", r)
    if st: print("TX_DATA status", st, "at", off); break
    off = nxt
print("uploaded", off, "bytes")
t0 = time.time()
r, other = L.request(TX_COMMIT, b"", wait=5); print("TX_COMMIT status", struct.unpack_from("<i", r)[0])
end = None
while time.time() - t0 < 8 and end is None:
    for t, seq, p in other + L.frames():
        if t == TX_END: end = p
    other = []
if end:
    f = struct.unpack_from("<iiiBHHIIQQQI", end)
    names = "error stop_error recovery_error tx_flags final_level min_level irq samples start_time elapsed_us max_gap_us untested".split()
    print("TX_END:", " ".join(f"{n}={v:#x}" if n in ("irq", "tx_flags", "untested") else f"{n}={v}" for n, v in zip(names, f)))
else:
    print("no TX_END")
L.request(BYE)
