"""Save Wi-Fi credentials on the LR T-Display running lr2021-sdr firmware, reading the
password from a file so it is never typed, echoed or printed.

usage: python wifi_provision.py "<ssid>" "<file holding only the password>"

Prints only the board's `wifi:` lines. Opening the port resets the board; the script
waits for the prompt, saves the credentials, and the board joins on its own.
"""
import re, sys, time
import serial
from serial.tools import list_ports

SN = "5B91081095"

def quote(v):
    return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'

ssid = sys.argv[1]
password = open(sys.argv[2], encoding="utf-8").read().strip("\r\n")
port = next(p.device for p in list_ports.comports() if p.serial_number == SN)
s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.1; s.dtr = False; s.rts = False; s.open()
buf, end = b"", time.time() + 10
while time.time() < end and b"lriq> " not in buf:
    buf += s.read(4096)
s.write(f"wifi set {quote(ssid)} {quote(password)}\r\n".encode())
del password
out, end = b"", time.time() + 25
while time.time() < end:
    out += s.read(4096)
    if b"connected ip=" in out or b"result=-" in out:
        break
s.close()
for line in out.decode("utf-8", "replace").splitlines():
    if re.match(r"^\s*(wifi:|[IWE] \(\d+\) wifi)", line) and "wifi set" not in line:
        print(line.strip())
