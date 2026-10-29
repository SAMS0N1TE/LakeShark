"""Near-field snap check: the SX1262 board's mic (COM13) listens while the LR
board (COM19) wakes and re-parks its audio clocks.
nearmic.py <setup cmds for LR, ';'-separated> <trials> [lr_cmd]"""
import sys, time, re
import serial
from serial.tools import list_ports

LR, SX = "5B91081095", "5C84301528"
WIN = int(__import__("os").environ.get("WIN", "6000"))  # ms; mic throws one window away first
OFFSET = float(__import__("os").environ.get("OFFSET", "0.3"))  # s, LR command vs capture start
import os; GAIN = os.environ.get("GAIN", "")


def port(sn):
    p = next(p.device for p in list_ports.comports() if p.serial_number == sn)
    s = serial.Serial(); s.port = p; s.baudrate = 115200; s.timeout = 0.05
    s.dtr = False; s.rts = False; s.open(); time.sleep(0.3); s.read(65536)
    return s


lr, sx = port(LR), port(SX)
for c in [c for c in sys.argv[1].split(";") if c]:
    lr.write(b"\r\n" + c.encode() + b"\r\n"); time.sleep(0.5)
trials = int(sys.argv[2])
lr_cmd = sys.argv[3] if len(sys.argv) > 3 else "audio silence"
time.sleep(4)
peaks = []
for t in range(trials):
    sx.read(65536); lr.read(65536)
    lead = WIN / 1000 + OFFSET     # LR command time after the mic command
    if lead >= 0:
        sx.write(f"\r\nmic {WIN}\r\n".encode()); time.sleep(lead)
        lr.write(b"\r\n" + lr_cmd.encode() + b"\r\n")
    else:
        lr.write(b"\r\n" + lr_cmd.encode() + b"\r\n"); time.sleep(-lead)
        sx.write(f"\r\nmic {WIN}\r\n".encode())
    buf = b""; lrlog = b""
    end = time.time() + 2 * WIN / 1000 + 4
    while time.time() < end:
        buf += sx.read(4096); lrlog += lr.read(4096)
        if b"rms" in buf: break
    m = re.search(rb"peak (\d+)\s+rms (\d+)", buf)
    ev = [l.strip().decode(errors="replace")[-40:] for l in lrlog.splitlines() if b"clocks" in l]
    print(f"trial {t+1}: {m.group(0).decode() if m else buf[-120:]}   LR: {ev}")
    if m: peaks.append(int(m.group(1)))
    time.sleep(1)
print("peaks", peaks)
