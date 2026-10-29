"""Bench client for the lr2021-sdr bridge protocol v1 (the lr2021-sdr repo's spec) over the
LR T-Display's CH343. Written from the spec only, to check the firmware independently of
the host library.

usage: bridge_test.py <baud> [--hz 152600000] [--bw 31250] [--gain 0] [--secs 10]
                     [--gainstep N] [--save file.npy] [--reopen]
"""
import argparse, socket, struct, sys, time, zlib
import numpy as np
import serial
from serial.tools import list_ports

SN = "5B91081095"
SYNC = b"LR\x01"
HELLO, SET_RX, RX_START, RX_STOP, SET_GAIN, GET_TIME, BYE = 1, 2, 3, 4, 5, 9, 10
RX_DATA, RX_END, TX_END = 0xC0, 0xC1, 0xC2

def frame(t, seq, payload=b""):
    body = struct.pack("<BBHH", 1, t, seq, len(payload)) + payload
    return b"LR" + body + struct.pack("<I", zlib.crc32(body))

class Link:
    def __init__(self, s):
        self.s, self.buf, self.seq, self.crc_bad, self.skipped = s, bytearray(), 1, 0, 0
    def send(self, t, payload=b""):
        self.seq = (self.seq + 1) & 0xFFFF
        self.s.write(frame(t, self.seq, payload)); return self.seq
    def frames(self, timeout=0.05):
        chunk = self.s.read(1 << 18)
        if chunk: self.buf += chunk
        out = []
        while True:
            k = self.buf.find(SYNC)
            if k < 0:
                self.skipped += max(0, len(self.buf) - 2); del self.buf[:max(0, len(self.buf) - 2)]; break
            if k: self.skipped += k; del self.buf[:k]
            if len(self.buf) < 8: break
            _, t, seq, n = struct.unpack_from("<BBHH", self.buf, 2)
            if n > 2048: del self.buf[:1]; continue
            if len(self.buf) < 12 + n: break
            crc = struct.unpack_from("<I", self.buf, 8 + n)[0]
            if zlib.crc32(bytes(self.buf[2:8 + n])) != crc:
                self.crc_bad += 1; del self.buf[:1]; continue
            out.append((t, seq, bytes(self.buf[8:8 + n]))); del self.buf[:12 + n]
        return out
    def request(self, t, payload=b"", wait=2.0):
        q = self.send(t, payload); end = time.time() + wait; other = []
        while time.time() < end:
            got = self.frames()
            for i, f in enumerate(got):
                if f[0] == t | 0x80 and f[1] == q: return f[2], other + got[i + 1:]
                other.append(f)
        return None, other

GAIN_TAGS = {}
def decode(b, n, fmt, shift):
    if fmt in (2, 3):
        v = np.frombuffer(b[:2 * n], dtype=np.int8).reshape(-1, 2)
        if fmt == 2:
            return v.astype(np.int32)
        u = v.view(np.uint8).astype(np.int32)
        tag = (u[:, 0] & 3) | ((u[:, 1] & 3) << 2)
        for t_, c_ in zip(*np.unique(tag, return_counts=True)):
            GAIN_TAGS[int(t_)] = GAIN_TAGS.get(int(t_), 0) + int(c_)
        return (v.astype(np.int32) >> 2)
    if fmt == 0:
        return np.frombuffer(b[:4 * n], dtype="<i2").reshape(-1, 2).astype(np.int32)
    w = np.frombuffer(b[:3 * n], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
    v = w[:, 0] | (w[:, 1] << 8) | (w[:, 2] << 16)
    i, q = v & 0xFFF, (v >> 12) & 0xFFF
    i = np.where(i >= 2048, i - 4096, i); q = np.where(q >= 2048, q - 4096, q)
    return (np.stack([i, q], axis=1) << shift).astype(np.int32)

class TcpPort:
    """The two calls Link uses, over a socket."""
    def __init__(self, host, port=51021):
        self.k = socket.create_connection((host, port), timeout=5)
        self.k.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.k.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 22)
        self.k.settimeout(0.05)
    def write(self, b): self.k.sendall(b)
    def read(self, n):
        try: return self.k.recv(n)
        except socket.timeout: return b""
    def close(self): self.k.close()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("baud", type=int)
    ap.add_argument("--hz", type=int, default=152600000)
    ap.add_argument("--bw", type=int, default=31250)
    ap.add_argument("--gain", type=int, default=0)
    ap.add_argument("--secs", type=float, default=10)
    ap.add_argument("--gainstep", type=int, default=None, help="live SET_GAIN halfway")
    ap.add_argument("--save")
    ap.add_argument("--fmt", type=int, default=0, help="0 CS16, 1 CS12")
    ap.add_argument("--tcp", help="board address: use the TCP bridge instead of serial")
    ap.add_argument("--route", type=int, default=None, help="0 source 2, 1 source 0, 2 source 1, 3 source 6")
    ap.add_argument("--input", type=int, default=0, help="input code for routes 1..3")
    ap.add_argument("--rate", type=int, default=0, help="pairs/s for routes 1..3")
    a = ap.parse_args()
    if a.tcp:
        s = TcpPort(a.tcp)
    else:
        port = next(p.device for p in list_ports.comports() if p.serial_number == SN)
        s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.05; s.dtr = False; s.rts = False; s.open()
        # boot banner (opening resets the board), then enter bridge mode
        end, banner = time.time() + 8, b""
        while time.time() < end and b"lriq> " not in banner: banner += s.read(4096)
        line = [l for l in banner.decode("utf-8", "replace").splitlines() if "LR2021-SDR" in l]
        print("banner:", line[0] if line else banner[-120:])
        s.write(f"\r\nbridge {a.baud}\r\n".encode()); end, b = time.time() + 2, b""
        while time.time() < end and b"lrbridge: baud" not in b: b += s.read(4096)
        print("console:", [l for l in b.decode("utf-8", "replace").splitlines() if "lrbridge" in l])
        time.sleep(0.1); s.baudrate = a.baud
        try: s.set_buffer_size(rx_size=1 << 22, tx_size=1 << 16)   # Windows default is 4 KB, ~10 ms here
        except Exception: pass
        s.reset_input_buffer()
    L = Link(s)
    r, _ = L.request(HELLO, struct.pack("<H", 1))
    if r is None: print("HELLO: no answer"); return 1
    st, ver, caps, rmaj, rmin, maxp, clk, txcap = struct.unpack_from("<iHHBBHII", r)
    name, ident = r[20:52].rstrip(b"\0").decode(), r[52:84].rstrip(b"\0").decode()
    print(f"HELLO: status={st} v{ver} caps=0x{caps:02X} ROM {rmaj}.{rmin} max_payload={maxp} clock={clk} "
          f"tx_capacity={txcap} name='{name}' id='{ident}'")
    rec = struct.pack("<IIBBB", a.hz, a.bw, a.gain, 0, 2)
    if a.route is not None:
        rec += struct.pack("<BBI", a.route, a.input, a.rate)
    r, _ = L.request(SET_RX, rec, wait=3)
    st, errs, rate = struct.unpack_from("<iHI", r)
    print(f"SET_RX {a.hz} bw={a.bw}: status={st} radio_errors=0x{errs:04X} rate={rate}")
    r, _ = L.request(GET_TIME); t_dev = struct.unpack_from("<iQ", r)[1]
    r, early = L.request(RX_START, struct.pack("<HIB", 1024, 0, a.fmt), wait=3)
    st, gen, rate = struct.unpack_from("<iII", r)
    print(f"RX_START: status={st} generation={gen} rate={rate}")
    t0 = time.time(); expect, gaps, pairs, dropped, data = 0, 0, 0, 0, []
    end_msg, gain_sent, shifts, gap_log, fmts = None, False, {}, [], {}
    pending = early
    while True:
        for t, seq, p in pending + L.frames():
            if t == RX_DATA:
                g, rt, first, start, drp, n, fmt, shift = struct.unpack_from("<IIQQIHBB", p)
                if first != expect:
                    gaps += 1; gap_log.append((round(time.time() - t0, 2), expect, first - expect))
                expect = first + n; pairs += n; dropped = drp; shifts[shift] = shifts.get(shift, 0) + 1
                fmts[fmt] = fmts.get(fmt, 0) + 1
                if a.save or fmt == 3: data.append(decode(p[32:], n, fmt, shift))
            elif t == RX_END:
                end_msg = p
        pending = []
        if end_msg: break
        el = time.time() - t0
        if a.gainstep is not None and not gain_sent and el > a.secs / 2:
            L.send(SET_GAIN, struct.pack("<BB", a.gainstep, 0)); gain_sent = True
        if el > a.secs and not hasattr(L, "stopped"):
            L.send(RX_STOP); L.stopped = time.time()
        if hasattr(L, "stopped") and time.time() - L.stopped > 5: print("no RX_END"); break
    el = time.time() - t0
    if end_msg:
        (gen, cerr, rerr, reason, flags, lvl, drp, gch, tot, start, elapsed, gap) = struct.unpack_from("<IiiBBHIIQQQQ", end_msg)  # 56 B; later fields ignored
        print(f"RX_END: capture={cerr} recovery={rerr} reason={reason} flags=0x{flags:02X} fifo_peak={lvl} "
              f"dropped_pairs={drp} gain_changes={gch} pairs_checked={tot} elapsed={elapsed/1e6:.2f}s "
              f"max_gap={gap}us")
        print("gaps (t, expected, missing pairs):", gap_log[:12], "..." if len(gap_log) > 12 else "")
        print(f"frame shifts: {dict(sorted(shifts.items()))}  formats: {fmts}  gain tags: {dict(sorted(GAIN_TAGS.items()))}")
        print(f"device pair rate: {tot/max(elapsed,1)*1e6:.0f} pairs/s (checked pairs over device elapsed time)")
        print(f"host: pairs received={pairs} ({100*pairs/max(tot,1):.1f}% of checked), discontinuities={gaps}, "
              f"crc_bad={L.crc_bad}, skipped_bytes={L.skipped}, link {pairs*(2 if 2 in fmts or 3 in fmts else 3 if 1 in fmts else 4)/el/1e3:.0f} kB/s of samples")
    r, _ = L.request(BYE)
    print("BYE:", "ok" if r is not None else "no answer")
    if not a.tcp:
        s.baudrate = 115200; time.sleep(1.0)
        print("console:", s.read(8192).decode("utf-8", "replace").strip()[-200:])
    if a.save and data: np.save(a.save, np.concatenate(data))
    s.close()

if __name__ == "__main__":
    sys.exit(main())
