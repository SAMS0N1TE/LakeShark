"""Capture the rendered RGB565 frame held by LakeShark Game over UART."""

from argparse import ArgumentParser
from pathlib import Path
from time import monotonic, sleep

from PIL import Image
from serial import Serial

parser = ArgumentParser(description=__doc__)
parser.add_argument("port", help="USB-to-UART serial port")
parser.add_argument("output", type=Path, help="Destination PNG")
parser.add_argument("--keys", default="", help="Optional UART keys to send before capturing")
args = parser.parse_args()

with Serial(args.port, 115200, timeout=0.3) as port:
    sleep(2.2)
    port.reset_input_buffer()
    for key in args.keys.encode("ascii"):
        port.write(bytes((key,)))
        sleep(0.10)
    port.write(b"!")
    until = monotonic() + 8
    while monotonic() < until:
        if b"RGB565 400 240" in port.readline():
            break
    else:
        raise SystemExit("Firmware did not respond to the screenshot command")

    size = 400 * 240 * 2
    pixels = bytearray()
    until = monotonic() + 25
    while len(pixels) < size and monotonic() < until:
        pixels.extend(port.read(min(4096, size - len(pixels))))
    if len(pixels) != size:
        raise SystemExit(f"Incomplete frame ({len(pixels)} of {size} bytes)")

image = Image.frombytes("RGB", (400, 240), bytes(pixels), "raw", "BGR;16")
image.resize((800, 480), Image.Resampling.NEAREST).save(args.output)
