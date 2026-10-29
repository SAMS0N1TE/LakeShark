"""LR board console helper: lrcon.py "<cmd>" [wait_s] ... ; prints the board's reply."""
import sys, time
import serial
from serial.tools import list_ports

SN = "5B91081095"

def open_port():
    port = next(p.device for p in list_ports.comports() if p.serial_number == SN)
    s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.2
    s.dtr = False; s.rts = False; s.open(); time.sleep(0.3); s.read(65536)
    return s

def cmd(s, c, wait=3.0, until=None):
    s.write(b"\r\n" + c.encode() + b"\r\n")
    end, buf = time.time() + wait, b""
    while time.time() < end:
        buf += s.read(65536)
        if until and until.encode() in buf:
            buf += s.read(65536)
            break
    return buf.decode("utf-8", "replace")

if __name__ == "__main__":
    s = open_port()
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        c = args[i]; w = 3.0
        if i + 1 < len(args):
            try: w = float(args[i + 1]); i += 1
            except ValueError: pass
        print(f">>> {c}"); print(cmd(s, c, w)); i += 1
    s.close()
