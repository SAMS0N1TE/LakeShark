"""bridge_console.py <cmd> [cmd...]: run console commands on the bridge board (COM by serial number)."""
import sys, time, serial
from serial.tools import list_ports
port = next(p.device for p in list_ports.comports() if p.serial_number == "5B91081095")
sp = serial.Serial(); sp.port = port; sp.baudrate = 115200; sp.timeout = 0.05; sp.dtr = False; sp.rts = False; sp.open()
b, end = b"", time.time() + 10
while time.time() < end and b"lriq> " not in b: b += sp.read(4096)
for c in sys.argv[1:]:
    sp.write(c.encode() + b"\r\n"); b, end = b"", time.time() + 3
    while time.time() < end and b.count(b"lriq> ") < 1: b += sp.read(4096)
    print(b.decode(errors="replace").replace("lriq> ", "").strip())
sp.close()
