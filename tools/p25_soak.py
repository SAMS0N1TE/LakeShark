#!/usr/bin/env python3
"""Listen to live P25 on the board for a while and summarise how voice did.

    python tools/p25_soak.py 10                  # ten minutes, P25 must be tuned
    python tools/p25_soak.py 10 --open           # open the P25 app first (after a flash)
    python tools/p25_soak.py 10 --save run.log   # keep the log for later
    python tools/p25_soak.py --summarise before.log after.log

One P25QUAL line arrives per call. Calls of 2 s or more are summarised - a
shorter one is mostly its own tail - as frames synced per second of air, NID
failures, audio gaps per second (underruns beyond the ~19 every call ends
with), and the hold's released/discarded frames. Compare two firmwares at
the same time of day on the same channel: the air changes by the hour
(docs/P25_VOICE.md).

The board keeps running; nothing is reset. Opening the port does not reset
the T-Display-P4 because DTR and RTS are held low.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

QUAL = re.compile(r"\((\d+)\) P25QUAL: nac=\w+ dur=([\d.]+)s bchOK=(\d+) bchFAIL=(\d+) ok%=(\d+) "
                  r"vox=(\d+) under=(\d+) drop=(\d+)(?:.*rel=(\d+) disc=(\d+) muted=(\d+))?")
TAIL_UNDERRUNS = 19


def calls(text):
    out, seen = [], set()
    for m in QUAL.finditer(text):
        if m.group(1) in seen:
            continue
        seen.add(m.group(1))
        g = m.groups()
        out.append({"dur": float(g[1]), "ok": int(g[2]), "fail": int(g[3]), "vox": int(g[5]),
                    "under": int(g[6]), "drop": int(g[7]), "rel": int(g[8] or 0),
                    "disc": int(g[9] or 0), "muted": int(g[10] or 0)})
    return out


def summary(name, text):
    allc = calls(text)
    c = [x for x in allc if x["dur"] >= 2.0]
    air = sum(x["dur"] for x in c)
    if not c:
        return "%-24s %d calls, none of 2 s or more" % (name, len(allc))
    ok = sum(x["ok"] for x in c)
    fail = sum(x["fail"] for x in c)
    gaps = sum(max(0, x["under"] - TAIL_UNDERRUNS) for x in c)
    return ("%-24s calls %2d (of %2d)  air %5.1f s  synced/s %.2f  NID fail %4.1f%%  "
            "gaps/s %.2f  drops %d  released %d  discarded %d  muted %d" % (
                name, len(c), len(allc), air, ok / air, 100.0 * fail / max(1, ok + fail),
                gaps / air, sum(x["drop"] for x in c), sum(x["rel"] for x in c),
                sum(x["disc"] for x in c), sum(x["muted"] for x in c)))


def record(minutes, port, open_p25=False):
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = False
    s.open()
    try:
        s.set_buffer_size(rx_size=1 << 20)
    except (AttributeError, NotImplementedError):
        pass
    text = ""
    cmds = ["log * error", "log P25QUAL warn"]
    if open_p25:
        # P25 is screen 1 on the T-Display-P4 (registration order in
        # main/compact_ui.cpp); opening it starts the receiver, which a
        # freshly flashed board does not run.
        cmds.append("call ui.screen 1")
    for c in cmds:
        s.write((c + "\r\n").encode())
        time.sleep(0.5)
    if open_p25:
        time.sleep(6)
    end = time.time() + minutes * 60
    last = 0
    while time.time() < end:
        d = s.read(4096)
        if d:
            text += d.decode(errors="replace")
        n = len(calls(text))
        if n != last:
            last = n
            print("  %d calls so far" % n, flush=True)
    s.close()
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("minutes", nargs="?", type=float)
    ap.add_argument("--save", help="write the raw log here")
    ap.add_argument("--open", action="store_true", help="open the P25 app first (after a flash)")
    ap.add_argument("--summarise", nargs="+", metavar="LOG", help="summarise saved logs instead")
    ap.add_argument("--serial")
    ap.add_argument("--port")
    a = ap.parse_args()
    if a.summarise:
        for f in a.summarise:
            print(summary(os.path.basename(f), open(f, encoding="utf-8", errors="replace").read()))
        return 0
    if not a.minutes:
        ap.error("give minutes, or --summarise LOG...")
    from ls_console import DEFAULT_SERIAL, find_port
    port = a.port or find_port(a.serial or DEFAULT_SERIAL)
    if not port:
        sys.exit("no board found")
    text = record(a.minutes, port, a.open)
    if a.save:
        open(a.save, "w", encoding="utf-8").write(text)
    if not calls(text):
        print("no calls heard - is P25 running on a busy channel? (--open starts it)")
        return 1
    print(summary("this run", text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
