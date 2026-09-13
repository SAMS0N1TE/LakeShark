"""Replay a pinned public symbol capture into the P4 USB diagnostic; no RF output."""
import argparse
import hashlib
from pathlib import Path
import re
import time
import serial

SHA256 = "136bdb57d08c8ed4a9dd7b5f8a2e7476f995b60aafb45de27a0189fd66ce253b"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    data = args.capture.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError("unexpected capture hash")
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    log = []

    def command(text, marker, timeout=8):
        port.write((text + "\n").encode())
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            line = port.readline().decode(errors="replace").strip()
            if marker in line:
                return line[line.index(marker):]
            if any(s in line for s in ("Guru Meditation", "panic", "Stack canary")):
                raise RuntimeError(line)
        raise TimeoutError(text[:32])

    try:
        log.append(command("p2 test begin 92715 1f6 01a 1", "P2TEST"))
        if log[-1] != "P2TEST READY":
            raise RuntimeError(log[-1])
        started = time.monotonic()
        for pos in range(0, len(data), 208):
            part = data[pos:pos + 208]
            packed = bytearray((len(part) + 3) // 4)
            for i, dibit in enumerate(part):
                packed[i // 4] |= dibit << (6 - (i % 4) * 2)
            text = f"p2 test data {len(part)} {packed.hex()}"
            deadline = time.monotonic() + 15
            while command(text, "P2DATA") != "P2DATA OK":
                if time.monotonic() > deadline:
                    raise TimeoutError("device queue did not drain")
                time.sleep(0.02)
            if pos and pos % (208 * 250) == 0:
                print(f"Sent {pos}/{len(data)} symbols", flush=True)
        line = command("p2 test end", "P2TEST")
        deadline = time.monotonic() + 30
        while "run=1" in line:
            if time.monotonic() > deadline:
                raise TimeoutError("device decoder did not finish")
            time.sleep(0.2)
            line = command("p2 test status", "P2TEST")
        log += [line, f"transfer_seconds={time.monotonic() - started:.2f}"]
        values = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
        expected = dict(failed=0, queued=0, accepted=416245, symbols=416245,
                        voice=1368, audio=1214, muted=154, samples=1368 * 160)
        for key, value in expected.items():
            if values.get(key) != value:
                raise RuntimeError(f"{key}: {values.get(key)} != {value}")
        if values["nonzero"] <= 160 or values["energy"] <= 1000000 or values["stack_free"] < 2048:
            raise RuntimeError("missing audio or insufficient stack headroom")
        log.append("DEVICE REPLAY OK")
    finally:
        port.close()
        args.log.write_text("\n".join(log) + "\n")
    print("\n".join(log), flush=True)


if __name__ == "__main__":
    main()
