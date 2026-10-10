#!/usr/bin/env python3
"""LakeShark Wi-Fi over USB serial (Python 3; pip install pyserial).

  python bench/lswifi.py --board t-display-p4 status
  python bench/lswifi.py --port COM7 add "My Net"       # secret prompt
  python bench/lswifi.py --port COM7 join "Guest" --open
  python bench/lswifi.py --port COM7 forget all

Run on the PC holding the USB board, including over SSH. Close other serial
monitors first. Passwords are prompted, never accepted as command arguments.
"""

import argparse
import getpass
import json
from pathlib import Path
import re
import sys
import time

PROMPT = b"lakeshark>"
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def quote(value):
    """esp_console_split_argv double quotes with escaped slash/quote."""
    if any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError("SSID/passphrase cannot contain control characters")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def find_port(config_path, board, explicit, ports):
    if explicit:
        return explicit
    configs = json.loads(config_path.read_text(encoding="utf-8"))["configs"]
    if board and not any(c["name"] == board for c in configs):
        raise ValueError(f"Unknown board config: {board}")
    configs = [c for c in configs if (not board or c["name"] == board) and c.get("usb_serial")]
    matches = [(p, c["name"]) for p in ports for c in configs
               if p.serial_number and p.serial_number.casefold() == c["usb_serial"].casefold()]
    devices = sorted({p.device for p, _ in matches})
    if len(devices) == 1:
        return devices[0]
    if matches:
        listing = "\n".join(f"  {p.device}: {name} (USB {p.serial_number})" for p, name in matches)
        raise ValueError("Several boards match; specify --port:\n" + listing)
    raise ValueError("No configured USB serial matches; set usb_serial in bench/configs.json or use --port")


def read_until(port, marker, timeout, require_result=False):
    data = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = port.read(max(1, port.in_waiting))
        if chunk:
            data.extend(chunk)
            # A network name or the echoed command may itself contain the
            # prompt text. For replies, require the handler's result first.
            result = re.search(rb"(?:^|\n)wifi: ESP_[A-Z0-9_]+\r?\n", data)
            if marker in data and (not require_result or
                                  (result and marker in data[result.end():])):
                return bytes(data)
            if len(data) > 1024 * 1024:
                raise ValueError("Serial reply exceeded limit")
    raise TimeoutError("Timed out waiting for LakeShark console")


def exchange(port, command, secret, timeout):
    port.reset_input_buffer()
    port.write(b"\r\n")
    read_until(port, PROMPT, timeout)
    port.reset_input_buffer()
    port.write(command.encode("utf-8") + b"\r\n")
    # Everything before the handler's marker is echo, including wrapped or
    # ANSI-redrawn input. Never expose it, even on timeout or parse failure.
    raw = read_until(port, PROMPT, timeout, require_result=True)
    reply = ANSI.sub("", raw.decode("utf-8", errors="replace")).replace("\r", "")
    lines = reply.splitlines()
    begin = next((i for i, line in enumerate(lines) if line.strip() == "wifi: begin"), None)
    if begin is None:
        raise ValueError("No Wi-Fi reply marker; check that the board has the wifi console command")
    result = None
    output = []
    for line in lines[begin + 1:]:
        if line.startswith("wifi: ESP_"):
            result = line.strip().removeprefix("wifi: ")
            break
        if line.strip() == command or not line.strip():
            continue
        # Board log lines ("I (1234) tag: ...") interleave with the reply.
        if re.match(r"^[IWEDV] \(\d+\) ", line):
            continue
        if secret:
            line = line.replace(secret, "[redacted]").replace(quote(secret), "[redacted]")
        output.append(line)
    if result is None:
        raise ValueError("Incomplete Wi-Fi reply")
    for line in output:
        print(line)
    if result != "ESP_OK":
        print(result, file=sys.stderr)
        return 1
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", help="Explicit serial port (e.g. COM7 or /dev/ttyACM0)")
    parser.add_argument("--board", help="Name in bench/configs.json")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=45, help="Console/scan timeout in seconds")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("status", "list", "scan", "connect", "leave"):
        sub.add_parser(name)
    for name in ("add", "join"):
        child = sub.add_parser(name)
        child.add_argument("ssid")
        child.add_argument("--open", action="store_true", help="Open network; no passphrase prompt")
    sub.add_parser("forget").add_argument("ssid", help="SSID or all")
    args = parser.parse_args(argv)
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print("pyserial is required: python -m pip install pyserial", file=sys.stderr)
        return 1
    secret = ""
    try:
        device = find_port(Path(__file__).with_name("configs.json"), args.board, args.port,
                           list_ports.comports())
        command = "wifi " + args.command
        if hasattr(args, "ssid"):
            if args.command != "forget" and not 1 <= len(args.ssid.encode("utf-8")) <= 32:
                raise ValueError("SSID must be 1-32 bytes")
            command += " " + quote(args.ssid)
        if args.command in ("add", "join"):
            if not args.open:
                secret = getpass.getpass("Wi-Fi passphrase: ")
                if not 8 <= len(secret.encode("utf-8")) <= 63:
                    raise ValueError("Passphrase must be 8-63 bytes; use --open for an open network")
            command += " " + quote(secret)
        # Construct unopened: the default Serial(port) opens and asserts DTR.
        with serial.Serial(port=None, baudrate=args.baud, timeout=0.1,
                           write_timeout=5) as port:
            port.dtr = False
            port.rts = False
            port.port = device
            port.open()
            return exchange(port, command, secret, args.timeout)
    except (ValueError, TimeoutError, OSError, serial.SerialException) as exc:
        # Do not include raw replies/commands in exceptions or diagnostics.
        message = str(exc)
        if secret:
            message = message.replace(secret, "[redacted]")
        print(message, file=sys.stderr)
        return 1
    finally:
        # Python immutable strings cannot be securely wiped. Drop references;
        # firmware wipes its writable credential buffers after using them.
        secret = ""


if __name__ == "__main__":
    sys.exit(main())
