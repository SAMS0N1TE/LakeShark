"""Pair a Bluetooth LE keyboard or mouse with the board, from the PC.

  python ble_kbd.py COM20 scan [seconds]   list what is advertising, keyboards first
  python ble_kbd.py COM20 pair <id>        pair one; type the passkey it shows on the keyboard, then Enter
  python ble_kbd.py COM20 list             paired keyboards and their state
  python ble_kbd.py COM20 forget <id>      drop one pairing
  python ble_kbd.py COM20 watch [seconds]  print the keys and pointer events the board receives
  python ble_kbd.py COM20 cancel           stop a pairing in progress

Put the keyboard in pairing mode first. It talks to the board's console
(the 'ble hid' commands) and does not reset the board. Close the desktop
window first: only one program can hold the port.
"""
import re
import sys
import time

import serial

ROW = re.compile(r"^\s*(\d+)\s+(.*?)\s*([0-9a-f]{2}(?::[0-9a-f]{2}){5}/\d)\s+rssi=(-?\d+)\s+hid=(\d)\s+appearance=([0-9a-f]+)(.*)$")


def open_port(name):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = name, 115200, 0.1
    s.dtr = s.rts = False                   # opening must not reset the board
    s.open()
    s.write(b"\r")
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def run(s, cmd, seconds, show=False):
    s.write(cmd.encode() + b"\r")
    end, out = time.time() + seconds, b""
    while time.time() < end:
        chunk = s.read(4096)
        out += chunk
        if show and chunk:
            print(chunk.decode(errors="replace"), end="", flush=True)
    return out.decode(errors="replace")


def devices(text):
    rows = []
    for line in text.splitlines():
        m = ROW.match(line)
        if m:
            idn, name, addr, rssi, hid, app, rest = m.groups()
            kbd = hid == "1" or "kbd=1" in rest or app.lower().startswith("03c")
            rows.append((not kbd, not name, -int(rssi), idn, name or "(no name)", addr, rssi, kbd, rest))
    return sorted(rows)


def scan(s, seconds):
    print("scanning %d s - keyboard in pairing mode?" % seconds)
    run(s, "ble hid scan %d" % seconds, seconds + 1)
    rows = devices(run(s, "ble hid devices", 2))
    named = [r for r in rows if r[7] or r[4] != "(no name)"]
    print("\n%-5s %-26s %-20s %5s  %s" % ("id", "name", "address", "rssi", "kind"))
    for r in named:
        print("%-5s %-26s %-20s %5s  %s" % (r[3], r[4][:26], r[5], r[6], "KEYBOARD/MOUSE" if r[7] else ""))
    print("(%d more without a name)" % (len(rows) - len(named)))
    if not any(r[7] for r in rows):
        print("\nNo keyboard heard. Put it in pairing mode (on a Rii: hold the Bluetooth/connect key until the light flashes) and scan again.")
    else:
        print("\nNext: python ble_kbd.py %s pair <id>" % s.port)


def pair(s, idn):
    print("pairing %s - when a 6-digit passkey appears, type it on the keyboard and press Enter" % idn)
    end = time.time() + 90
    text = run(s, "ble hid pair %s" % idn, 1, show=True)
    while time.time() < end:
        chunk = s.read(4096).decode(errors="replace")
        if chunk:
            print(chunk, end="", flush=True)
            text += chunk
            m = re.search(r"passkey\D{0,12}(\d{6})", text, re.I)
            if m and "SHOWN" not in text:
                print("\n\n    >>> type  %s  on the keyboard, then Enter <<<\n" % m.group(1), flush=True)
                text += "SHOWN"
        if re.search(r"\bREADY\b", text) or re.search(r"pair(ing)? (failed|timed out|cancel)", text, re.I):
            break
    print()
    print(run(s, "ble hid bonds", 2))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    s = open_port(sys.argv[1])
    cmd, rest = sys.argv[2], sys.argv[3:]
    if cmd == "scan":
        scan(s, int(rest[0]) if rest else 15)
    elif cmd == "pair" and rest:
        pair(s, rest[0])
    elif cmd == "list":
        print(run(s, "ble hid bonds", 2))
        print(run(s, "ble hid devices", 2))
    elif cmd == "forget" and rest:
        print(run(s, "ble hid forget %s" % rest[0], 2))
    elif cmd == "cancel":
        print(run(s, "ble hid cancel", 2))
    elif cmd == "watch":
        secs = int(rest[0]) if rest else 30
        run(s, "input watch on", 1)
        try:
            run(s, "", secs, show=True)
        finally:
            run(s, "input watch off", 1)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
