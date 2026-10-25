"""LakeShark on the desktop: the board plugged in over USB, in a window of its own.

    pythonw lsdesk.py            (the shortcut runs this; no console)
    python lsdesk.py --install   puts a LakeShark shortcut on the desktop and in the Start menu

One click finds the board, starts the screen bridge (lsweb.py) on 127.0.0.1 and
opens it as an app window (Edge or Chrome, a profile of its own). Unplug the
board and the window says so and waits; plug it back in, any USB port, and it
carries on. Closing the window stops the bridge. A second click while it runs
opens another window on the same board.

Which board: the one it used last (~/.lsdesk.json remembers its USB serial
number), else the only CH343 board plugged in, else the one that answers as a
Nano. Ports listed under "skip" in that file are never opened.
"""
import json
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lsweb  # noqa: E402

CONFIG = os.path.join(os.path.expanduser("~"), ".lsdesk.json")
HOME = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "LakeShark")
PORT = 8770
CH343 = 0x1A86


def log(*a):
    os.makedirs(HOME, exist_ok=True)
    with open(os.path.join(HOME, "lsdesk.log"), "a", encoding="utf-8") as f:
        f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + " ".join(str(x) for x in a) + "\n")


def load():
    try:
        return json.load(open(CONFIG, encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save(cfg):
    try:
        json.dump(cfg, open(CONFIG, "w", encoding="utf-8"), indent=2)
    except OSError:
        pass


def answers_as(device):
    """The board= word of a board's 'version', asked without resetting it."""
    import serial
    try:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = device, 115200, 0.2
        s.dtr = s.rts = False
        s.open()
        s.read(1 << 16)
        s.write(b"\rversion\r")
        got, t = b"", time.time()
        while time.time() - t < 1.5 and b"board=" not in got:
            got += s.read(4096)
        s.close()
        i = got.find(b"board=")
        return got[i + 6:].split()[0].decode(errors="replace") if i >= 0 else None
    except Exception:
        return None


def finder(cfg):
    """find() for the bridge: the remembered board, else the likeliest one plugged in."""
    asked = {}

    def find():
        import serial.tools.list_ports
        skip = set(x.upper() for x in cfg.get("skip", []))
        ports = [p for p in serial.tools.list_ports.comports()
                 if p.vid == CH343 and p.device.upper() not in skip and (p.serial_number or "").upper() not in skip]
        want = cfg.get("board")
        for p in ports:
            if want and p.serial_number == want:
                return p.device
        if not ports:
            return None
        pick = ports[0] if len(ports) == 1 else None
        if not pick:                             # several: the one that says it is a Nano
            for p in ports:
                if p.serial_number not in asked:
                    asked[p.serial_number] = answers_as(p.device)
                if (asked[p.serial_number] or "").startswith("ESP32-P4-NANO"):
                    pick = p
                    break
        if not pick:
            return None
        if pick.serial_number and pick.serial_number != want:
            cfg["board"] = pick.serial_number
            save(cfg)
        return pick.device
    return find


def browser():
    """Edge or Chrome, for a window with no tabs or address bar."""
    pf = [os.environ.get(k, "") for k in ("ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA")]
    for base in pf:
        for rel in (r"Microsoft\Edge\Application\msedge.exe", r"Google\Chrome\Application\chrome.exe"):
            path = os.path.join(base, rel)
            if base and os.path.isfile(path):
                return path
    return None


def open_window(url):
    """The app window; returns its process, or None when the default browser had to do."""
    exe = browser()
    if not exe:
        import webbrowser
        webbrowser.open(url)
        return None
    profile = os.path.join(HOME, "window")
    return subprocess.Popen([exe, "--app=" + url, "--user-data-dir=" + profile, "--no-first-run",
                             "--no-default-browser-check", "--window-size=1500,860", "--disable-features=Translate"])


def running(port):
    with socket.socket() as s:
        s.settimeout(0.3)
        return s.connect_ex(("127.0.0.1", port)) == 0


def install():
    """Desktop and Start-menu shortcuts that run this with pythonw."""
    pyw = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    if not os.path.isfile(pyw):
        pyw = sys.executable
    icon = os.path.join(HOME, "lakeshark.ico")
    os.makedirs(HOME, exist_ok=True)
    png = open(os.path.join(lsweb.WEB, "icon-192.png"), "rb").read()
    # an .ico that carries the PNG as it is (Vista and later read it)
    head = bytes([0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 32, 0]) + len(png).to_bytes(4, "little") + (22).to_bytes(4, "little")
    open(icon, "wb").write(head + png)
    places = [os.path.join(os.path.expanduser("~"), "Desktop"),
              os.path.join(os.environ.get("APPDATA", ""), r"Microsoft\Windows\Start Menu\Programs")]
    for place in places:
        if not os.path.isdir(place):
            continue
        lnk = os.path.join(place, "LakeShark.lnk")
        ps = ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut('%s');$s.TargetPath='%s';"
              "$s.Arguments='\"%s\"';$s.WorkingDirectory='%s';$s.IconLocation='%s';"
              "$s.Description='LakeShark on the desktop';$s.Save()") % (lnk, pyw, os.path.abspath(__file__), HERE, icon)
        subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=True)
        print("shortcut:", lnk)


def main():
    if "--install" in sys.argv:
        install()
        return
    cfg = load()
    port = int(cfg.get("port", PORT))
    url = "http://127.0.0.1:%d/" % port
    if running(port):                            # already up: just another window on it
        open_window(url)
        return
    server = lsweb.serve(finder(cfg), port, int(cfg.get("baud", 2000000)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    log("bridge on", url)
    open_window(url)
    # The browser's first process often hands over to another and leaves, so
    # the window is watched through its page instead: up once it connects, and
    # gone when nothing has connected for a while. A page whose board is
    # unplugged keeps trying every few seconds, which keeps this running.
    start = time.time()
    while lsweb.CLIENTS == 0 and time.time() - start < 90:
        time.sleep(0.5)
    while lsweb.CLIENTS > 0 or time.time() - lsweb.LAST_SEEN < 12:
        time.sleep(1)
    log("window closed")
    if lsweb.BOARD and lsweb.BOARD.up.is_set():
        lsweb.BOARD.stop()
    os._exit(0)                                  # the bridge's threads hold the port: leave at once


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        log("failed:", repr(e))
        raise
