"""Check 'term serial' on a board end to end, without a person at the keyboard.

  python -I check_serial.py COM5 [--baud 921600] [--keys 2]

Starts the serial terminal the way lsterm.py does, reads the frames back
through bench/tools/term_check.py's screen interpreter, presses the keys
(default '2', which opens FM from HOME), checks the screen changed, then
ends the session and checks the console answers at 115200 again.
Exit 0 only when all of that held. The screens are printed.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "bench", "tools"))
import serial                      # noqa: E402
from lsterm import Frames          # noqa: E402
from term_check import Screen      # noqa: E402


def pump(s, frames, screen, seconds):
    end = time.time() + seconds
    n = 0
    while time.time() < end:
        for body in frames.feed(s.read(65536)):
            screen.feed(body)
            n += len(body)
    return n


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    port = sys.argv[1]
    baud = int(sys.argv[sys.argv.index("--baud") + 1]) if "--baud" in sys.argv else 921600
    keys = sys.argv[sys.argv.index("--keys") + 1] if "--keys" in sys.argv else "2"
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.05
    s.dtr = s.rts = False
    s.open()
    s.set_buffer_size(rx_size=1 << 20, tx_size=1 << 14)
    s.write(b"\r")
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write(b"term serial %d\r" % baud)
    line, end = b"", time.time() + 4
    while b"term: serial begin" not in line and time.time() < end:
        line += s.read(4096)
    ok = b"term: serial begin" in line
    print("begin:", ok)
    if not ok:
        print(line.decode(errors="replace")[-400:])
        sys.exit(1)
    time.sleep(0.05)
    s.baudrate = baud
    frames, screen = Frames(), Screen()
    t0 = time.time()
    first = pump(s, frames, screen, 3)
    home = screen.text()
    print("first frames: %d bytes in 3 s, %d cells, bad frames %d" % (first, len(screen.cells), frames.bad))
    print(home)
    s.write(b"term in " + keys.encode().hex().encode() + b"\r")
    after = pump(s, frames, screen, 3)
    moved = screen.text()
    print("after '%s': %d bytes" % (keys, after))
    print(moved)
    s.write(b"term off\r")
    time.sleep(1.0)
    s.baudrate = 115200
    back, end = "", time.time() + 6
    while "LakeShark" not in back and time.time() < end:
        s.reset_input_buffer()
        s.write(b"\rversion\r")        # the first line can carry noise from the rate change
        time.sleep(1.0)
        back += s.read(8192).decode(errors="replace")
    console_ok = "LakeShark" in back
    print("console at 115200 after term off:", console_ok)
    changed = moved != home and len(moved) > 100
    print("unknown sequences:", len(screen.unknown), screen.unknown[:3])
    good = first > 1000 and changed and console_ok and not screen.unknown and frames.bad == 0
    print("RESULT:", "PASS" if good else "FAIL")
    sys.exit(0 if good else 1)


if __name__ == "__main__":
    main()
