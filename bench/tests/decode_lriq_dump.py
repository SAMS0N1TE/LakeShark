"""Test helper: decode_lriq_dump.py console.log output.iq16le"""
import argparse
import base64
import binascii
import re
from pathlib import Path


def decode(text):
    """Decode one complete dump, ignoring unrelated console lines; fail closed."""
    header = None
    data = bytearray()
    complete = False
    for line in text.splitlines():
        if "LRIQ-BEGIN " in line:
            if header is not None:
                raise ValueError("multiple or restarted dumps")
            match = re.fullmatch(
                r"LRIQ-BEGIN bytes=(\d+) rate=(\d+) freq=(\d+) format=IQ16LE",
                line[line.index("LRIQ-BEGIN "):])
            if not match:
                raise ValueError("invalid header")
            header = dict(zip(("bytes", "rate", "freq"), map(int, match.groups())))
            if not 0 < header["bytes"] <= 131068 or header["bytes"] % 4 or header["rate"] != 125000:
                raise ValueError("invalid IQ dimensions")
        elif "LRIQ-DATA " in line:
            if header is None or complete:
                raise ValueError("data outside dump")
            payload = line[line.index("LRIQ-DATA ") + 10:]
            chunk = base64.b64decode(payload, validate=True)
            if not 0 < len(chunk) <= 48 or len(data) + len(chunk) > header["bytes"]:
                raise ValueError("invalid data length")
            data.extend(chunk)
        elif "LRIQ-END " in line:
            match = re.fullmatch(r"LRIQ-END crc32=([0-9A-Fa-f]{8})", line[line.index("LRIQ-END "):])
            if header is None or complete or not match:
                raise ValueError("invalid trailer")
            if len(data) != header["bytes"] or binascii.crc32(data) != int(match[1], 16):
                raise ValueError("length or CRC-32 mismatch")
            complete = True
    if not complete:
        raise ValueError("incomplete dump (missing CRC-32 trailer)")
    return header, bytes(data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    metadata, raw = decode(args.log.read_text(errors="replace"))
    args.output.write_bytes(raw)
    print(metadata)
