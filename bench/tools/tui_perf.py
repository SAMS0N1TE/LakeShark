"""Drive the TUI over the console and collect `tui cost` frame timings.

Usage: python bench/tools/tui_perf.py COMx "cmd1" "cmd2" ...
Each argument is sent as a console line; `@N` sleeps N seconds, and
`cost:N` samples `tui cost` N times one second apart. Opens the port once,
with DTR/RTS low, so the board is not reset between commands.
"""
import re
import sys
import time

import serial


def drain(sp, secs):
    end = time.time() + secs
    buf = b""
    while time.time() < end:
        buf += sp.read(4096)
    return buf.decode("utf-8", "replace").replace("\r", "")


def main():
    port, cmds = sys.argv[1], sys.argv[2:]
    sp = serial.Serial()
    sp.port, sp.baudrate, sp.timeout = port, 115200, 0.1
    sp.dtr = sp.rts = False
    sp.open()
    drain(sp, 0.5)
    for c in cmds:
        if c.startswith("@"):
            time.sleep(float(c[1:]))
            continue
        if c.startswith("cost:"):
            for _ in range(int(c[5:])):
                sp.write(b"tui cost\n")
                out = drain(sp, 1.0)
                for line in out.split("\n"):
                    if line.startswith("tui:") and re.search(
                            r"cells of|frame every|frame split|cell cache|peak frame", line):
                        print(line)
            continue
        sp.write((c + "\n").encode())
        out = drain(sp, 1.5)
        for line in out.split("\n"):
            if line.strip() and not re.match(r"^[IWD] \(\d+\)", line):
                print(line)
    sp.close()


if __name__ == "__main__":
    main()
