"""Radios somewhere else, driven from here.

Run `serve` on any small computer beside the radios (a Raspberry Pi, an old
laptop) with Tailscale on it. Each radio's USB serial port is offered on the
network as RFC 2217, which pyserial and esptool open like a local port, and
an RTL-SDR is offered through rtl_tcp for SDR programs on this side.

    python tools/ls_remote.py serve                     # this computer only
    python tools/ls_remote.py serve --bind tailscale    # reachable over the tailnet
    python tools/ls_remote.py serve --port COM5         # a named port as well
    python tools/ls_remote.py check HOST           # from here: what answers

Then, from this machine:

    python tools/ls_console.py --port rfc2217://HOST:4001 find
    python -m esptool -p "rfc2217://HOST:4001?ign_set_control" ...
    rtl_tcp in SDRangel / SDR++ / GQRX:  HOST:1234

Only ports whose USB identity is a known radio are offered unless a port is
named: another board on the same computer (a live gateway, say) is never
opened. DTR and RTS are held low from before the port opens, so offering a
T-Display-P4 does not reset it. Nothing here authenticates, so it listens on
this computer only (127.0.0.1) unless --bind names an address: anything that
can reach that address can drive the radios. `--bind tailscale` uses this
computer's own Tailscale address, which every device on the tailnet can reach.
"""

import argparse
import shutil
import socket
import subprocess
import sys
import threading
import time

import serial
import serial.rfc2217
from serial.tools import list_ports

# USB identities offered without being named.
KNOWN = {
    (0x1A86, 0x55D3): "T-Display-P4 console (CH343)",
    (0x0483, 0x5740): "Flipper Zero CLI",
}
FIRST_PORT = 4001
RTL_PORT = 1234


def tailscale_address():
    try:
        out = subprocess.run(["tailscale", "ip", "-4"], capture_output=True, text=True, timeout=5)
        ip = out.stdout.strip().splitlines()[0] if out.returncode == 0 and out.stdout.strip() else ""
        return ip or None
    except (OSError, subprocess.SubprocessError, IndexError):
        return None


def radios(named):
    found = []
    for p in list_ports.comports():
        label = KNOWN.get((p.vid, p.pid)) if p.vid is not None else None
        if p.device in named:
            label = label or p.description or "named port"
        if label:
            found.append((p.device, label, p.serial_number or "?"))
    for n in named:
        if not any(d == n for d, _, _ in found):
            found.append((n, "named port (not seen yet)", "?"))
    return found


class Bridge(threading.Thread):
    """One serial port, one TCP listener, one client at a time."""

    def __init__(self, device, label, bind, port):
        super().__init__(daemon=True)
        self.device, self.label, self.bind, self.port = device, label, bind, port

    def open_serial(self):
        s = serial.Serial()
        s.port = self.device
        s.baudrate = 115200
        s.timeout = 0.05
        s.dtr = False          # low before open: a CH343 asserting them resets the board
        s.rts = False
        s.open()
        return s

    def run(self):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((self.bind, self.port))
        srv.listen(1)
        while True:
            client, addr = srv.accept()
            client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print(f"[{self.device}] {addr[0]} connected", flush=True)
            try:
                self.session(client)
            except Exception as e:          # one bad session must not end the listener
                print(f"[{self.device}] session ended: {e}", flush=True)
            client.close()
            print(f"[{self.device}] {addr[0]} left", flush=True)

    def session(self, client):
        ser = self.open_serial()
        lock = threading.Lock()

        class Link:
            """PortManager answers through write(); the socket is shared with the reader."""
            def write(self, data):
                with lock:
                    client.sendall(data)

        link = Link()
        manager = serial.rfc2217.PortManager(ser, link)
        alive = [True]

        def to_client():
            while alive[0]:
                try:
                    data = ser.read(ser.in_waiting or 1)
                    if data:
                        link.write(b"".join(manager.escape(data)))
                except (OSError, serial.SerialException):
                    break
            alive[0] = False

        reader = threading.Thread(target=to_client, daemon=True)
        reader.start()
        try:
            while alive[0]:
                data = client.recv(1024)
                if not data:
                    break
                ser.write(b"".join(manager.filter(data)))
        except (OSError, serial.SerialException):
            pass
        alive[0] = False
        reader.join(1)
        ser.close()


def serve(args):
    bind = args.bind or "127.0.0.1"
    if bind == "tailscale":
        bind = tailscale_address()
        if not bind:
            print("No Tailscale address on this computer.")
            return 1
    found = radios(args.port or [])
    if not found:
        print("No known radio is plugged in. Name one with --port.")
    bridges = []
    for i, (device, label, sn) in enumerate(found):
        b = Bridge(device, label, bind, FIRST_PORT + i)
        b.start()
        bridges.append(b)
        print(f"rfc2217://{bind}:{b.port}  {device}  {label}  serial {sn}")
    rtl = None
    if not args.no_rtl and shutil.which("rtl_tcp"):
        rtl = subprocess.Popen(["rtl_tcp", "-a", bind, "-p", str(RTL_PORT)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        print(f"rtl_tcp            {bind}:{RTL_PORT}  (an RTL-SDR, if one is plugged in)")
    elif not args.no_rtl:
        print("rtl_tcp is not installed: RTL-SDR not offered")
    if bind == "127.0.0.1":
        print("Listening on this computer only. --bind tailscale (or an address) to reach it from elsewhere.")
    else:
        print(f"Anything that can reach {bind} can drive these radios. Ctrl-C stops it.")
    try:
        while True:
            time.sleep(1)
            if rtl and rtl.poll() is not None:
                rtl = subprocess.Popen(["rtl_tcp", "-a", bind, "-p", str(RTL_PORT)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    except KeyboardInterrupt:
        if rtl:
            rtl.terminate()


def check(args):
    for i in range(args.count):
        port = FIRST_PORT + i
        url = f"rfc2217://{args.host}:{port}"
        try:
            s = serial.serial_for_url(url, do_not_open=True)
            s.timeout = 0.3
            s.dtr = False
            s.rts = False
            s.open()
            s.write(b"\r\n")
            time.sleep(0.8)
            reply = s.read(4096).decode("utf-8", "replace").strip().splitlines()
            s.close()
            print(f"{url}  answers: {reply[-1] if reply else '(silent)'}")
        except (serial.SerialException, OSError) as e:
            print(f"{url}  no: {e}")
    try:
        socket.create_connection((args.host, RTL_PORT), timeout=3).close()
        print(f"rtl_tcp {args.host}:{RTL_PORT}  open")
    except OSError:
        print(f"rtl_tcp {args.host}:{RTL_PORT}  closed")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve", help="offer this computer's radios on the network")
    s.add_argument("--port", action="append", help="also offer this serial port (repeatable)")
    s.add_argument("--bind", help="address to listen on: 127.0.0.1 (default), 'tailscale', or an address")
    s.add_argument("--no-rtl", action="store_true", help="do not start rtl_tcp")
    c = sub.add_parser("check", help="see what a serving computer answers")
    c.add_argument("host")
    c.add_argument("--count", type=int, default=2, help="how many serial ports to try")
    args = p.parse_args()
    return serve(args) if args.cmd == "serve" else check(args)


if __name__ == "__main__":
    sys.exit(main())
