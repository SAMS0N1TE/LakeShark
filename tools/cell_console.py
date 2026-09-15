"""Small P4 console client. One connection per batch; never toggles reset lines.

Commands execute sequentially and require a returned prompt. Downloads verify
length, offset and CRC before publishing the destination. No implicit flashing.
"""
import argparse
import base64
import json
import re
import time
import zlib
from pathlib import Path
import serial


class Console:
    def __init__(self, port):
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = 115200
        self.serial.timeout = 0.15
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.serial.write(b"\r\n")
        self._read(12)

    def _read(self, timeout):
        data = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            chunk = self.serial.read(max(1, self.serial.in_waiting))
            data.extend(chunk)
            if b"lakeshark>" in data and not chunk:
                return data.decode(errors="replace").replace("\r", "")
        raise TimeoutError(bytes(data[-1500:]).decode(errors="replace"))

    def command(self, command, timeout=20):
        if "\n" in command or "\r" in command or len(command) > 124:
            raise ValueError("One console command, at most 124 characters")
        self.serial.reset_input_buffer()
        self.serial.write(command.encode() + b"\r\n")
        return self._read(timeout)

    def close(self):
        self.serial.close()

    def download(self, name, destination):
        response = self.command('celliq read ' + name, timeout=900)
        data = bytearray()
        begin = re.search(r'CFILE BEGIN (\d+)', response)
        end = re.search(r'CFILE END (\d+) ([0-9a-f]{8})', response)
        if not begin or not end:
            raise ValueError('Missing file transfer boundary: ' + response[-300:])
        for offset, hexdata in re.findall(r'^CF (\d+) ([0-9a-f]+)$', response, re.M):
            if int(offset) != len(data):
                raise ValueError('Transfer offset mismatch')
            data.extend(bytes.fromhex(hexdata))
        # esp_rom_crc32_le complements on entry/exit, like zlib's running API.
        crc = zlib.crc32(data)
        if len(data) != int(begin[1]) or len(data) != int(end[1]) or crc != int(end[2], 16):
            raise ValueError('File length/CRC mismatch')
        destination = Path(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_name(destination.name + '.partial')
        temporary.write_bytes(data)
        temporary.replace(destination)
        return data

    def screenshot(self, destination):
        response=self.command('tui png cellwatch', timeout=180)
        end=re.search(r'tui: png end cellwatch (\d+) bytes crc ([0-9a-f]{8})',response)
        data=b''.join(base64.b64decode(line,validate=True)
                      for line in re.findall(r'^~([A-Za-z0-9+/=]+)$',response,re.M))
        if not end or len(data)!=int(end[1]) or zlib.crc32(data)!=int(end[2],16):
            raise ValueError('Screenshot length/CRC mismatch')
        destination=Path(destination)
        destination.parent.mkdir(parents=True,exist_ok=True)
        destination.write_bytes(data)
        return len(data)

    def check_file(self, name, expected_bytes, expected_crc):
        """Read back an SD file on the P4 without sending its raw IQ over UART."""
        response = self.command('celliq check ' + name, timeout=60)
        begin = re.search(r'CFILE BEGIN (\d+)', response)
        end = re.search(r'CFILE END (\d+) ([0-9a-f]{8})', response)
        if (not begin or not end or int(begin[1]) != expected_bytes or
                int(end[1]) != expected_bytes or int(end[2], 16) != int(expected_crc, 16)):
            raise ValueError('SD readback length/CRC mismatch: ' + response[-300:])
        return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("commands", nargs="+")
    args = parser.parse_args()
    console = Console(args.port)
    records = []
    try:
        for cmd in args.commands:
            result = console.command(cmd, args.timeout)
            records.append({"command": cmd, "output": result})
            print(result, flush=True)
    finally:
        console.close()
        if args.log:
            args.log.parent.mkdir(parents=True, exist_ok=True)
            args.log.write_text(json.dumps(records, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
