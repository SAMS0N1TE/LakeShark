"""Lab firmware: sustained FIFO service at 125k/250k pairs/s against the SPI clock (no host link).
usage: spitest.py   (runs `bw`, `spiclk`, then `check 3000` x3 for each combination)"""
import re, time
import serial
from serial.tools import list_ports
SN = "5B91081095"
port = next(p.device for p in list_ports.comports() if p.serial_number == SN)
s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.05; s.dtr = False; s.rts = False; s.open()
def cmd(c, wait, until):
    s.reset_input_buffer(); s.write(c.encode() + b"\r\n"); end, b = time.time() + wait, b""
    while time.time() < end:
        b += s.read(1 << 16)
        if re.search(until.encode(), b): break
    return b.decode("utf-8", "replace")
cmd("", 6, "lriq> ")
for bw in (62500,):
    print(cmd(f"bw {bw}", 3, r"bw: .*result=-?\d+").strip().splitlines()[-1])
    for clk in (16000000, 20000000, 26666666):
        l = cmd(f"spiclk {clk}", 3, r"spiclk: .*result=-?\d+")
        m = re.search(r"spiclk: .*", l); print("  ", m.group(0) if m else l[-100:])
        for k in range(3):
            o = cmd("check 3000", 10, r"result=(PASS|FAIL)")
            m = re.search(r"check: accepted=(\d+) elapsed_us=(\d+) peak=(\d+) service_gap_us=(\d+) flags=(\w+).*result=(\w+)", o)
            if m:
                acc, el = int(m.group(1)), int(m.group(2))
                print(f"     bw={bw} clk={clk} run{k}: {m.group(6)} rate={acc/4/max(el,1)*1e6:.0f} peak={m.group(3)} gap={m.group(4)}us flags={m.group(5)}", flush=True)
            else:
                print("     no result", o[-150:])
cmd("spiclk 16000000", 3, "spiclk"); cmd("bw 31250", 3, "bw:")
