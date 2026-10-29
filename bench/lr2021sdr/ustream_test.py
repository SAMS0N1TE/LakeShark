"""Host side of the lab firmware's `ustream`: what the CH343 link really carries.

Sends setup commands at 115200, starts `ustream <ms> <baud> <fmt> [shift]`, switches the
host port to <baud>, reads binary frames (A5 5A | seq u32 | drops u32 | len u16 | payload |
crc32), checks CRC and sequence, then reads the device's summary back at 115200.

usage: ustream_test.py <ms> <baud> <cs16|cs8> [shift] [--pre "cmd" ...] [--save file.npy]
"""
import re, struct, sys, time, zlib
import numpy as np
import serial
from serial.tools import list_ports

SN = "5B91081095"

def main():
    a = sys.argv[1:]
    pre, save = [], None
    while "--pre" in a:
        i = a.index("--pre"); pre.append(a[i + 1]); del a[i:i + 2]
    if "--save" in a:
        i = a.index("--save"); save = a[i + 1]; del a[i:i + 2]
    ms, baud, fmt = int(a[0]), int(a[1]), a[2]
    shift = a[3] if len(a) > 3 else "3"
    port = next(p.device for p in list_ports.comports() if p.serial_number == SN)
    s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.05
    s.dtr = False; s.rts = False; s.open()
    time.sleep(0.2); s.read(1 << 20)
    def cmd(c, wait=2.0, until=None):
        s.write(c.encode() + b"\r\n"); end, buf = time.time() + wait, b""
        while time.time() < end:
            buf += s.read(1 << 16)
            if until and until.encode() in buf: break
        return buf.decode("utf-8", "replace")
    banner = cmd("", 6, "lriq> ")
    if "radio_init" in banner:
        print(re.search(r"LR2021-SDR.*", banner).group(0))
    for c in pre:
        print(">", c, "|", " ".join(l for l in cmd(c, 3).splitlines() if ":" in l and c.split()[0] in l)[:200])
    s.reset_input_buffer()
    s.write(f"ustream {ms} {baud} {fmt} {shift}\r\n".encode())
    buf = b""; end = time.time() + 3
    while b"switching" not in buf and time.time() < end:
        buf += s.read(4096)
    if b"switching" not in buf:
        print("no switch line:", buf[-200:]); return 1
    time.sleep(0.25)
    s.baudrate = baud
    s.timeout = 0.2
    t0 = time.time()
    raw = bytearray(); idle = 0
    deadline = t0 + ms / 1000 + 20
    while time.time() < deadline:
        chunk = s.read(1 << 18)
        if chunk:
            raw += chunk; idle = 0
            if len(raw) > 32 and raw.rfind(b"\xA5\x5A") > len(raw) - 64:
                k = raw.rfind(b"\xA5\x5A")
                if len(raw) - k >= 16 and struct.unpack_from("<H", raw, k + 10)[0] == 0:
                    break
        else:
            idle += 1
            if idle > 15 and raw: break
    t1 = time.time()
    # parse
    i, frames, bad, seq_gaps, last, payload, drops = 0, 0, 0, 0, None, bytearray(), 0
    while True:
        k = raw.find(b"\xA5\x5A", i)
        if k < 0 or k + 12 > len(raw): break
        seq, d, n = struct.unpack_from("<IIH", raw, k + 2)
        if n > 4096 or k + 16 + n > len(raw):
            i = k + 1; continue
        crc = struct.unpack_from("<I", raw, k + 12 + n)[0]
        if zlib.crc32(bytes(raw[k + 2:k + 12 + n])) != crc:
            bad += 1; print(f'  crc bad at byte {k} of {len(raw)}, seq field {seq}, after good seq {last}'); print('   text near:', bytes(raw[k:k+400]).decode('ascii','replace').encode('ascii','replace')[:0] or repr(bytes(b for b in raw[k:k+600] if 32<=b<127 or b in (10,13))[:300])); i = k + 1; continue
        if n == 0: break
        if last is not None and seq != last + 1:
            seq_gaps += 1; print(f'  seq gap {last} -> {seq} at byte {k}')
        last = seq; frames += 1; drops = d
        payload += raw[k + 12:k + 12 + n]; i = k + 16 + n
    s.baudrate = 115200; time.sleep(0.4)
    summary = cmd("", 3, "\n")
    s2 = re.search(r"ustream: .*", summary)
    pairs = len(payload) // (4 if fmt == "cs16" else 2)
    print(f"host: {len(raw)} bytes in {t1-t0:.1f} s, frames ok={frames} crc_bad={bad} seq_gaps={seq_gaps} "
          f"device_drops={drops} B, pairs={pairs}")
    print("dev :", s2.group(0) if s2 else summary[-300:])
    if save and payload:
        dt = "<i2" if fmt == "cs16" else "i1"
        np.save(save, np.frombuffer(bytes(payload), dtype=dt).reshape(-1, 2))
    s.close()
    ok = frames and not bad and not seq_gaps and not drops
    print("RESULT", "PASS" if ok else "FAIL")
    return 0 if ok else 2

if __name__ == "__main__":
    sys.exit(main())
