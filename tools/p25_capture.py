#!/usr/bin/env python3
"""Record a P25 call on the board and bring it to the bench.

    python tools/p25_capture.py choppy-dispatch          # waits for voice, then records
    python tools/p25_capture.py quiet-ch --now --blocks 128
    python tools/p25_capture.py x --host 192.168.4.1     # fetch over Wi-Fi instead

The board records 16 KiB blocks of the IQ the P25 receiver is decoding (256
blocks is 8.7 s, the most it holds), saves them to the SD card with
'p25 capture save', and reads the capture over the console into
bench/captures/p25/ - about 35 minutes for a full capture, 9 for --blocks 64.
Taking the card out and copying <name>.iq and <name>.txt from the capture
folder is quicker. --host fetches over the Wi-Fi file page instead, which only
the headless boards run; boards with a screen do not start it.

From bench/captures/p25/, bench/p25_voice_check.py replays it through the
production decoder on every bench run, and p25_voice_replay writes what it
heard as a WAV.

Leave P25 open on the frequency you want before running this.
"""
import argparse
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ls_console import DEFAULT_SERIAL, find_port  # noqa: E402
import serial  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "bench", "captures", "p25")
TEL = re.compile(r"P25TEL: .*sync=(\d+) .*bchOK=(\d+) .*vox=(\d+)")


class Board:
    def __init__(self, port):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.05
        self.s.dtr = self.s.rts = False      # high resets the P4
        self.s.open()
        try:
            self.s.set_buffer_size(rx_size=1 << 20)
        except (AttributeError, NotImplementedError):
            pass
        self.pending = ""

    def send(self, cmd):
        self.s.write((cmd + "\r\n").encode())

    def lines(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            d = self.s.read(8192)
            if not d:
                continue
            self.pending += d.decode(errors="replace").replace("\r", "")
            while "\n" in self.pending:
                line, self.pending = self.pending.split("\n", 1)
                yield line

    def until(self, pattern, timeout):
        rx = re.compile(pattern)
        for line in self.lines(timeout):
            m = rx.search(line)
            if m:
                return m
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", help="letters, digits, - and _")
    ap.add_argument("--blocks", type=int, default=256, help="16 KiB blocks, 1..256 (default 256 = 8.7 s)")
    ap.add_argument("--now", action="store_true", help="record at once instead of waiting for voice")
    ap.add_argument("--wait", type=float, default=600, help="seconds to wait for a call (default 600)")
    ap.add_argument("--host", help="fetch over this address's Wi-Fi file page (headless boards)")
    ap.add_argument("--serial", default=DEFAULT_SERIAL)
    ap.add_argument("--port")
    a = ap.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,40}", a.name):
        sys.exit("name is letters, digits, - and _ only")

    port = a.port or find_port(a.serial)
    if not port:
        sys.exit("no board with serial %s" % a.serial)
    b = Board(port)
    b.send("log * error")
    time.sleep(0.4)
    b.send("log P25TEL warn")
    b.send("p25 capture free")
    time.sleep(0.4)

    if not a.now:
        print("waiting for voice on the P25 frequency...", flush=True)
        prev, end = None, time.time() + a.wait
        while True:
            m = b.until(TEL, max(1, end - time.time()))
            if not m:
                sys.exit("no voice heard - is P25 open, and is anybody talking?")
            vox = int(m.group(3))
            if prev is not None and vox > prev:
                break
            prev = vox
    b.send("p25 capture start %d" % a.blocks)
    print("recording %d blocks (%.1f s)..." % (a.blocks, a.blocks * 16384 / 480000), flush=True)
    time.sleep(a.blocks * 16384 / 480000 + 1)
    b.send("log * error")
    for _ in range(30):
        b.send("p25 capture status")
        m = b.until(r"P25IQ state=(\S+) reason=(\S+).*captured=(\d+)", 2)
        if m and m.group(1) in ("done", "aborted", "failed"):
            break
        time.sleep(0.5)
    if not m or not int(m.group(3)):
        sys.exit("capture did not finish: %s" % (m.group(0) if m else "no status"))
    print("board: %s" % m.group(0), flush=True)

    b.send("p25 capture save %s" % a.name)
    # 4 MiB to the card took 30-90 s on the T-Display-P4.
    m = b.until(r"P25IQ (saved=\S+|save: .*)", 240)
    if not m or not m.group(1).startswith("saved="):
        sys.exit("save failed: %s" % (m.group(1) if m else "no answer"))

    host = a.host
    os.makedirs(OUT, exist_ok=True)
    dest = os.path.join(OUT, a.name)
    if host:
        for ext in (".iq", ".txt"):
            url = "http://%s/dl?f=%s%s" % (host, a.name, ext)
            t = time.time()
            with urllib.request.urlopen(url, timeout=60) as r, open(dest + ext, "wb") as fh:
                fh.write(r.read())
            print("fetched %s  %d bytes in %.1f s" % (url, os.path.getsize(dest + ext), time.time() - t))
    else:
        print("reading it over the console (slow - the card is quicker)...", flush=True)
        b.send("p25 capture status")
        m = b.until(r"captured=(\d+).*rate=(\d+).*demod_gain_bits=([0-9a-f]+)", 3)
        size, rate, bits = int(m.group(1)), m.group(2), m.group(3)
        import struct
        gain = struct.unpack(">f", bytes.fromhex(bits))[0]
        with open(dest + ".iq", "wb") as fh:
            off = 0
            while off < size:
                b.send("p25 capture read %d %d" % (off, min(256, size - off)))
                m = b.until(r"P25IQ offset=(\d+) bytes=(\d+) hex=([0-9a-f]+)", 3)
                if m and int(m.group(1)) == off:
                    fh.write(bytes.fromhex(m.group(3)))
                    off += int(m.group(2))
        with open(dest + ".txt", "w") as fh:
            fh.write("format=u8-iq\nbytes=%d\nrate=%s\ndemod_gain=%g\ndemod_gain_bits=%s\n" % (size, rate, gain, bits))
    b.send("p25 capture free")
    print("\n%s.iq is on the bench. Listen to what the decoder makes of it:" % dest)
    print("  bench/build/p25_voice_replay %s.iq <demod_gain from %s.txt> %s.wav" % (dest, dest, dest))
    print("and it is checked on every run by bench/p25_voice_check.py from now on.")


if __name__ == "__main__":
    main()
