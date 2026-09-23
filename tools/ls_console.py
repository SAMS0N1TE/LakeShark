#!/usr/bin/env python3
"""Drive the board's console over USB serial.

Run it with the IDF virtualenv interpreter, which carries pyserial:

    ~/.espressif/python_env/idf5.4_py3.12_env/bin/python tools/ls_console.py ...

Three properties of this console shape the tool. Replies lag by one
command, so a batch sends a throwaway first and attributes each reply to
the command before it. Opening the port can reset the board, so DTR and
RTS are forced low before open() and --reset asks for a reset explicitly.
Ports move between boards, so the board is found by serial number.

    ls_console.py --reset --boot            # boot log and a health summary
    ls_console.py "scan status" "spec"      # commands against a running board
    ls_console.py --serial 5B90159305 crash # a different board
"""
import argparse
import base64
import glob
import os
import re
import sys
import time
import zlib

try:
    import serial
except ImportError:  # pragma: no cover - the message is the whole point
    sys.exit("pyserial is missing. Run this with\n"
             "  ~/.espressif/python_env/idf5.4_py3.12_env/bin/python "
             + os.path.basename(__file__))

# The T-Display-P4 on this bench. The 4.3in LCD GUI board is 5B90159305 and
# the third P4 is 5B61091177.
DEFAULT_SERIAL = "5C84301528"

PROMPT = "lakeshark>"

# A healthy boot says both of these. The dongle drops off the host port
# intermittently and there is no software VBUS control on this board.
RTL_TUNER_LINE = "Found Rafael Micro R820T tuner"
RTL_STREAM_RATE = 524288


def _ports():
    """(serial number, device) for every port, on either host.

    pyserial reports the USB serial number on Windows and on Linux; the
    by-id symlinks are the Linux fallback for when it does not."""
    try:
        from serial.tools import list_ports
        found = [(p.serial_number or "?", p.device) for p in list_ports.comports()]
        if any(s != "?" for s, _ in found):
            return found
    except ImportError:
        pass
    out = []
    for path in glob.glob("/dev/serial/by-id/*"):
        m = re.search(r"_([0-9A-Za-z]+)-if", os.path.basename(path))
        out.append((m.group(1) if m else "?", os.path.realpath(path)))
    return out


def find_port(serial_number):
    """The device path for the board with this serial number, or None."""
    for found, device in _ports():
        if serial_number in found:
            return device
    return None


def list_boards():
    return sorted(_ports())


class Console:
    def __init__(self, port, reset=False, baudrate=115200):
        self.port = port
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = baudrate
        self.serial.timeout = 0.1
        # Low before open: on a CH343 the host asserting these pulses the
        # board's reset, which would clear the state being read.
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        # A screenshot arrives as one uninterrupted 45 KB burst, which
        # overruns the default driver buffer and loses whole lines.
        try:
            self.serial.set_buffer_size(rx_size=1 << 20, tx_size=4096)
        except (AttributeError, NotImplementedError):
            pass
        if reset:
            self.reset()

    def reset(self):
        """Pulse the board, the way esptool's ClassicReset does."""
        self.serial.dtr = False
        self.serial.rts = True
        time.sleep(0.1)
        self.serial.rts = False
        time.sleep(0.05)

    def read_for(self, seconds, until=None):
        """Read for `seconds`, stopping early on `until` if it appears."""
        data = bytearray()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            chunk = self.serial.read(max(1, self.serial.in_waiting))
            if chunk:
                data.extend(chunk)
                if until and until.encode() in data:
                    break
            else:
                time.sleep(0.02)
        return data.decode(errors="replace").replace("\r", "")

    def send(self, command):
        if "\n" in command or "\r" in command:
            raise ValueError("one console command per call")
        self.serial.write(command.encode() + b"\r\n")

    def wait_idle(self, quiet=2.5, max_wait=45.0):
        """Drain until the board stops talking.

        The first thing on the wire after an open is the boot log: about
        seven seconds, or twenty-three while an empty storage partition
        formats. Silence is the test that it has finished, since the right
        delay is not a constant.
        """
        start = last = time.monotonic()
        data = bytearray()
        while time.monotonic() - start < max_wait:
            n = self.serial.in_waiting
            if n:
                data.extend(self.serial.read(n))
                last = time.monotonic()
            else:
                time.sleep(0.05)
            if time.monotonic() - last > quiet and time.monotonic() - start > 1.0:
                break
        return data.decode(errors="replace").replace("\r", "")

    def batch(self, commands, settle=1.5, first_settle=2.5):
        """Run commands, returning [(command, reply)].

        The lag is handled here: a bare newline goes first and its reply is
        dropped, each reply is collected after the following command has
        been sent, and a trailing newline flushes the last one.
        """
        self.wait_idle()
        self.serial.reset_input_buffer()
        self.send("")
        self.read_for(first_settle)

        replies = []
        for command in commands:
            self.send(command)
            replies.append(self.read_for(settle, until=PROMPT))
        # Flush the last reply out from behind one more prompt.
        self.send("")
        tail = self.read_for(settle, until=PROMPT)
        if replies:
            replies[-1] += tail
        return list(zip(commands, [clean(r) for r in replies]))

    def capture_png(self, path, name="shot", timeout=180.0):
        """Run 'tui png' and write the framebuffer to `path`.

        The board frames the image: 'png begin', then every data line
        prefixed with '~' so a log line from another task landing in the
        middle is not mistaken for image data, then 'png end' with the byte
        count and CRC-32. Both are checked before anything is written.
        """
        self.wait_idle()
        self.serial.reset_input_buffer()
        self.send("tui png %s" % name)

        b64, size, crc, started = [], None, None, False
        deadline = time.monotonic() + timeout
        pending = ""
        while time.monotonic() < deadline:
            chunk = self.serial.read(max(1, self.serial.in_waiting))
            if not chunk:
                time.sleep(0.02)
                continue
            pending += chunk.decode(errors="replace").replace("\r", "")
            while "\n" in pending:
                line, pending = pending.split("\n", 1)
                line = line.replace(PROMPT, "").strip()
                if line.startswith("~"):
                    b64.append(line[1:])
                elif "png begin" in line:
                    started = True
                elif "png end" in line:
                    m = re.search(r"(\d+) bytes crc ([0-9a-fA-F]+)", line)
                    if m:
                        size, crc = int(m.group(1)), int(m.group(2), 16)
                    deadline = 0
                    break
            if deadline == 0:
                break

        if not started:
            raise RuntimeError("the board never started an image - is the TUI running?")
        if not size:
            raise RuntimeError("the encoder did not finish")

        raw = base64.b64decode("".join(b64))
        if len(raw) != size:
            raise RuntimeError("image is %d bytes, the board sent %d"
                               % (len(raw), size))
        got = zlib.crc32(raw) & 0xFFFFFFFF
        if got != crc:
            raise RuntimeError("CRC-32 %08x does not match the board's %08x"
                               % (got, crc))
        with open(path, "wb") as fh:
            fh.write(raw)
        return size, crc

    def close(self):
        self.serial.close()


def clean(text):
    """Drop prompts and the echoed command, keep everything else."""
    lines = []
    for line in text.split("\n"):
        if line.strip() in ("", PROMPT):
            continue
        lines.append(line.replace(PROMPT, "").rstrip())
    return "\n".join(l for l in lines if l.strip())


def boot_health(log, we_reset=False):
    """What the boot log says about the parts that read as working.

    Returns a list of (ok, note).
    """
    out = []
    out.append((RTL_TUNER_LINE in log,
                "RTL tuner: " + ("R820T found" if RTL_TUNER_LINE in log
                                 else "ABSENT - every sweep will measure "
                                      "nothing; needs a physical reseat")))
    # HIGH speed carries 960 kSPS; FULL does not, and is reached with the
    # tuner found, so the tuner line alone is not enough.
    m = re.search(r"USB negotiated speed: (\w+)\(", log)
    if m:
        out.append((m.group(1) == "HIGH",
                    "RTL link speed: %s" % m.group(1) +
                    ("" if m.group(1) == "HIGH"
                     else "  <- need HIGH(480M) for 960 kSPS; reseat it")))

    m = re.search(r"(\d+)\s*B/s", log)
    if m:
        rate = int(m.group(1))
        out.append((rate >= RTL_STREAM_RATE,
                    "RTL stream: %d B/s (want %d)" % (rate, RTL_STREAM_RATE)))
    else:
        # Nothing streams at boot, so this is a note rather than a warning.
        out.append((True, "RTL stream: no throughput line (nothing streaming "
                          "yet, which is normal at boot)"))

    m = re.search(r"rst:0x([0-9a-f]+)\s*\(([^)]*)\)", log)
    if m:
        cause = m.group(2)
        poweron = "POWERON" in cause.upper()
        # A POWERON with no coredump is a rail collapse rather than a
        # crash, and the keyboard's 5V output feeding back into the P4
        # produces it. esptool and --reset land as POWERON too, so it is
        # only reported when we did not ask for the reset.
        out.append((not poweron or we_reset,
                    "reset cause: " + cause +
                    ("" if not poweron else
                     "  (asked for)" if we_reset else
                     "  <- unasked-for POWERON: a rail collapse, not a "
                     "crash; is the keyboard plugged in?")))
    version = re.search(r"2\.\d+\.\d+[-\w]*", log)
    if version:
        out.append((True, "firmware: " + version.group(0)))
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("commands", nargs="*", help="console commands to run")
    p.add_argument("--serial", default=DEFAULT_SERIAL,
                   help="board serial number (default: %(default)s)")
    p.add_argument("--port", help="device path, overriding --serial")
    p.add_argument("--reset", action="store_true",
                   help="pulse reset on connect (a boot capture wants this)")
    p.add_argument("--boot", action="store_true",
                   help="capture and summarise the boot log")
    p.add_argument("--boot-seconds", type=float, default=30.0,
                   help="how long to capture for (default: %(default)s; a "
                        "blank-storage first boot takes about 23 s, versus "
                        "about 7 normally)")
    p.add_argument("--settle", type=float, default=1.5,
                   help="seconds to read after each command")
    p.add_argument("--raw", action="store_true", help="do not tidy replies")
    p.add_argument("--list", action="store_true",
                   help="list attached boards by serial number and exit")
    p.add_argument("--png", metavar="FILE",
                   help="save the panel's own pixels to FILE as a PNG")
    args = p.parse_args(argv)

    if args.list:
        for sn, path in list_boards():
            print("%-14s %s" % (sn, path))
        return 0

    port = args.port or find_port(args.serial)
    if not port:
        print("No board with serial %s. Attached:" % args.serial,
              file=sys.stderr)
        for sn, path in list_boards():
            print("  %-14s %s" % (sn, path), file=sys.stderr)
        return 2

    # A browser tab on the flasher page holds the port.
    try:
        console = Console(port, reset=args.reset or args.boot)
    except serial.SerialException as e:
        print("Could not open %s: %s\nCheck `fuser -v %s` - a browser tab on "
              "the flasher page holds it." % (port, e, port), file=sys.stderr)
        return 2

    rc = 0
    try:
        if args.boot:
            # Reads the full duration: the console comes up around 6.8 s,
            # before the UI registers its apps and takes the panel.
            log = console.read_for(args.boot_seconds)
            print(log if args.raw else log.strip())
            print("\n---- boot health " + "-" * 40)
            for ok, note in boot_health(log, we_reset=True):
                print(("  ok   " if ok else "  WARN ") + note)
                if not ok:
                    rc = 1
        if args.png:
            size, crc = console.capture_png(args.png)
            print("%s  %d bytes  crc %08x" % (args.png, size, crc))
        if args.commands:
            if not args.boot and not args.png:
                boot = console.wait_idle()
                if boot.strip():
                    print("---- (board was booting; %d bytes of log drained)"
                          % len(boot))
            for command, reply in console.batch(args.commands,
                                                settle=args.settle):
                print("---- %s" % command)
                print(reply if reply.strip() else "(no reply)")
    finally:
        console.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
