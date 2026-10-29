"""Resume shortly after a park: silence, wait for the park (~3.7 s) plus GAP,
then silence again inside a 2 s mic window on the SX board.
earlyresume.py <gap_s> <trials>"""
import sys, time, re
import serial
from serial.tools import list_ports

LR, SX = "5B91081095", "5C84301528"


def port(sn):
    p = next(p.device for p in list_ports.comports() if p.serial_number == sn)
    s = serial.Serial(); s.port = p; s.baudrate = 115200; s.timeout = 0.05
    s.dtr = False; s.rts = False; s.open(); time.sleep(0.3); s.read(65536)
    return s


lr, sx = port(LR), port(SX)
def send(c): lr.write(b"\r\n" + c.encode() + b"\r\n")
gap, trials = float(sys.argv[1]), int(sys.argv[2])
peaks = []
for t in range(trials):
    sx.read(65536)
    send("audio silence")
    time.sleep(3.7 + gap - 2.0)            # mic flushes 2 s first
    sx.write(b"\r\nmic 2000\r\n")
    time.sleep(2.0)
    send("audio silence")
    buf = b""; end = time.time() + 5
    while time.time() < end and b"rms" not in buf: buf += sx.read(4096)
    m = re.search(rb"peak (\d+)", buf)
    peaks.append(int(m.group(1)) if m else -1)
    time.sleep(8)
print("peaks", peaks)
